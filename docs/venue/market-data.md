# Market data

The engine emits `OutboundEvent`s about individual orders. A market data feed
is an ordered, sequenced stream that subscribers can apply to rebuild the
book. `MarketDataPublisher` is the bridge between the two.

```cpp
MarketDataPublisher<> md([](const MdMessage& m) { multicast(m); },
                         /*tickSize*/ Price::fromDouble(0.01), /*symbol*/ 1);

MatchingEngine<MatchingBook> venue(cfg, [&](const OutboundEvent& e)
                                   {
                                     // The engine's sequencer time stamps the feed;
                                     // the publisher never reads a clock for it.
                                     md.onEvent(e, venue.engineTimeNs());
                                     ledgerOrMetrics(e);
                                   });

md.book();   // lrvx::NLevelOrderBook kept in step with the venue book
```

## Messages

| `MdType` | Meaning | `price` field | `qty` field |
|---|---|---|---|
| `AddOrder` | an order joined the book | limit price | shown quantity |
| `Trade` | a print (`id` is the taker) | trade price | executed quantity |
| `Executed` | a resting order was hit | resting price | displayed remaining |
| `Cancel` | an order left the book | resting price | — |
| `Replace` | an order was repriced/resized | new price | displayed remaining |
| `Triggered` | a stop fired | reference price | — |
| `TradingStatus` | halt / pause / auction transition | — | — |
| `DerivativesUpdate` | mark, funding, open interest | mark price | open interest |

Every message carries `(symbol, epoch, seq)`: `seq` is contiguous from 1
within one publisher lifetime, `epoch` is a random value minted when the
publisher is constructed. One publisher serves one symbol; several publishers
may share a multicast group, and the symbol in every root block is what a
consumer demultiplexes on before any gap accounting.

## Time: `engineTsNs` and `sendTsNs`

Every message on every path carries two nanosecond timestamps, and they mean
different things on purpose.

| Field | Source | Reproducible on replay? |
|---|---|---|
| `engineTsNs` | the sequencer-ts of the command that caused the event (`MatchingEngine::engineTimeNs()`) | **yes** |
| `sendTsNs` | wall clock when the publisher sent *this copy* | **no** |

`engineTsNs` is journaled input, so replaying the same command stream produces
the same value on every message — it is what time bars, a faithful tape and
any metric with time as a variable are built on, and it is safe to fold into a
determinism digest. The publisher does not invent it: an `OutboundEvent`
carries no time of its own, so the caller — which knows the engine's time —
passes it to `onEvent`.

`sendTsNs` is a wall-clock read at send time and is deliberately **not**
reproducible. A resend of the same message carries a later `sendTsNs` than the
original, which is exactly how a consumer tells a replayed copy from live
flow. `sendTsNs - engineTsNs` is the venue's own outbound latency. It must
never enter an event hash or a determinism comparison (`hashEvent` does not
see it: the timestamps are added by the publisher, below the event layer).

Both are non-decreasing within one publisher: `engineTsNs` because sequencer
time is, `sendTsNs` because the publisher latches the maximum it has issued —
a backwards step of the system clock cannot make the feed look reordered.

On the wire:

- **SBE** — two trailing `int64` fields (`engineTs`, `sendTs`, `sinceVersion=3`)
  appended to the end of every incremental root block, exactly as `epoch` was
  in version 1. A version-1/2 reader skips them via the header's `blockLength`
  and decodes everything else byte for byte.
- **FIX** — `52 SendingTime` carries `sendTsNs`, `60 TransactTime` and
  `273 MDEntryTime` carry `engineTsNs`. All standard fields; a message from a
  publisher that has no timestamp (0) omits the tag rather than printing the
  epoch.

## Trading status

Halts, volatility pauses and auction phases are published as transitions, from
the engine's own state changes — a subscriber never has to infer a halt from a
feed that went quiet, and never sees a repeat for a state that did not change.

| `TradingStatus` | Meaning | `untilNs` |
|---|---|---|
| `Trading` | continuous matching | 0 |
| `Halted` | operator halt; new orders rejected | 0 |
| `LuldPause` | timed limit-up/limit-down volatility pause | pause deadline (sequencer time) |
| `AuctionPreOpen` | pre-open accumulation, no matching | 0 |
| `AuctionUncross` | the uncross itself | 0 |
| `Closed` | the session is closed; new orders rejected, the book stands | 0 |
| `Delisted` | withdrawn from trading, with no scheduled return; the book was pulled | 0 |

The transitions between them are one table in `lrvx-venue/engine/session.h`,
written out in [matching.md](matching.md#the-session-automaton).

`TradingStatusReason` says why: `Administrative` (an operator `AdminCmd`),
`LuldBreach`, `LuldPauseElapsed` (the timed pause ran out and trading
resumed), `Auction`, `Session` (a session boundary).

`untilNs` is the deadline of the **timed pause and of nothing else**. Any other
state publishes 0, even when a pause deadline is still stored underneath it — a
subscriber is never handed an expiry for a state that does not expire.

An opening auction publishes three transitions — `AuctionPreOpen`,
`AuctionUncross`, `Trading` — so the uncross fills sit between the last two and
can be attributed to the auction rather than to continuous trading.

### `Closed` is not `Halted`

They are different states on purpose. A halt is an *exception* — something went
wrong and an operator stopped the instrument, with no promise about when it
resumes. A closed session is the instrument's *normal* out-of-hours condition.
A subscriber that cannot tell them apart cannot tell a broken market from a
sleeping one, and the two reject an order with different reasons
(`Halted` vs `MarketClosed`).

- `Closed` is the outermost state: it wins over an auction phase and over a
  halt in `tradingStatus()`, and the halt or auction underneath it is left
  untouched — a close during a halt reopens **still halted**, because a session
  boundary must not silently clear an exception an operator raised.
- Closing does **not** pull the book (that is `HaltAndCancelAll`). Resting
  orders stand across the close, and a cancel is still accepted: a client must
  be able to get out of what it cannot add to.
- Transitions arrive as the sequenced `AdminCmd` actions `CloseSession` /
  `OpenSession`, so they journal, replay, hash and checkpoint like a halt.

What the engine deliberately does **not** own is the **calendar**. Nothing here
fires on a clock; the schedule that decides *when* to send `CloseSession` /
`OpenSession` belongs to the operator / control plane (see
[runtime.md](runtime.md)), which drives it through the control-plane `session`
verb.

On the wire the value is appended at the end of the enum, so no root block
changes and a reader of the previous schema decodes every field of the message
and sees only an unrecognised status code — which it must treat as **not
tradeable**, never as `Trading`. On FIX it is `326 = 18` (*Not available for
trading*, FIX 4.4's end-of-session value), deliberately not the halt code `2`.

`Delisted` is appended the same way and reaches FIX as `326 = 18` as well:
`SecurityTradingStatus` has no value for a delisting, so the reason travels in
Text (58) as `Delisted`, the way a volatility pause does. A status value the
FIX encoder does not know goes out as `326 = 18` with the text
`Not available for trading`, never as `17`.

The engine reaches the feed through a new `OutboundEvent`,
`TradingStatusChanged`, which is hashed like every other event
(`untilNs` is sequencer time, so it replays). It carries no account: the
order-entry codecs have no exec-report form for it, exactly like
`MmpTriggered` / `FeeCharged` / `Liquidation`.

On FIX it is the standard `35=f SecurityStatus`:
`326 SecurityTradingStatus` = `17` trading, `2` halt (both an operator halt
and a volatility pause), `21` pre-open, `22` opening rotation, with the
distinction FIX 4.4 cannot express in `326` spelled out in `58 Text`. It is
sent regardless of the subscription's `MDEntryType` filter — a subscriber that
asked for trades only still has to be told the instrument stopped trading.

## Derivatives

`DerivativesUpdate` carries what a perp holder needs to value a position:

| Field | Meaning |
|---|---|
| `price` | mark price (the last `SetMark` the engine was given) |
| `fundingRateRaw` | last applied funding rate, at `kFundingRateScale` (= the price/qty scale, 1e-8). A rate never travels as a float |
| `nextFundingNs` | next settlement boundary; **0 = the venue does not fund this instrument** |
| `qty` | open interest: the long side of the open positions the engine tracks (equal to the short side) |

It is emitted from the sequenced `SetMark`, `ApplyFunding` and
`SetFundingSchedule` commands — all journaled, so mark, rate, calendar and open
interest all replay. Open interest is published *with the mark*, not on every
fill: a per-trade open-interest message would multiply the feed's rate for a
number consumers read at mark cadence.

### The funding calendar is state, not a formula

`nextFundingNs` is engine state, not a computation over
`(now, SymbolConfig::fundingIntervalNs)`. Deriving it from startup config would
drift from reality the moment an operator changes the interval or shifts a
settlement:

- **`SetFundingSchedule{symbol, intervalNs, nextFundingNs}`** — a sequenced,
  journaled command (control-plane verb of the same name). The operator owns the
  calendar; the engine holds it.
- **`ApplyFunding` advances it** by one whole interval, and by further whole
  intervals if the settlement ran late enough that one step would still leave
  the boundary in the past (an operator catching up after an outage does not
  leave a stale boundary in the feed).
- It is **hashed and checkpointed** (`RestoreFunding`), so a restored engine
  publishes the boundary the venue will actually settle on.
- With **no schedule set** the historical derivation from
  `SymbolConfig::fundingIntervalNs` still applies, unchanged — an engine that
  never learned a schedule behaves exactly as it did before the command
  existed. A non-positive interval or boundary clears the schedule back to it.

The engine still settles funding only when told to (`ApplyFunding`). This is
the calendar it publishes and advances, **not** a timer that fires payments.

The funding **rate** likewise survives a checkpoint now (same `RestoreFunding`
record), so an engine restored from a snapshot publishes the real rate instead
of `0` until the next `ApplyFunding`.

One honest limit remains: on an instrument the engine has never been given a
mark for, no `DerivativesUpdate` is published at all — the feed says nothing
rather than publishing a zero mark. A schedule set before the first mark is in
force but silent until that mark arrives.

On FIX the layer travels as a `35=X` with two standard MDEntries —
`269=6` SettlementPrice for the mark and `269=C` OpenInterest — plus the
funding rate and next funding time in the user-defined tag range, because FIX
4.4 predates the instrument and has no field for either:

| Tag | Field |
|---|---|
| `5001` | funding rate (exact decimal, same printing path as prices) |
| `5002` | next funding time (UTCTimestamp) |
| `5003` | volatility-pause deadline on `35=f` |

## Sequencing, restarts and epochs

Sequence numbers are deliberately not persisted across restarts. The book is
rebuildable from matching state, so a restarted publisher mints a new epoch
and starts over at `seq=1` instead of carrying seq durability it does not
need. A consumer that sees the epoch change knows every prior seq is void and
re-snapshots; without the epoch, a restarted feed would look like an endless
stream of duplicates to a surviving consumer (a live but empty feed).

## Gap detection (client side)

`GapDetector` (`resend_buffer.h`) is the client-side sequencer. Fed the raw
(possibly reordered, duplicated, gapped) datagram stream, it delivers messages
strictly in seq order per `(symbol, epoch)`:

- A message ahead of the next expected seq is held, not delivered; when the
  missing seq arrives — a late, reordered datagram, not a duplicate — the
  contiguous run drains in order. If held messages exceed `maxHeld`, the gap
  is abandoned and the held run is delivered in seq order rather than lost.
- `onGap(symbol, epoch, fromSeq)` fires when a gap opens and re-fires every
  `reraiseAfter` further messages while it stays unfilled, so a lost resend
  request does not strand the consumer.
- An epoch change drops held state, resets the stream to seq 1 and fires
  `onEpoch` so the consumer can re-snapshot.
- `reset()` fast-forwards a stream after a snapshot was applied.

`UdpMdSubscriber::setGapDetector` wires this into `recv()`: with a detector
attached, `recv()` only surfaces in-order messages; without one it is the raw
decode path, unchanged.

## Snapshot and recovery (TCP)

`MarketDataPublisher::snapshotAtomic()` returns a consistent
`(orders, lastSeq, epoch)` triple — taken under the same lock as the live
emit path, so no increment with `seq <= lastSeq` is missing from the snapshot
and none with `seq > lastSeq` is included. The publisher also keeps a bounded
ring of recent increments (`resendCapacity`, default 1024) for tail replay.

`MdRecoveryServer` (`md_recovery.h`) serves both over TCP with
length-prefixed frames, one request per connection:

- client sends `ResendRequest{symbol, fromSeq}` (`fromSeq=0` = late joiner);
- if `fromSeq` is inside the ring, the server replays the increments with
  `seq >= fromSeq` and closes (EOF terminates the replay);
- otherwise it answers `SnapshotRequired` followed by `SnapshotBegin` + the
  instrument's current state + `AddOrder` body messages (`seq=0`, epoch set) +
  `SnapshotEnd{lastSeq}`; the consumer applies the body and resumes the
  incremental feed at `lastSeq+1`.

### What a snapshot carries

A book alone is not a startable state: a halted, paused or pre-open instrument
looks exactly like a quiet one, and a perp position cannot be valued without a
mark. So a snapshot carries the **current** `TradingStatus` and
`DerivativesUpdate` (whichever the publisher has published) as `seq=0` body
records placed **between `SnapshotBegin` and the orders**, so a subscriber
knows the instrument is halted before it applies a single order.

`SnapshotBegin::orderCount` still counts `AddOrder` messages only, and
`MdRecoveryClient::Result` keeps them out of `messages` (the book body) in
`hasStatus`/`status` and `hasDerivatives`/`derivatives`. On a **resend** the
same two message types are ordinary sequenced increments and stay in the
stream — diverting them there would punch a hole in the seq run.
`RecoveringMdSubscriber::onSnapshotState` is the optional hook for consumers
that track halts or value positions; a consumer that only rebuilds a book
ignores it.

On FIX the snapshot leads with `35=f SecurityStatus` and the derivatives
`35=X` before the `35=W` full refresh, whose entries each carry `273
MDEntryTime`.

`MdRecoveryClient` (same header) is the consumer-side counterpart: one
`recover(symbol, fromSeq, out)` call opens the connection, sends the request
and returns the classified reply — `snapshot` flag, the replayed increments or
the snapshot body, and the `lastSeq`/`epoch` resume point (feed
`resetSequencer(symbol, epoch, lastSeq + 1)` to the `GapDetector` and continue
the incremental feed). The socket carries a receive timeout; an unreachable
server is a `ConnectError` status, a stalled reply a `Timeout`, and a
truncated snapshot a `ProtocolError` — never an exception. The low-level
protocol cases in `test_venue_md_recovery` drive it directly (they are the
protocol's documentation); the late-joiner and publisher-restart cases run
through `RecoveringMdSubscriber` below.

`MdCounters` tracks the paths: `resendServed`, `snapshotsServed`, and
`sendDrops` (below).

### Exposing the recovery channel

`MdRecoveryServer::start(port, bindIp)` binds loopback by default
(`bindIp = nullptr`, the historical behaviour). Passing an explicit address
(`"0.0.0.0"`, a NIC address) exposes the channel to other hosts, and an
unparseable address fails the start rather than silently falling back to
loopback.

The hardening contract is the same as for the order-entry gateways: the
recovery protocol carries **no authentication and no encryption** of its own,
so anything reachable off-host must sit behind TLS termination and/or a
firewall / private network, exactly like the control plane. Binding
externally is a deployment decision, not a default.

## Automatic client recovery (`RecoveringMdSubscriber`)

`GapDetector` reports; `MdRecoveryClient` fetches; `RecoveringMdSubscriber`
(`md_recovery.h`) is the two composed into the whole consumer protocol, so a
subscriber implements neither. It wraps a `UdpMdSubscriber` (which must NOT
have its own detector attached — this class owns sequencing) and keeps a
`recv()`-style API: only in-order messages surface, recovery is invisible
except through the `onSnapshot` callback and the counters.

```cpp
UdpMdSubscriber sub;
sub.join(group, port, /*multicast*/ true);

RecoveringMdSubscriber::Config cfg{host, recoveryPort, /*maxGapBeforeSnapshot*/ 256};
RecoveringMdSubscriber rsub(sub, cfg);
rsub.onSnapshot([&](SymbolId sym, const MdSnapshotBegin& b, const std::vector<MdMessage>& orders)
                { book.clear(); for (const auto& m : orders) book.apply(m); });

MdMessage m;
while (rsub.recv(m)) book.apply(m);   // strictly in seq order, holes already recovered
```

- **Gap** → `ResendRequest{fromSeq}`; the replayed increments are woven back
  through the detector, so they fill the hole in seq order and drain whatever
  was held behind it.
- **Epoch change** (publisher restart) or a gap deeper than
  `maxGapBeforeSnapshot` → snapshot recovery (`fromSeq = 0`): the body goes to
  `onSnapshot`, the sequencer fast-forwards to `lastSeq + 1` and the
  incremental feed continues. A deep gap takes this path because the server
  would route it there anyway once the ring has trimmed.
- **Failure** → bounded retries with doubling backoff (`maxAttempts`,
  `initialBackoff`), never an eternal loop. On exhaustion the incident is
  counted in `recoveriesFailed()` and the stream falls back to plain gap
  semantics — the detector re-raises on further traffic and recovery re-arms
  (a test drives exactly that: server down → bounded failure → clean `recv()`
  timeout → server back → the re-raised gap recovers through the ring).
- Counters: `gapsDetected`, `epochChanges`, `resendsRecovered`,
  `snapshotsRecovered`, `recoveriesFailed`.

Single-threaded like the subscriber itself: all recovery work happens inside
`recv()` on the calling thread (the backoff sleeps block the caller, as any
synchronous recovery would).

## Choosing a transport

Two transports carry the same feed. They differ in reach, not in content: a
subscriber on either sees the same messages with the same `seq` under the same
`epoch`.

| | Multicast (`udp_multicast.h`) | Unicast TCP (`md_distribution.h`) |
|---|---|---|
| Reach | one layer-2 segment | anywhere the venue is reachable |
| Fan-out cost | one datagram per message, any number of consumers | one encode + one write per subscriber |
| Delivery | best effort; holes are the consumer's to recover | in order, on a reliable stream |
| Snapshot / resend | separate channel (`MdRecoveryServer`) | inline on the same connection |
| Encodings | SBE | SBE, FIX, or another registered one |

**Pick multicast when the consumer is colocated** — inside the same segment,
on the data NIC. It is the lowest-latency, lowest-cost fan-out there is, and
the venue does the same work for one consumer as for a hundred.

**Pick unicast TCP for everybody else.** Multicast does not survive a WAN hop,
most cloud networks do not route it, and a consumer behind NAT cannot join a
group. That is not a tuning problem — it is the reason an external consumer
has no multicast option at all.

Running both is the normal arrangement: fan the publisher's sink into the
multicast publisher and the distribution server together.

```cpp
MdDistributionServer dist(&counters);

MarketDataPublisher<> md([&](const MdMessage& m)
                         {
                           udpPub.publish(m);   // colocated consumers
                           dist.publish(m);     // everybody else
                         },
                         /*tickSize*/ Price::fromDouble(0.01), /*symbol*/ 1);

dist.addPublisher(md);
dist.start(/*port*/ 9010, /*bindIp*/ "0.0.0.0");
```

## Unicast distribution (`MdDistributionServer`)

A long-lived TCP session per subscriber. Nothing about sequencing is
reinvented: `MarketDataPublisher` stays the source of truth for `seq`, the
`epoch`, `snapshotAtomic()` and the resend ring, and the server is a fan-out
in front of it.

**Verbs** (SBE templates 11/12 and the existing 7; the FIX equivalents are
below):

- `Subscribe{symbol, fromSeq, snapshotOnly}` — start streaming. `fromSeq = 0`
  asks for the atomic snapshot followed by increments; `fromSeq > 0` resumes
  at that seq. `snapshotOnly` serves one book and leaves no subscription.
- `Unsubscribe{symbol}` — stop that symbol; the others keep flowing. One
  connection carries any number of symbols.
- `ResendRequest{symbol, fromSeq}` — the same verb the recovery channel uses,
  answered inline on this connection.

**Subscribing mid-stream.** A snapshot taken while the feed is moving must
neither lose an increment nor let one jump ahead of the book it belongs to.
The symbol is registered *unarmed* first, so every increment from that instant
is encoded and parked in a bounded buffer; the snapshot is then taken outside
the session lock; finally the snapshot frames are queued, the parked
increments past `lastSeq` are queued behind them, and the subscription arms at
`lastSeq + 1`. A test drives exactly that race and checks the joiner's book
against a consumer that watched from the first message.

**Resend.** `fromSeq` inside the ring is replayed from it
(`MdCounters::resendServed`). `fromSeq` older than the ring's tail gets the
explicit `SnapshotRequired` followed by a full snapshot
(`MdCounters::snapshotsServed`) — the same two-way answer the recovery channel
gives, without a second connection. Replayed increments may arrive behind
newer live ones; a consumer sequences with `GapDetector` exactly as on the
multicast path.

**Slow consumers.** Each subscriber has a bounded outbound queue drained by
its own writer thread, the same shape as the order-entry session writer. The
publishing thread encodes and enqueues; it never waits on a socket. A full
queue is the verdict: the connection is shut down, its frames are dropped and
`MdCounters::slowConsumerDisconnects` is bumped. One wedged subscriber cannot
delay another and cannot delay the engine. There is deliberately no send
timeout on the socket — a blocked writer thread is fine, and a socket timer
would misreport a slow reader as a broken connection. `sendBufferBytes` bounds
`SO_SNDBUF` so the kernel does not hide backlog the queue is meant to see.
Conflation is not the v1 policy; the bounded queue and the disconnect are.

**Liveness.** No inbound byte within `idleTimeoutMs` drops the subscriber
(`MdCounters::idleDisconnects`), the same policy the order-entry gateways
hold. The server sends its own beat every `heartbeatMs` when the connection is
quiet — an empty frame on the binary encoding, `35=0` on FIX. A subscriber is
expected to beat back.

**Publisher restart.** A message whose `epoch` differs from the
subscription's resets the delivery position, so a feed that restarted at
`seq = 1` is not filtered out as stale by a subscription still expecting
`seq = 500`. The subscriber sees the epoch change and re-subscribes.

**Counters.** `subscribers` (live sessions), `slowConsumerDisconnects`,
`idleDisconnects`, `subscribeRejects`, plus the shared `resendServed` /
`snapshotsServed`.

**Exposure.** `start(port, bindIp)` binds loopback unless given an explicit
address. The protocol carries no authentication and no encryption of its own,
so anything reachable off-host belongs behind TLS termination and/or a
firewall — the same contract as the recovery channel and the order-entry
gateways.

## Pluggable encodings

Transport and encoding are independent axes. `MdEncoder` (`md_encoder.h`) owns
the bytes and knows nothing about subscriptions, snapshots or sequencing; the
distribution server owns those and knows nothing about the bytes. Framing
belongs to the encoder — every method appends complete, self-framed wire bytes
that the session writes verbatim, which is what lets one port carry a
length-prefixed binary encoding and a self-delimiting text one. Adding an
encoding is writing one `MdEncoder` and registering it; no distribution logic
changes.

The encoding is chosen per connection **from its first byte, peeked and not
consumed**, so both protocols still start at their own first byte and an
off-the-shelf implementation of either connects unmodified:

| First byte | Encoding | Why it is unambiguous |
|---|---|---|
| `0x00` | SBE | the high byte of the u32 frame length; feed frames are tens of bytes |
| `'8'` | FIX | the first byte of the BeginString `8=FIX.4.4` |

`registerEncoding(leadByte, factory)` adds a third.
`MdDistributionConfig::encoding` pins a port to one encoding instead, for a
deployment that prefers a port per protocol. One encoder instance lives per
connection, so an encoder may hold session state; every call on it is
serialized by the session.

## FIX market data

`fix_md_codec.h`. The framing, the checksum, the exact decimal printing and
the UTCTimestamp all come from the order-entry codec unchanged — prices and
sizes go through `decwire`, never through a double, so `100.25` prints as
`100.25`.

It is a separate header because market data is a different message family:
order entry is flat `tag=value` and `FixCodec::parseFields` deliberately
collapses repeats into a map, while market data is built on repeating groups
(146, 267, 268) where the repeats *are* the payload.

**Inbound — MarketDataRequest (35=V)**

| Tag | Field | Handling |
|---|---|---|
| 262 | MDReqID | required; echoed on every W/X/Y |
| 263 | SubscriptionRequestType | `0` snapshot, `1` snapshot + updates, `2` unsubscribe; anything else → `35=Y`, `281=4` |
| 264 | MarketDepth | `0` or absent = full book; a positive depth → `35=Y`, `281=5` |
| 267 / 269 | MDEntryTypes | `0` bid, `1` offer, `2` trade; omitted = all three; unknown → `35=Y`, `281=8` |
| 146 / 55 | Symbols | one subscription per symbol; none → `35=Y`, `281=0` |

**Outbound**

- **35=W MarketDataSnapshotFullRefresh** — 262, 55, 268 `NoMDEntries`, then
  per entry 269 `MDEntryType`, 270 `MDEntryPx`, 271 `MDEntrySize`, 273
  `MDEntryTime` (the engine time the order was accepted at), 278 `MDEntryID`.
  The feed is order-level, so every entry carries its order id. The current
  `35=f` and the derivatives `35=X` are sent ahead of it.
- **35=X MarketDataIncrementalRefresh** — 262, 60 `TransactTime`, 268, then
  279 `MDUpdateAction` (`0` New, `1` Change, `2` Delete), 269, 55, 270, 271,
  273, 278. `AddOrder` and a trade print map to New, `Cancel` and a fully
  consumed order to Delete, `Replace` and a partial fill to Change. A stop
  trigger has no market-data entry and is not sent. The derivatives layer uses
  the same message with two entries (`269=6` mark, `269=C` open interest) plus
  tags 5001/5002.
- **35=f SecurityStatus** — 324, 55, 326 `SecurityTradingStatus`, 60, 58, and
  5003 for a volatility-pause deadline. Sent on every trading-status
  transition regardless of the `MDEntryType` filter.
- **35=Y MarketDataRequestReject** — 262, 281 `MDReqRejReason`, 58.

**Session layer.** Deliberately a light one — Logon (35=A), Heartbeat (35=0),
TestRequest (35=1), Logout (35=5) and a monotonic outbound MsgSeqNum — not
`FixSessionHost`/`FixConnection`. That session layer is built around order
entry: accounts, the `SessionRegistry` exec-report log, application-level
resend with PossDup. None of it has a meaning for a broadcast feed. Market
data is not account-scoped, and a consumer recovers from a hole with the
feed's own tools (a fresh MarketDataRequest, or a seq-based resend on the
binary encoding), never by replaying a per-account message log. FIX market
data has no seq-based resend at all, so a re-request is answered with a fresh
full refresh. A message before the Logon is answered with a Logout; an
unsupported MsgType with a session Reject (35=3) rather than silence.

## Icebergs and the public feed

An iceberg shows a peak and hides the rest. The public feed must only ever see
the peak, or the book can be probed for hidden size by watching what the feed
reports.

That is why `OrderExecuted` carries two quantities:

- `leavesQty`: the whole remaining (peak + hidden), for the owner's execution
  report. You should see your own order in full.
- `displayLeaves`: the displayed peak, for the public feed.

The publisher drives depth from `displayLeaves`. Using the whole remaining
would leak the reserve and, on a partial peak fill, corrupt the level
aggregate.

## Depth is the aggregate, kept in step

The publisher maintains a `lrvx::NLevelOrderBook`, the same aggregate book
type strategies consume elsewhere in lrvx, so market-data depth and the
venue's internal order-level book stay in agreement. A test drives 300k
commands and periodically compares the publisher's full depth against the
engine's book.

## SBE codec and multicast

`sbe_md_codec.h` encodes `MdMessage` in SBE (Simple Binary Encoding): a
canonical SBE message header (`blockLength`, `templateId`, `schemaId`,
`version`) followed by a per-type little-endian root block. One template per
`MdType`, each carrying only the fields that type needs, plus the recovery
templates (`ResendRequest`, `SnapshotBegin`, `SnapshotEnd`,
`SnapshotRequired`) and the distribution verbs (`Subscribe`, `Unsubscribe`,
`SubscribeReject`), the instrument-wide messages (`TradingStatus`,
`DerivativesUpdate`). Schema version 1 appended a trailing `epoch` (u64) to
every incremental template — the SBE-sanctioned extension path: a version-0
reader skips it via the header's `blockLength`, and the decoder accepts v0
frames (epoch reads as 0). Version 2 added the three distribution templates
and nothing else: no existing root block changed, so a version-1 reader
decodes every incremental and recovery message byte for byte as before.
Version 3 added the two instrument-wide templates and, on every incremental
template, the two trailing timestamps (`engineTs`, `sendTs`) at the end of the
root block — again the same path, so a version-1/2 reader decodes every field
it knows out of a version-3 frame unchanged and skips the rest. Template ids
1..6 mirror the order-level `MdType`s (`templateId = MdType + 1`); the two new
ones are 14 and 15 and are mapped explicitly, since 7..13 were already taken by
the recovery and session verbs. Enumerations extend the same way fields do —
new values are appended (`TradingStatus::Closed`, `StatusReason::Session`), so
no root block changes and an older reader decodes every field of the message,
seeing only a status code it does not recognise. A consumer must treat an
unknown `TradingStatus` as *not tradeable*. The schema in `venue/schema/md-sbe.xml` is the
source of truth; the codec is hand-written against it (no Java codegen in the
build). This is the venue's own SBE schema, not Nasdaq ITCH.

`UdpMdPublisher` (`udp_multicast.h`) publishes frames over IP multicast, with
`UdpMdSubscriber` on the receiving end. Both take an optional interface selector
(`ifaceIp`): the default (`nullptr`) lets the kernel routing table pick the
egress NIC -- the production default so the feed reaches co-located clients on
other hosts; pass `"127.0.0.1"` to pin loopback for same-host use.

The publisher socket is non-blocking and `publish()` checks the `sendto`
result: a datagram that cannot be handed to the kernel (full socket buffer,
closed fd) is a counted drop (`MdCounters::sendDrops`), never a stall of the
matching thread and never a silent loss the venue cannot see. Consumers
recover the hole through gap detection and the recovery channel.

```cpp
std::vector<uint8_t> buf;
SbeMdCodec::encode(msg, buf);                        // header + template block

MdMessage out;
const bool ok = SbeMdCodec::decode(buf.data(), buf.size(), out);  // false if short/foreign
```

The decoder validates the SBE header (rejects a foreign `schemaId`), uses
`blockLength` to skip trailing fields a newer schema version appended (forward
compatible), and bounds-checks before reading. The parser fuzz drives it with
hostile input alongside the FIX / SBE order-entry / REST parsers.

## Tape

`tape_recorder.h` records trade prints only: it filters `Trade` events out of
the outbound stream and persists them through `BinaryLogWriter`, the same
binary format the rest of lrvx reads. The trade tape a venue produces can be
replayed by ordinary lrvx tooling; the non-trade events (adds, cancels,
replaces) are not on the tape.
