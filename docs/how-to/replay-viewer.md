# Inspect a tape and run in the replay viewer

The replay viewer is a single-page UI that loads a `.lrvx` tape, an optional `.lrvxrun` strategy trace, or both, and lays them out on a shared timeline. Drag, scrub, share a URL.

It is a static HTML build with no backend, no auth, and no live data path. The whole point is to look at one captured run, with no infrastructure beyond a browser.

## What you see

Six views on one page:

- **Price chart with trades and signals.** Top banner. Trade-line (price over time) plus per-trade dots colored by side, vertical dashed bands at signal timestamps with names, triangles where fills landed, and a vertical cursor line. Hover a point to see a tooltip with the nearest trade's timestamp, side, price, and qty. Click anywhere on the chart to seek the cursor to that timestamp.
- **Order book heatmap.** Bid / ask depth at the cursor, reconstructed from the latest snapshot plus deltas. Reads `.lrvx` book frames. A top-of-book frame (frame type `6`) is not depth and stays out of the ladder; when one arrived after the last depth frame, the touch it carries is drawn as a line between the asks and the bids. A symbol with tops and no depth yet shows that line alone.
- **Trades.** Tail of the most recent trades up to the cursor. Reads `.lrvx` trade frames.
- **Strategy signals.** Cards listing each `SignalRecord` from the run trace, with name, symbol ids, and payload. Reads `.lrvxrun`.
- **Orders timeline.** Order events plus rolling fill totals per order. Reads `.lrvxrun`.
- **Equity curve.** PnL derived inline from fills. Reads `.lrvxrun`.

A view shows an empty-state message when the data it needs is not loaded ("No `.lrvxrun` loaded. Drop one to see strategy decisions.").

## Open with the CLI

The `lrvx tape view` subcommand stages your artifact, starts a local web server, and opens a browser tab pointed at the SPA:

```bash
lrvx tape view path/to/btc.lrvx
lrvx tape view path/to/btc.lrvxrun
lrvx tape view path/to/btc.lrvx path/to/btc.lrvxrun
lrvx tape view tapes/bybit-btc tapes/binance-btc          # merged multi-tape
```

`paths` is variadic. Pass any number of `.lrvx` directories and
at most one `.lrvxrun`; the viewer auto-loads what you gave it. With
two or more tapes the CLI materialises a merged synthetic tape with
symbols rekeyed into a global id space keyed by `(exchange, name)`
and points the viewer at that — see
[Merge multiple tapes](multi-tape.md).

Flags:

- `--port 8765` (default). Override if the port is taken.
- `--no-open`. Print the URL instead of launching a browser. Useful over SSH.

## Open with drag-and-drop

A file-system check or curl is overkill for one-off inspection. Open the SPA at `http://localhost:8765` (or wherever you serve `tools/replay-viewer/dist/`), then drag the `.lrvx` directory or the `.lrvxrun` directory onto the drop zone. The viewer recognizes both shapes and routes them to the right parser.

## Scrubbing and playback

The control bar has a scrubber, a play / pause button, and a speed selector (`1x`, `5x`, `25x`, `100x`). Playback advances the cursor in real time at the chosen multiplier. Each view re-renders to reflect the slice up to the cursor. Pause to stop.

The cursor's nanosecond timestamp shows on the right. The same value is written into the URL hash:

```
http://localhost:8765/#t=1700000000000000000&s=5
```

Copy the URL to share a specific moment with someone. Reload it; the cursor jumps back to the same instant. Replace `s=5` with your speed.

## Tape format support

The viewer reads:

- The published [`lrvx v1.0` directory layout](../spec/lrvx.md) with `manifest.json` plus `*-NNNNNN.bin` segments.
- The legacy single-file form (`<timestamp>.lrvx`) that `lrvx tape record` emits today.
- Both compressed and uncompressed segments. LZ4 block decompression runs in the browser.

For `.lrvxrun`, the viewer reads the [v1.0 directory layout](../spec/lrvxrun.md) with `manifest.json` plus per-kind segment files (`signals-NNN.bin`, `orders-NNN.bin`, `fills-NNN.bin`).

## Performance budget

Target: load a one-hour BTCUSDT tape (~50K trades) in under three seconds on an M-class macbook, scrub at 60fps. The MVP uses straightforward array filtering on every cursor change; profile and add binary-search indices when a real workload exposes a slowdown.

A capture with no book frames (the default `lrvx tape record` shape today) shows the order-book view as empty. Wire book frames into the recorder via the recorder hooks if you need them.

## Limitations

- No backend, no auth, no live data. Open captured runs only.
- One run at a time. Multiple tapes are merged into a single synthetic stream rather than shown side by side; the SPA has no multi-venue legend or per-venue book chart, so each merged tape's symbols appear as distinct global ids on one chart.
- The order-book view limits to the top fifteen levels per side.
- Equity is a simple cash + last-price-marked-position computation. Realistic backtest equity (with mark-to-market on the tape clock) belongs in the backtest reports, not here.
- Price-chart interactivity is hover-tooltip plus click-to-seek only. No zoom, no pan, no drag-select range. Add as follow-ups if a real workload demands them.

## See also

- [`lrvx` tape format spec](../spec/lrvx.md). The market-data side.
- [`lrvxrun` strategy-trace format spec](../spec/lrvxrun.md). The strategy-side artifact the viewer reads.
- [Record a strategy run as `.lrvxrun`](lrvxrun.md). How to produce the artifact this viewer consumes.
- [Record and replay tapes](tape-record.md). How to capture the input the viewer renders.
