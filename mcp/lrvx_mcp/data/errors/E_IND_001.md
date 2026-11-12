---
code: E_IND_001
title: Indicator period must be positive
severity: error
since: 0.8.1
---

# E_IND_001 — Indicator period must be positive

An indicator was constructed or called with an integer parameter below the
smallest value its window is defined for. A zero-length window has nothing to
average over, a standard deviation over one point is undefined, so the call is
rejected instead of silently returning an all-NaN array.

Every indicator in the registry (`SMA`, `EMA`, `MACD`, `CHOP`, ...) checks its
integer parameters, in every binding: the class constructor and the batch
function both refuse them. The rolling `vwap` window is checked the same way.

| Indicator | Smallest value |
|---|---|
| `EMA`, `SMA`, `RMA`, `RSI`, `DEMA`, `TEMA`, `ATR`, `CCI`, `ParkinsonVol`, `RogersSatchellVol`, `Bollinger` | `period` 1 |
| `KAMA` | `period`, `fast`, `slow` 1 |
| `Slope` | `length` 1 |
| `MACD` | `fast`, `slow`, `signal` 1 |
| `Stochastic` | `k_period`, `d_period` 1 |
| `RollingZScore`, `Correlation`, `CHOP` | `period` 2 |
| `ShannonEntropy` | `period` 2, `bins` 2 |
| `AutoCorrelation` | `window` 2, `lag` 1 |
| `Skewness` | `period` 3 |
| `Kurtosis` | `period` 4 |

The table is `include/lrvx/indicator/params.h`. How the refusal surfaces:

- Python and Node: an `LrvxError` (Node: an `Error` with `code`) carrying
  `E_IND_001`, for example `chop: period must be at least 2, got 1.`
- QuickJS: a `TypeError` with the same text. Codon: a `ValueError`.
- C API: `lrvx_streaming_indicator_create` returns `NULL`; a batch
  function such as `lrvx_indicator_sma` writes nothing to its outputs. Both
  record code 2 with the text in `lrvx_last_error_message`; clear the error
  before a batch call to tell a refusal apart.

The C++ classes themselves accept any value and return NaN for a window they
cannot fill, so their `compute()` is safe on any input.

## How to fix

Pass a value at or above the indicator's floor:

=== "Python"
    ```python
    import lrvx

    # OK
    lrvx.sma(prices, period=20)

    # Raises E_IND_001
    # lrvx.sma(prices, period=0)
    # lrvx.CHOP(1)
    ```

## Common causes

- A period sourced from a config file or CLI argument that was left unset
  and defaulted to `0`.
- A parameter-search / optimizer bound that includes `0` (or `1` for a
  rolling standard deviation) in its range for a period argument.
- An off-by-one when deriving a period from another value (e.g.
  `fast_period - 1` when `fast_period` is `1`).
