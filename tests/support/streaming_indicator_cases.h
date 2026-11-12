// tests/support/streaming_indicator_cases.h
//
// Shared by the tests that hold a binding's streaming indicators against the
// C++ classes (test_capi_streaming_indicator, test_quickjs): one constructor
// argument list per registry row, a 400-step series with and without NaNs,
// and a uniform view of the C++ class for each row.

#pragma once

#include "lrvx/indicator/atr.h"
#include "lrvx/indicator/autocorrelation.h"
#include "lrvx/indicator/bollinger.h"
#include "lrvx/indicator/cci.h"
#include "lrvx/indicator/correlation.h"
#include "lrvx/indicator/dema.h"
#include "lrvx/indicator/ema.h"
#include "lrvx/indicator/kama.h"
#include "lrvx/indicator/kurtosis.h"
#include "lrvx/indicator/macd.h"
#include "lrvx/indicator/parkinson_vol.h"
#include "lrvx/indicator/rma.h"
#include "lrvx/indicator/rogers_satchell_vol.h"
#include "lrvx/indicator/rolling_zscore.h"
#include "lrvx/indicator/rsi.h"
#include "lrvx/indicator/shannon_entropy.h"
#include "lrvx/indicator/skewness.h"
#include "lrvx/indicator/slope.h"
#include "lrvx/indicator/sma.h"
#include "lrvx/indicator/stochastic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace lrvx::test_streaming
{

using namespace lrvx::indicator;

// Constructor arguments per registry row, by snake_case name, in the
// registry's order.
inline const std::map<std::string, std::vector<double>> kCases = {
    {"ema", {14}},
    {"sma", {20}},
    {"rma", {14}},
    {"rsi", {14}},
    {"kama", {10, 2, 30}},
    {"dema", {10}},
    {"tema", {10}},
    {"slope", {12}},
    {"skewness", {20}},
    {"kurtosis", {20}},
    {"rolling_zscore", {30}},
    {"shannon_entropy", {20, 8}},
    {"autocorrelation", {20, 3}},
    {"atr", {14}},
    {"cci", {20}},
    {"stochastic", {14, 3}},
    {"parkinson_vol", {20}},
    {"rogers_satchell_vol", {20}},
    {"correlation", {20}},
    {"macd", {12, 26, 9}},
    {"bollinger", {20, 2.0}},
};

struct Row
{
  const char* cls;
  const char* snake;
  const char* kind;
};

inline const std::vector<Row> kRegistry = {
#define LRVX_INDICATOR(Cls, snake, Kind, Args) {#Cls, #snake, #Kind},
#include "lrvx/indicator/registry.def"
#undef LRVX_INDICATOR
};

struct Step
{
  double open, high, low, close, x, y;
};

// A random walk. With `nans`, every field is NaN at a few places, including
// a run of consecutive NaNs, so each indicator's NaN contract is exercised.
inline std::vector<Step> series(size_t n, bool nans)
{
  std::mt19937 rng(20261112);
  std::normal_distribution<double> walk(0.0, 1.0);
  std::uniform_real_distribution<double> spread(0.1, 2.0);
  std::vector<Step> out(n);
  double px = 100.0;
  double other = 50.0;
  for (size_t i = 0; i < n; ++i)
  {
    Step& s = out[i];
    s.open = px;
    px += walk(rng);
    other += 0.5 * walk(rng);
    s.close = px;
    s.high = std::max(s.open, s.close) + spread(rng);
    s.low = std::min(s.open, s.close) - spread(rng);
    s.x = px;
    s.y = other + 0.3 * px;
  }
  if (!nans)
  {
    return out;
  }
  const double nan = std::nan("");
  for (size_t i : {57u, 58u, 59u, 140u, 301u})
  {
    out[i].close = nan;
    out[i].x = nan;
  }
  for (size_t i : {90u, 222u})
  {
    out[i].high = nan;
    out[i].y = nan;
  }
  for (size_t i : {175u, 340u})
  {
    out[i].low = nan;
    out[i].open = nan;
  }
  return out;
}

inline uint64_t bits(double v)
{
  uint64_t b = 0;
  std::memcpy(&b, &v, sizeof b);
  return b;
}

// What the C++ side of one case looks like through a uniform interface:
// step, then read each output in the order the C API documents.
struct CppSide
{
  std::function<double(const Step&)> step;
  std::function<std::vector<double>()> outputs;
  std::function<bool()> ready;
  std::function<size_t()> count;
  std::function<void()> reset;
  uint32_t inputs = 0;
};

template <typename T>
std::vector<double> outputsOf(const T& ind)
{
  if constexpr (std::is_same_v<T, MACD>)
  {
    return {ind.value(), ind.signalValue(), ind.histogramValue()};
  }
  else if constexpr (std::is_same_v<T, Bollinger>)
  {
    return {ind.value(), ind.upperValue(), ind.lowerValue()};
  }
  else if constexpr (std::is_same_v<T, Stochastic>)
  {
    return {ind.value(), ind.dValue()};
  }
  else
  {
    return {ind.value()};
  }
}

template <typename T, typename... A, size_t... I>
std::shared_ptr<T> construct(std::type_identity<void(A...)>, std::index_sequence<I...>,
                             const std::vector<double>& args)
{
  return std::make_shared<T>(static_cast<A>(args[I])...);
}

template <typename T, typename... A>
CppSide cppSide(std::type_identity<void(A...)> sig, const char* kind,
                const std::vector<double>& args)
{
  auto ind = construct<T>(sig, std::index_sequence_for<A...>{}, args);
  CppSide s;
  const std::string k = kind;
  if constexpr (requires(T t) { t.update(0.0, 0.0, 0.0, 0.0); })
  {
    s.inputs = 4;
    s.step = [ind](const Step& st)
    {
      ind->update(st.open, st.high, st.low, st.close);
      return ind->value();
    };
  }
  else if constexpr (requires(T t) { t.update(0.0, 0.0, 0.0); })
  {
    s.inputs = 3;
    s.step = [ind](const Step& st)
    {
      ind->update(st.high, st.low, st.close);
      return ind->value();
    };
  }
  else if constexpr (requires(T t) { t.update(0.0, 0.0); })
  {
    s.inputs = 2;
    if (k == "PairInput")
    {
      s.step = [ind](const Step& st)
      {
        ind->update(st.x, st.y);
        return ind->value();
      };
    }
    else
    {
      s.step = [ind](const Step& st)
      {
        ind->update(st.high, st.low);
        return ind->value();
      };
    }
  }
  else
  {
    s.inputs = 1;
    s.step = [ind](const Step& st)
    {
      ind->update(st.close);
      return ind->value();
    };
  }
  s.outputs = [ind]
  { return outputsOf(*ind); };
  s.ready = [ind]
  { return ind->ready(); };
  s.count = [ind]
  { return ind->count(); };
  s.reset = [ind]
  { ind->reset(); };
  return s;
}

inline CppSide makeCppSide(const std::string& snake, const std::vector<double>& args)
{
#define LRVX_INDICATOR(Cls, sn, Kind, Args)                            \
  if (snake == #sn)                                                    \
  {                                                                    \
    return cppSide<Cls>(std::type_identity<void Args>{}, #Kind, args); \
  }
#include "lrvx/indicator/registry.def"
#undef LRVX_INDICATOR
  ADD_FAILURE() << "no registry row " << snake;
  return {};
}

inline bool isPairRow(const std::string& snake)
{
#define LRVX_INDICATOR(Cls, sn, Kind, Args)   \
  if (snake == #sn)                           \
  {                                           \
    return std::string(#Kind) == "PairInput"; \
  }
#include "lrvx/indicator/registry.def"
#undef LRVX_INDICATOR
  return false;
}

}  // namespace lrvx::test_streaming
