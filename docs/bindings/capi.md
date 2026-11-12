# C API

`lrvx_capi.h` is a C interface to the lrvx engine. All existing bindings (Python, Node.js, Codon, embedded JS) use it. If you're adding support for a new language or embedding lrvx in a C project, start here.

## Build

```bash
cmake -B build \
  -DLRVX_BUILD_CAPI=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

Produces `build/src/capi/liblrvx_capi.so`.

## Header

```c
#include "lrvx/capi/lrvx_capi.h"
```

## Minimal example

```c
#include "lrvx/capi/lrvx_capi.h"
#include <stdio.h>

static void on_trade(void* user_data, const LrvxSymbolContext* ctx,
                     const LrvxTradeData* trade) {
    double price = lrvx_price_to_double(trade->price_raw);
    printf("trade: %.2f\n", price);
}

static void on_signal(void* user_data, const LrvxSignal* sig) {
    printf("signal: %s qty=%.4f\n",
           sig->side == 0 ? "buy" : "sell",
           sig->quantity);
}

int main(void) {
    LrvxRegistryHandle reg = lrvx_registry_create();
    uint32_t btc = lrvx_registry_add_symbol(reg, "binance", "BTCUSDT", 0.01);

    LrvxStrategyCallbacks cbs = {0};
    cbs.on_trade = on_trade;

    LrvxStrategyHandle strat = lrvx_strategy_create(0, &btc, 1, reg, cbs);

    LrvxRunnerHandle runner = lrvx_runner_create(reg, on_signal, NULL);
    lrvx_runner_add_strategy(runner, strat);
    lrvx_runner_start(runner);

    // inject a tick
    lrvx_runner_on_trade(runner, btc, 67000.0, 0.01, 1, 0);

    lrvx_runner_stop(runner);
    lrvx_runner_destroy(runner);
    lrvx_strategy_destroy(strat);
    lrvx_registry_destroy(reg);
    return 0;
}
```

Compile:

```bash
gcc -o example example.c \
  -I/path/to/lrvx/include \
  -L/path/to/build/src/capi \
  -llrvx_capi \
  -Wl,-rpath,/path/to/build/src/capi
```

## Calling contract

A C caller cannot catch a C++ exception and cannot tell a live handle from a
stale integer. Both of those are settled here, the same way on all 735
exported functions, and written down in the header.

### Handles and NULL

Every `lrvx*Handle` is an opaque pointer owned by whoever created it. Passing
`NULL` where a handle is expected is safe everywhere: the call does nothing
and returns the value it documents for failure. That is zero for the integer
and floating-point returns, `NULL` for handles and strings, an all-zero
struct for the by-value struct returns. Destroying `NULL` is a no-op too.

Arguments that are not handles (paths, names, output buffers) are checked
where the header says so. The entry guard does not cover them.

### Ownership

A function whose name ends in `_create` returns a handle you own and must
pass to the matching `_destroy`. An accessor that reaches inside a composite
returns a borrowed handle instead:

```c
LrvxVenueStackHandle stack = lrvx_venue_stack_create(0, 42, 10000.0);

LrvxAccountHandle account = lrvx_venue_stack_account(stack);   /* borrowed */
LrvxSimulatedExecutorHandle exec = lrvx_venue_stack_executor(stack);

lrvx_account_destroy(account);   /* no-op: the stack still owns it */
lrvx_venue_stack_destroy(stack); /* frees the account and the executor */
```

A borrowed handle stays valid while the composite lives, and `_destroy` on
one does nothing. So a wrapper that puts every handle it receives into a
finaliser is safe to write.

### Exceptions and the last error

No exception crosses the boundary. Unwinding out of a frame with C linkage is
undefined behaviour, so every exported function catches, returns its failure
value, and records what happened for the calling thread:

```c
LrvxDataWriterHandle w = lrvx_data_writer_create("/read/only/path", 0, 0);
if (w == NULL) {
    printf("%d: %s\n", lrvx_last_error_code(), lrvx_last_error_message());
}
```

`lrvx_last_error_code()` returns 0 when nothing has been recorded, 1 for a
NULL handle or a NULL required argument, and 2 for an exception stopped at
the boundary. Read it straight after a call that reported failure and nowhere
else: a successful call does not clear it. `lrvx_clear_last_error()` is there
if you want a clean slate first. The slot is per thread, so one thread's
failure never shows up on another.

That rule covers calls *into* the library. The callbacks you register — the
strategy callbacks, the hooks, the pre-trade gates — run in the other
direction, and there the boundary belongs to the caller: an exception raised
in your callback must stop inside it, and a gate that could not answer must
return its failure value, 0, which drops the signal. The Python binding
implements exactly that; see
[When a callback raises](python.md#when-a-callback-raises) for how the
description is carried out of the callback when there is no
`lrvx_last_error_*` to read.

### ABI version

The header declares `LRVX_CAPI_ABI_VERSION` and the library reports
`lrvx_capi_abi_version()`. Every struct here is packed with no reserved tail,
so a header from one release used against a library from another gives you
wrong numbers instead of a failed load. Check the pair once at startup:

```c
if (lrvx_capi_abi_version() != LRVX_CAPI_ABI_VERSION) {
    fprintf(stderr, "lrvx_capi ABI mismatch\n");
    return 1;
}
```

C++ callers get the same comparison from `lrvx/capi/abi_check.hpp`:
`lrvx::capi::checkAbiVersion(LRVX_CAPI_ABI_VERSION, &message)` returns
false on a mismatch and fills `message` with both versions. Every shipped
binding runs it at load and refuses to come up on a mismatch -- the
Python extension raises `ImportError`, the Node addon throws from
`require`, `registerLrvxBindings` registers nothing and leaves the
refusal on the QuickJS context, and the Codon package exits. Each also
exports the pair it compared (`CAPI_ABI_VERSION` / `capi_abi_version()`,
`__LRVX_CAPI_ABI_VERSION` / `__lrvx_capi_abi_version()`).

The shared library also carries a real `SOVERSION` now, so the platform
loader refuses a mismatched major/minor before any of this runs.

### Threads

Handles are not synchronised. Two threads may use two different handles
freely; sharing one handle between threads is yours to serialise.

Results the library keeps for a follow-up call belong to the handle, not to
the calling thread. The delta-book encoder's level lists and the portfolio
risk breach list can both be produced on one thread and read on another:

```c
lrvx_delta_book_encoder_encode(enc, sym, bids, 2, asks, 2,
                               &is_delta, &n_bids, &n_asks);
/* another thread */
lrvx_delta_book_encoder_copy_bids(enc, out, 4);
```

The strings a `LrvxBreach` points at follow the same rule. They live until
the next call that refills the breach list for that handle, or until the
handle is destroyed. Copy them out if they have to outlive that.

## Streaming indicators

`lrvx_indicator_<name>` computes a whole array. For one value at a time, a
`LrvxStreamingIndicatorHandle` owns an instance of the C++ streaming
indicator; every row of `include/lrvx/indicator/registry.def` can be created
by its snake_case name, with that row's constructor arguments passed as
doubles:

```c
double args[] = {20};
LrvxStreamingIndicatorHandle sma = lrvx_streaming_indicator_create("sma", args, 1);
for (size_t i = 0; i < n; ++i) {
    double v = lrvx_streaming_indicator_update(sma, close[i]);  /* NaN until ready */
}
lrvx_streaming_indicator_destroy(sma);
```

An update is one step of the C++ class: no history is kept, its cost does
not depend on how many values the handle has seen, and update, value,
output, ready and count never allocate. Values, readiness and NaN behaviour
are the class's, bit for bit. Each indicator takes the update for its inputs
(`_update`, `_update_bar`, `_update_high_low`, `_update_ohlc` or
`_update_pair`); `_output(h, i)` reads the extra outputs of MACD, Bollinger
and Stochastic. A bad name or argument returns NULL with the reason in
`lrvx_last_error_message()`. The Codon and QuickJS indicator classes are
built on these handles.

## Full API reference

See [C API Reference](../reference/api/capi/lrvx_capi.md) for all functions, structs, and callback signatures.
