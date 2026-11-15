# History and crossings

`lrvx/strategy/history.h`, namespace `lrvx`. A fixed-capacity ring of the last values of a series, and the two crossing tests a bar strategy writes by hand otherwise.

## History

```cpp
explicit History(std::size_t capacity);  // std::invalid_argument for 0
void push(double value);
double operator[](std::size_t index) const;  // unchecked
double at(std::size_t index) const;          // std::out_of_range when index >= size()
std::size_t size() const;
std::size_t capacity() const;
bool full() const;
```

Indexed newest first: `h[0]` is the value pushed last, `h[1]` the one before. Once `size()` reaches `capacity()`, a push drops the oldest. A NaN takes a slot and reads back as NaN. The ring is allocated in the constructor; `push` does not allocate.

## crossedAbove / crossedBelow

```cpp
bool crossedAbove(double aPrev, double a, double bPrev, double b);
bool crossedBelow(double aPrev, double a, double bPrev, double b);
```

`crossedAbove` is true when `a` was at or below `b` on the previous step and is strictly above it now (`aPrev <= bPrev && a > b`); `crossedBelow` is the mirror (`aPrev >= bPrev && a < b`). Touching the other series is not a cross; leaving it from an equal start is. For a fixed level, pass it as both `bPrev` and `b`. A NaN anywhere is no cross.

```cpp
lrvx::History fast(2), slow(2);
// per bar:
fast.push(fastMa.value());
slow.push(slowMa.value());
if (fast.full() && lrvx::crossedAbove(fast[1], fast[0], slow[1], slow[0]))
{
  // golden cross
}
```

## Bindings

| Binding | History | Crossings |
|---|---|---|
| Python | `lrvx.History(capacity)`: `push`, `get(i)`, `h[i]`, `len(h)`, `capacity`, `full`; `IndexError` / `ValueError` | `lrvx.crossed_above`, `lrvx.crossed_below` (`None` is no cross) |
| Node | `new History(capacity)`: `push`, `get(i)`, `size`, `capacity`, `full`; `RangeError` | `crossedAbove`, `crossedBelow` (`null` is no cross) |
| QuickJS | `new History(capacity)`: `push`, `get(i)`, `length`, `capacity`, `full`; `RangeError` | `crossedAbove`, `crossedBelow` |
| Codon | `lrvx.history.History(capacity)`: `push`, `get(i)`, `h[i]`, `len(h)`, `capacity`, `full` | `lrvx.history.crossed_above`, `crossed_below` |
| C API | `LrvxHistoryHandle`, `lrvx_history_*` | `lrvx_crossed_above`, `lrvx_crossed_below` |
