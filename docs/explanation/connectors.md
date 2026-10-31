# Connectors

lrvx ships native exchange connectors as part of the same repo. They live under [`connectors/`](https://github.com/lrvx/lrvx/tree/main/connectors) and build into a single `lrvx::connectors` static library that links against `lrvx::lrvx`.

The module is gated by `LRVX_BUILD_CONNECTORS` in CMake. It is **off** by default — backtest-only and research builds skip the dependency cost (OpenSSL, libcurl, zlib, plus ixwebsocket and simdjson via FetchContent at configure time).

## Adapters in tree

| Venue | Trades | BBO | Book | Orders |
|---|---|---|---|---|
| Bybit (V5) | ✓ | ✓ | ✓ | ✓ |
| Bitget (V2) | ✓ | ✓ | ✓ | ✓ |
| Hyperliquid | ✓ | ✓ | ✓ | ✓ |
| Polymarket | ✓ | ✓ | — | ✓ |

(No position query/stream is implemented in any connector yet — the earlier "Positions" column was aspirational.)

Each adapter sits under `connectors/src/<venue>/` with public headers under `connectors/include/lrvx-connectors/<venue>/`. The header layout uses the `lrvx-connectors/` prefix so consumers' include sites are stable across the repo move.

### Order-type and flag coverage

The "Orders" column above means "submit/cancel/replace exists," not "every `OrderType` and flag is supported." Coverage differs per venue and is enforced at submit time — an order the connector cannot serialize correctly is rejected rather than sent as an approximation:

- `TimeInForce` and reduce-only are serialized on all four venues. Post-only maps to the venue's maker-only token (Bybit `PostOnly`, Bitget `post_only`, Hyperliquid `Alo`); `GTD` has no native equivalent on any of the four and is rejected rather than silently downgraded to `GTC`.
- Stop-market, stop-limit, take-profit-market and take-profit-limit route through Bitget's plan-order endpoint, Bybit's conditional-order fields on the regular order endpoint, and Hyperliquid's `trigger` order object (see [Hyperliquid executor](#hyperliquid-executor) below).
- A conditional order carries its execution price. Bitget's plan-order endpoint takes an order type of its own, so `STOP_LIMIT` and `TAKE_PROFIT_LIMIT` are sent as plan orders of type `limit` with the `price` field the strategy asked for; the market variants are sent as `market` with no price at all. A limit-typed conditional order with no limit price is rejected rather than downgraded to a stop-market, which would silently remove the bound on what it may fill at.
- Prices are formatted at the instrument's own precision, taken from `SymbolInfo::tickSize` in the registry the executor already holds (Hyperliquid, which has no fixed tick, follows its own significant-figure rule instead; see below). Venues validate a price against the symbol's precision and reject one with more digits than the instrument quotes, and rounding to a fixed digit count instead moves a protective trigger — on a symbol priced below the rounding granularity, to zero.
- `TRAILING_STOP` and `ICEBERG` are not implemented on any of the three CEX connectors (Polymarket has no order-side concept of either) and are rejected at submit time. Bybit's trailing stop in particular lives on a different endpoint (`/v5/position/trading-stop`) than the rest of order submission, which this connector does not call.

A rejected order publishes `OrderEventStatus::REJECTED` on the venue's `OrderExecutionBus` with a reason string identifying the unsupported type or flag — it never reaches the exchange as a same-looking order with the unsupported part silently dropped.

### Hyperliquid executor

`HyperliquidOrderExecutorT` sends every action to the `/exchange` endpoint as a signed L1 action.

- Limit orders use HL's `limit` order object with tif `Gtc`, `Ioc` or `Alo` (post-only). HL has no FOK or `GTD` tif, so those are rejected.
- HL has no market-order primitive. A `MARKET` order goes out as an IOC limit at `order.price`, and the caller sets that price with `marketPriceWithSlippage(reference, side, slippage)`. It moves the reference by the slippage toward the aggressive side and rounds to 5 significant figures, the same computation as `market_open` in the official SDK (whose default slippage is 0.05), except that a price of 100000 or more is rounded to an integer. A market order without a price is rejected, not sent at zero.
- `STOP_MARKET`, `STOP_LIMIT`, `TAKE_PROFIT_MARKET` and `TAKE_PROFIT_LIMIT` use HL's `trigger` order object: `order.triggerPrice` is the `triggerPx`, and `tpsl` is `sl` for the stops and `tp` for the take-profits. The limit variants carry `order.price` and are rejected without one. The market variants carry a limit price derived from the trigger, 10% to the aggressive side, which is HL's slippage for market TP/SL. An accepted trigger order is published as `SUBMITTED` and then `PENDING_TRIGGER`, and `cancelOrder` cancels it like any other order. `TRAILING_STOP` and `ICEBERG` have no HL primitive and are rejected.
- `placePosTpsl(symbol, positionSide, slTrigger, tpTrigger, qty, slLocalId, tpLocalId)` sends a position's stop-loss and take-profit as reduce-only market trigger orders in one `order` action with grouping `positionTpsl`, so the venue ties both legs to the position. A zero trigger skips that leg and the size must be positive. If the two triggers are on the wrong sides of each other, or either leg cannot be built, the whole group is rejected. Each leg is tracked and published under its own local id.
- `setLeverage(symbol, leverage, isCross, onResult)` sends the `updateLeverage` action. The callback receives the venue's answer, a transport error, or the reason the request was refused locally.
- Sizes are truncated toward zero to the asset's `szDecimals`, and an order whose size truncates to zero is rejected. Prices are rounded to 5 significant figures and at most `6 - szDecimals` decimals; an integer price is always valid.
- Asset indices and `szDecimals` come from a `meta` request to the `/info` endpoint on the same host as the executor's `/exchange` URL, so an executor on the test network reads that network's indices. The constructor waits up to 15 s for the answer. Until the map is loaded, orders, cancels, TP/SL and leverage calls are refused. Each refused call also starts a new fetch, at most once every 2 s, and does not wait for it; the answer loads the map for the calls that follow. `assetsLoaded()` says whether the map is there.
- Each action takes its nonce from `hl::nextNonceMs()`, which returns the wall clock in milliseconds bumped past the last value handed out in the process. Two actions signed in the same millisecond never share a nonce.
- With a vault address configured, every signed action (order, cancel, modify, leverage and position TP/SL) carries it in both the signature and the request body. Without one, neither does.
- `replaceOrder` sends HL's `modify` action addressed by the venue's order id, or by the cloid when the venue acknowledged the order without one. With neither known, the replacement is rejected. HL's `modify` without `always_place` takes a non-trigger order only, with tif `Alo` or a `Gtc` that would not execute, which the venue overrides to `Alo`; this connector does not send `always_place`, which would place the new order even when the cancel of the old one failed, so a trigger order is replaced by cancelling it and submitting a new one, and `replaceOrder` rejects a trigger order before anything is sent.
- Each action gets one info log line with its type, the local order ids or symbol, and the action itself. The signature is never logged.

## Threading: the transport and the rate-limit gate

Order submission is called from the strategy / event-bus consumer thread. Nothing on that path may wait for a venue, because a venue that accepts the connection and then says nothing is indistinguishable from a healthy one until a timeout fires — and while the thread waits, the engine processes no events at all, market data included.

Two components own that rule.

**`CurlTransport` is asynchronous.** `post()` copies the request, hands it to a sender thread and returns; `curl_easy_perform` and both completion callbacks run on that thread. The outcome reaches the strategy the way it always did — as an `OrderEvent` on the `OrderExecutionBus`, published from the callback — so a submit that fails on the wire still produces a `REJECTED` with the transport's reason, and nothing is dropped quietly. The defaults in `CurlDispatchConfig` are one sender thread, so requests leave in the order they were handed over (a cancel submitted after a place is sent after it), and a bounded queue: a full queue answers `onError` instead of growing, because a venue that has stopped draining is a condition the caller has to hear about rather than a buffer to absorb.

Timeouts are milliseconds throughout — `CurlTimeoutConfig` and `postWithTimeout()` map onto `CURLOPT_TIMEOUT_MS` / `CURLOPT_CONNECTTIMEOUT_MS`, so 1500 ms is 1500 ms and 250 ms is 250 ms. Second-resolution options truncated both to one second, which is the whole resolution an order path needs.

**The rate-limit policy is a gate, not a check.** Every send path of every executor — submit, cancel, replace, on Bitget also `setLeverage`, `submitOrderWithLeverage`, `placePosTpsl` and `modifyPosTpsl`, and on Hyperliquid also `setLeverage` and `placePosTpsl` — hands its request to `ActiveRateLimitPolicy::gate()`, which decides whether it may leave and when. A path that consults nothing is a path the venue budget does not cover.

- A request that takes a token is sent inline on the calling thread: unchanged ordering, unchanged latency, nothing queued.
- `RateLimitPolicy::REJECT` and `CALLBACK` refuse it and publish `REJECTED_RATE_LIMIT` on the bus, so a cancel that never left the process cannot leave the tracker reporting the order live.
- `RateLimitPolicy::WAIT` defers: the request is queued on the policy's own sender thread, `gate()` returns immediately, and the sender sends it once the bucket really has a token — re-checking the budget after every sleep, since several deferred requests wake into the same refill and only the one that takes the token may send. The deferral queue is bounded; past its depth the request is refused through the same `REJECTED_RATE_LIMIT` path as `REJECT`.

Because a deferred request runs after its entry point returned, each send path owns what it needs — it copies the order, or re-reads the tracker — instead of borrowing from the caller's frame.

**Market data arrives on the WebSocket client's own thread**, and every `onOpen`, `onMessage` and `onClose` callback runs there. `IxWebSocketClient::stop()` returns only after that thread has exited, so once a connector's `stop()` has returned none of its callbacks is running or will run again, and the connector can be destroyed straight away. The close that `stop()` itself causes reaches the connector's disconnect callback synchronously, inside `stop()` and on the socket's thread, so a caller must not hold a lock its disconnect handler takes while it calls `stop()`. From inside one of those callbacks, `stop()` only requests the stop, because the thread cannot wait for itself; the next `start()` or the destructor collects the thread, and the client may be destroyed there too. `start()` and the callback setters called from inside a callback are refused with an error log, so restarting a connector from its own disconnect handler does nothing: restart it from another thread.

## The live fill contract

Every connector that reports fills must honour all of the following. The engine has no way to detect a violation — a fill that breaks one of these rules looks exactly like a correct fill, and the damage shows up as a wrong position or a wrong PnL much later.

- **`fillQty` is the size that just traded**, not the order's size and not the cumulative filled quantity. A venue that reports cumulatively (Bybit's `cumExecQty`, Bitget's `accBaseVolume`) must be differenced against what the connector has already published. The cumulative number belongs in `order.filledQuantity`.
- **`fillPrice` is the price it traded at.** `PositionTracker::onOrderPartiallyFilled(order, fillQty, fillPrice)` builds cost basis and realized PnL from it, so an unset `fillPrice` books the position at zero.
- **No fill without a price.** A fill is published only when the venue has reported the price it traded at. `Price` has no unset state — its default and a parsed `"0"` are the same bits — so an unpriced fill and a fill that traded at zero reach a listener as the same event, and the cost basis is built at zero. Bybit's order topic reports `avgPrice` `"0"` until something trades, which makes this the everyday shape rather than an edge case. An unpriced increment is *held*, not dropped: the watermark is left where it was, so the next report that does carry a price publishes the whole quantity the venue has accumulated since. The order's own status and cumulative quantity still go out, demoted to `ACCEPTED` so nothing moves a position.
- **One event per execution.** Where a venue announces the same execution on more than one channel (Bybit's `order` and `execution` topics both report it), the connector publishes it once. Identity is the venue's own execution id or, where there is none, a per-order cumulative watermark: the first channel to report an execution advances the watermark and publishes the increment, the second computes a zero increment and publishes nothing.
- **`order.id` is the id the engine issued**, never the venue's. Executors send the engine's `OrderId` as the venue's client order id (Bybit `orderLinkId`, Bitget `clientOid`, Hyperliquid `cloid`) and the connector reads it back off every private frame. The venue's own order id is the fallback for orders this engine did not place, and it belongs in the tracker's `exchangeOrderId`, not in `OrderEvent::order.id`.
- **A venue rejection publishes `REJECTED` with the venue's own reason text** and leaves no live order behind, in the tracker or in the strategy's belief.
- **`recvNs` is stamped on receipt and `sourceExchange` names the venue** on every event. Both are covered in [Building a custom connector](../how-to/custom-connector.md): without `sourceExchange`, `CompositeBookMatrix` drops the update and the cross-venue book is empty in live; without `recvNs`, its staleness sweep skips the venue and a frozen feed keeps being quoted.

`connectors/tests/unit_test_*_fill_contract.cpp` pins this per venue, offline, by feeding recorded frames into the connector's message handlers and asserting on what a real `IOrderExecutionListener` receives from a real `OrderExecutionBus`.

## Feed health

`IExchangeConnector` carries the framework's only generic health surface —
`setErrorCallbacks(onDisconnect, onSequenceGap, onStaleData)` — and every
connector in tree honours the same contract, so a supervisor wires the three
callbacks once and hears about all four venues the same way.

| Event | Every connector raises it when |
|---|---|
| `onDisconnect` | The WebSocket closed. Delivered from the socket's own close handler through the connector's public `handleDisconnect(code, reason)`; the reason carries both the close code and the venue's text. Both the public market-data socket and, where a venue has one, the private order stream report — losing the private stream stops fills reaching the engine. |
| `onSequenceGap` | The venue's own continuity field broke, so the local book is no longer a valid continuation of the venue's: Bybit's orderbook update id `u` skipped, Bitget's `seq` skipped, or a Bitget snapshot failed its checksum. In every case the offending frame is dropped, further deltas are suppressed, and the topic is re-subscribed so the venue re-sends a snapshot — the event never replaces the invalidation, it reports it. | A checksum of 0 is the venue saying it computed none for that push, which is how the depth-limited channels carry the field; such a snapshot is accepted unverified.
| `onStaleData` | A subscribed symbol stopped ticking. This is the failure a close handler cannot catch: the socket stays open and the data stops. |

A gap event carries `(expected, received)` update ids. A Bitget checksum
failure means the same thing — the book is wrong and must be re-baselined —
and rides the same callback carrying the computed and received CRC32 values
instead; the log line at `error` level says which of the two fired.

Staleness is polled, not timed: no connector owns a timer, so the supervisor
calls `pollFeedHealth(now)` on its own cadence and each connector compares
`now` against its per-symbol last-arrival stamp. The window is
`<Config>::staleDataTimeoutMs` and defaults to **0, which disables the
check** — the right window is a property of the instrument's liquidity, not
of the venue, so there is no default the connector can pick. A symbol is
reported once per staleness episode, and fresh data re-arms it. Each
connector stamps every subscribed symbol at `start()`, so a feed that never
delivers a single frame ages out like one that stopped.

## Build

```bash
cmake -B build -DLRVX_BUILD_CONNECTORS=ON
cmake --build build --target lrvx-connectors
```

The result is `build/connectors/liblrvx-connectors.a`. To link against another target:

```cmake
target_link_libraries(my_target PRIVATE lrvx::connectors)
```

The Polymarket executor is a Rust FFI library. If `cargo` isn't on the path, the executor source is excluded from the build and a CMake warning surfaces instead of a configure error. Pass `-DLRVX_ENABLE_POLYMARKET_ORDER_EXECUTOR=OFF` to silence the warning when Rust isn't desired.

## Why monorepo

The connector code used to live in a separate `lrvx-connectors` repo, pulled in as a git submodule. Three reasons it moved:

- Cross-repo changes (a new event type in core needing a new field on the connector side) cost two PRs with submodule pinning. For a one-contributor project, the overhead is pure friction.
- AI agents reading lrvx without recursing into submodules saw zero connector code, which made the polyglot positioning weaker than it actually was.
- Single dependency policy. `connectors/CMakeLists.txt` now uses the same `FetchContent` mechanism the parent uses for `lz4` and `tracy`, instead of bundling its own external/ submodule tree.

The trade-off: every push that touches connectors runs the full lrvx CI. The `connectors-build` job stays minimal (no tests, no platform matrix beyond ubuntu) so the cost is bounded.

## Tests

`connectors/tests/` holds two kinds of test:

- `unit_test_*.cpp` are offline — no exchange endpoints and no credentials; the few that need a live session use a listener on `127.0.0.1`. Order-serialization tests fake the transport (`ITransport`) or the client-provided injection point and assert on the exact request body a connector builds; protocol tests feed raw WebSocket frames straight into a connector's message handler. They build and register with `ctest` whenever `LRVX_BUILD_CONNECTORS=ON` and `LRVX_BUILD_TESTS=ON` — no extra flag — and run in the default CI matrix.
- `integration_test_*.cpp` connect to real exchange WebSocket endpoints. Building them is gated by both `LRVX_BUILD_TESTS=ON` and `LRVX_BUILD_CONNECTOR_INTEGRATION_TESTS=ON`; running them via `ctest` requires the further `LRVX_RUN_CONNECTOR_INTEGRATION_TESTS=ON`. The default lrvx CI build skips them entirely.

To build the integration tests locally:

```bash
cmake -B build -DLRVX_BUILD_CONNECTORS=ON \
                -DLRVX_BUILD_TESTS=ON \
                -DLRVX_BUILD_CONNECTOR_INTEGRATION_TESTS=ON
cmake --build build
```

## Hyperliquid signing daemon

Hyperliquid uses an off-process Python daemon for order signing — the C++ executor talks to it over a local socket. The daemon wraps the official [`hyperliquid-python-sdk`](https://github.com/hyperliquid-dex/hyperliquid-python-sdk):

```bash
pip install git+https://github.com/hyperliquid-dex/hyperliquid-python-sdk.git
python3 connectors/utils/hl_signerd.py
```

Out-of-process signing keeps the secret out of the trading binary's address space and avoids shipping a Rust crypto stack into every lrvx build.

**Transport rule: the key travels over a Unix socket private to its owner, or it does not travel.** The signing request contains the raw private key in its body, so the transport *is* the access control:

- The client speaks `AF_UNIX` only. There is no TCP fallback — loopback authenticates neither end, and any local process that binds the port first harvests the key.
- The socket path is `LRVX_HL_SIGNER_SOCKET`, defaulting to `/dev/shm/hl_sign.sock`. Set it on any host without `/dev/shm` (macOS, for one) rather than expecting a fallback.
- Before a byte is written the client checks the path itself: it must be a socket (checked with `lstat`, so a symlink is refused rather than followed), owned by the calling user, with no group or other permission bits. The daemon creates it `0600` under a narrowed umask so it is private from the moment it exists, not from the moment a `chmod` lands.
- The reply's length header comes from the peer, so it is a request for an allocation rather than a fact. A signature is a few hundred bytes; anything above 4 KiB is refused before memory is reserved.
- The socket's send and receive timeouts are 2 s. The daemon answers one request at a time, so a sign can queue behind another; the bound is for a daemon that is wedged, not one that is busy.

With no daemon reachable, signing fails and returns no signature. It never falls back to a transport it cannot authenticate.

## Adding a new venue

The expected pattern for a new connector:

1. Create `connectors/src/<venue>/` and `connectors/include/lrvx-connectors/<venue>/`.
2. Implement the venue-specific subclasses of the connector / executor abstractions in `lrvx::IExchangeConnector` (`lrvx/connector/abstract_exchange_connector.h`) and `lrvx::IOrderExecutor` (`lrvx/execution/abstract_executor.h`) — both live directly in namespace `lrvx`.
3. The CMake glob in `connectors/CMakeLists.txt` picks up new `.cpp` files automatically — no CMakeLists edit needed.
4. Add an integration test under `connectors/tests/integration_test_<venue>.cpp` (build-only by default).

The existing four adapters are concrete worked examples; a Hyperliquid-style off-process signer or a Polymarket-style Rust FFI is supported via the same gating pattern.
