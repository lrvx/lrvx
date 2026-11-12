# Connectors

Native exchange connectors for the lrvx engine.

Adapters in this directory:

- **Bybit V5** — WebSocket + REST executor.
- **Bitget V2** — WebSocket + classic account REST executor.
- **Hyperliquid** — WebSocket + REST executor (uses `utils/hl_signerd.py` as the signing daemon).
- **Polymarket** — WebSocket + Rust FFI executor.

## The live fill contract

Every connector here reports fills under the same rules; see [docs/explanation/connectors.md](../docs/explanation/connectors.md) for the reasoning behind each.

- `OrderEvent::fillQty` is the size that just traded, `order.filledQuantity` the cumulative one.
- `OrderEvent::fillPrice` is the price it traded at. Unset means the position is booked at zero.
- A fill is published only when the venue reported the price it traded at. An unpriced increment is held for the next priced report, not published at zero and not dropped.
- One event per execution, even when the venue announces it on two channels.
- `order.id` is the id the engine issued (sent as the venue's client order id and read back), never the venue's own order id.
- A venue rejection publishes `REJECTED` with the venue's reason text and leaves no live order behind.
- Every event carries `recvNs`, stamped on receipt, and every book event carries `sourceExchange`.

`tests/unit_test_*_fill_contract.cpp` pins this per venue, offline.

## Ticks and refusals

At `start()` each connector reads its symbols' instrument info (price tick, size step) from the venue's public endpoint and registers the symbols with the venue's tick, marked known (a symbol registered before `start()` with no tick declared takes the venue's in place; a declared one is kept and a difference reported); the executors then refuse an off-tick price locally with the simulator's reason (`price_off_tick`, `trigger_off_tick`) and send on-tick prices exactly. A failed load is logged as an error and leaves the tick unknown, with prices sent as given. A refused or undelivered cancel, replace or position stop publishes an event with the venue's error text. The endpoints, fields and rules are in [docs/explanation/connectors.md](../docs/explanation/connectors.md#order-prices-and-the-tick); the recorded responses the parsers are tested against live in `tests/fixtures/instrument_info/`.

## Build

The connectors are part of the lrvx repo and are built when the parent project is configured with `-DLRVX_BUILD_CONNECTORS=ON`. The static library exposes the target `lrvx::connectors` (alias of `lrvx-connectors`).

```bash
cmake -B build -DLRVX_BUILD_CONNECTORS=ON
cmake --build build --target lrvx-connectors
```

`ixwebsocket` and `simdjson` come in via FetchContent at configure time, so a fresh clone needs no submodules. System dependencies: OpenSSL, zlib, libcurl. The Polymarket executor additionally needs the Rust toolchain (cargo); pass `-DLRVX_ENABLE_POLYMARKET_ORDER_EXECUTOR=OFF` to skip it.

## Tests

The integration tests under `tests/` connect to live exchange endpoints, so they are not part of the default ctest run. To build them:

```bash
cmake -B build -DLRVX_BUILD_CONNECTORS=ON \
                -DLRVX_BUILD_TESTS=ON \
                -DLRVX_BUILD_CONNECTOR_INTEGRATION_TESTS=ON
cmake --build build
```

To register them with `ctest` (still requires network access at runtime):

```bash
cmake -B build ... -DLRVX_RUN_CONNECTOR_INTEGRATION_TESTS=ON
```

## Hyperliquid signing daemon

Hyperliquid orders are signed via a small Python daemon that wraps the official [`hyperliquid-python-sdk`](https://github.com/hyperliquid-dex/hyperliquid-python-sdk):

```bash
pip install git+https://github.com/hyperliquid-dex/hyperliquid-python-sdk.git
python3 connectors/utils/hl_signerd.py
```

The C++ executor talks to it over a local socket; see `hyperliquid_order_executor.h` for the protocol.

## Polymarket configuration

Polymarket trading requires a proxy-wallet setup:

1. Connect a wallet at <https://polymarket.com> and enable trading. This creates a proxy wallet on Polygon.
2. Deposit USDC to fund the proxy wallet.
3. Note the proxy wallet address (account settings or API).

Pass the trading wallet's private key and the proxy wallet address into `PolymarketOrderExecutor`:

```cpp
#include <lrvx-connectors/polymarket/polymarket_order_executor.h>

lrvx::PolymarketOrderExecutor executor(
    "0xYOUR_PRIVATE_KEY_HEX",      // trading wallet private key
    "0xYOUR_PROXY_WALLET_ADDRESS", // proxy / funder wallet
    logger
);

if (executor.init()) {
    executor.warmup();
    executor.prefetch(tokenId);
    auto result = executor.buy(tokenId, Volume::fromDouble(10.0));
}
```

Recommended: keep credentials in env vars (`PM_PRIVATE_KEY`, `PM_FUNDER_WALLET`) rather than in source.

## License

MIT — same as lrvx.

## Disclaimer

The authors are not affiliated with any exchange listed above and accept no responsibility for trading losses or regulatory issues arising from use of this code. Comply with each venue's terms of service.
