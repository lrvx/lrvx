#pragma once

// lrvx/indicator/params.h
//
// The smallest value each integer constructor argument of a registry row can
// take: below it the indicator's window has no defined value (an empty window,
// a standard deviation over one point, a slope over no step). The C++ classes
// accept any value and answer NaN for a window they cannot fill, which keeps
// their compute() safe for any input; every binding and the C API check here
// instead, so a construction or a batch call with such an argument fails with
// E_IND_001 rather than returning an all-NaN result nobody asked for.
//
// Rows are named as in registry.def (snake case). Arguments are given in the
// row's order, integer arguments only (Bollinger's multiplier is a double and
// is not range-checked here).

#include "lrvx/error/lrvx_error.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>

namespace lrvx::indicator
{

struct ParamFloor
{
  std::string_view param;
  size_t min;
};

struct RowParamFloors
{
  std::string_view row;
  std::array<ParamFloor, 3> floors;
  size_t count;
};

inline constexpr RowParamFloors kRowParamFloors[] = {
    {"ema", {{{"period", 1}}}, 1},
    {"sma", {{{"period", 1}}}, 1},
    {"rma", {{{"period", 1}}}, 1},
    {"rsi", {{{"period", 1}}}, 1},
    {"kama", {{{"period", 1}, {"fast", 1}, {"slow", 1}}}, 3},
    {"dema", {{{"period", 1}}}, 1},
    {"tema", {{{"period", 1}}}, 1},
    {"slope", {{{"length", 1}}}, 1},
    {"skewness", {{{"period", 3}}}, 1},
    {"kurtosis", {{{"period", 4}}}, 1},
    {"rolling_zscore", {{{"period", 2}}}, 1},
    {"shannon_entropy", {{{"period", 2}, {"bins", 2}}}, 2},
    {"autocorrelation", {{{"window", 2}, {"lag", 1}}}, 2},
    {"atr", {{{"period", 1}}}, 1},
    {"cci", {{{"period", 1}}}, 1},
    {"chop", {{{"period", 2}}}, 1},
    {"stochastic", {{{"k_period", 1}, {"d_period", 1}}}, 2},
    {"parkinson_vol", {{{"period", 1}}}, 1},
    {"rogers_satchell_vol", {{{"period", 1}}}, 1},
    {"correlation", {{{"period", 2}}}, 1},
    {"macd", {{{"fast", 1}, {"slow", 1}, {"signal", 1}}}, 3},
    {"bollinger", {{{"period", 1}}}, 1},
};

inline const RowParamFloors* findRowParamFloors(std::string_view row) noexcept
{
  for (const auto& r : kRowParamFloors)
  {
    if (r.row == row)
    {
      return &r;
    }
  }
  return nullptr;
}

// Throws LrvxError E_IND_001 naming the first integer argument below its
// floor. A row without an entry, or more arguments than the row has floors
// for, is a programming error in the caller and throws std::logic_error.
inline void requireParams(std::string_view row, std::initializer_list<size_t> args)
{
  const RowParamFloors* r = findRowParamFloors(row);
  if (r == nullptr || args.size() != r->count)
  {
    throw std::logic_error("indicator parameter check: no floors for '" + std::string(row) +
                           "' with " + std::to_string(args.size()) + " argument(s)");
  }
  size_t i = 0;
  for (size_t v : args)
  {
    const ParamFloor& f = r->floors[i++];
    if (v < f.min)
    {
      throw LrvxError("E_IND_001", std::string(row) + ": " + std::string(f.param) +
                                       " must be at least " + std::to_string(f.min) +
                                       ", got " + std::to_string(v) + ".");
    }
  }
}

// The same check over a constructor's arguments as written, whatever their
// types: the integer ones are checked in order, the others are skipped.
template <typename... A>
void requireArgs(std::string_view row, A... args)
{
  std::array<size_t, sizeof...(A) + 1> ints{};
  size_t n = 0;
  (
      [&](auto v)
      {
        if constexpr (std::is_integral_v<decltype(v)>)
        {
          ints[n++] = static_cast<size_t>(v);
        }
      }(args),
      ...);
  switch (n)
  {
    case 1:
      requireParams(row, {ints[0]});
      break;
    case 2:
      requireParams(row, {ints[0], ints[1]});
      break;
    case 3:
      requireParams(row, {ints[0], ints[1], ints[2]});
      break;
    default:
      requireParams(row, {});
  }
}

}  // namespace lrvx::indicator
