---
code: E_DATA_002
title: Tape event arrived past reorder window
severity: error
since: 0.6.1
---

# E_DATA_002 — Tape event arrived past reorder window

`BinaryLogReader::streamForEach` / `run()` and `MergedTapeReader::streamEvents` / `run()` apply a bounded reorder buffer to segments without the Sorted flag. When an event arrives with `exchange_ts_ns` more than `reorder_window_ns` below the current watermark, the buffer has already emitted past that event's slot and cannot place it in sorted order.

The default reaction is not this error. `BinaryLogReader` drops the event, adds one to `ReaderStats::late_dropped`, and logs one warning per segment naming the symbol, event type, file and offset. Set `ReaderConfig::strict_ordering = true` to get the error instead. Reproducibility gates do, because there a dropped frame is a silent difference between two runs.

`MergedTapeReader` still raises unconditionally.

The message carries the observed delta, the configured window, and where in the file the event came from.

## Why dropping is the default

A five-day single-symbol bybit capture of 12.9 million frames carries about 135 000 inversions. Fifteen of them sit deeper than ten seconds. Raising on those fifteen ended the walk at the halfway mark, with aggregators already fed half a dataset and nothing to roll back to. Discarding fifteen frames and saying so at least leaves you a usable run and a number to look at. It is also what streaming systems normally do with late data.

## How to fix

Worth doing when `late_dropped` is higher than you are willing to lose, or when you turned strict ordering on and hit the error.

1. **Bump the reorder window** on the reader config. The default is 10 s; if the affected tape has reconnect-induced gaps longer than that, set a larger window when constructing the reader.

    === "Python"
        ```python
        reader = lrvx.DataReader("./tape", reorder_window_ns=30_000_000_000)
        ```

    === "C++"
        ```cpp
        lrvx::replay::ReaderConfig cfg;
        cfg.data_dir = "./tape";
        cfg.reorder_window_ns = 30'000'000'000;
        lrvx::replay::BinaryLogReader reader(cfg);
        ```

2. **Pre-sort the tape.** Re-process the segment through `BinaryLogWriter` (which sets the Sorted flag and the reader's fast path takes over). This is the right fix for archived data that won't change.

3. **Investigate the source.** A delta in the seconds-to-minutes range usually means an exchange reconnect or feed glitch. Beyond that range, the producer may genuinely be writing unsorted output and needs fixing upstream.

## Common causes

- Exchange websocket reconnect after a long network blip delivered a batch of stale events into a fresh tape block.
- The tape was produced by an external recorder that didn't sort events at write time and a particular trading session had a tail wider than the default 10 s window.
- The reader is configured with `reorder_window_ns = 0` while the source is not Sorted-flagged.

## Memory cost of a larger window

The reorder buffer holds up to `reorder_window_ns × peak_event_rate × sizeof(ReplayEvent)` bytes at any moment. At 10 s × 10 k events/s × ~360 B ≈ 36 MB. Doubling the window doubles the worst-case buffer; even 60 s on a busy multi-symbol feed stays under 250 MB.
