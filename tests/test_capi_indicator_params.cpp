/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Integer arguments of the registry indicators below their floor
// (lrvx/indicator/params.h) are refused through the C API: the streaming
// handle is not created, and a batch function writes nothing and records
// E_IND_001. The C++ classes themselves stay permissive (their compute()
// answers NaN for a window they cannot fill; see test_indicators.cpp,
// *ZeroPeriodIsSafeNotOutOfBounds), and arguments at the floor behave
// exactly as before.

#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/error/lrvx_error.h"
#include "lrvx/indicator/params.h"
#include "lrvx/indicator/sma.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

using lrvx::indicator::findRowParamFloors;
using lrvx::indicator::requireParams;

namespace
{

template <typename... A>
constexpr size_t integerArgs(std::type_identity<void(A...)>)
{
  return (0 + ... + (std::is_integral_v<A> ? 1 : 0));
}

struct Row
{
  const char* snake;
  size_t ints;
  size_t all;
};

template <typename... A>
constexpr size_t allArgs(std::type_identity<void(A...)>)
{
  return sizeof...(A);
}

const Row kRows[] = {
#define LRVX_INDICATOR(Cls, snake, Kind, Args) \
  {#snake, integerArgs(std::type_identity<void Args>{}), allArgs(std::type_identity<void Args>{})},
#include "lrvx/indicator/registry.def"
#undef LRVX_INDICATOR
};

// The row's constructor arguments with the integer ones at their floor, and
// one of them (`below`, counted among the integer arguments) one under it.
std::vector<double> argsFor(const char* snake, size_t all, long below)
{
  const auto* f = findRowParamFloors(snake);
  std::vector<double> args;
  size_t k = 0;
  for (size_t i = 0; i < all; ++i)
  {
    if (std::string(snake) == "bollinger" && i == 1)
    {
      args.push_back(2.0);  // the multiplier, a double
      continue;
    }
    const size_t min = f->floors[k].min;
    args.push_back(static_cast<double>(static_cast<long>(k) == below ? min - 1 : min));
    ++k;
  }
  return args;
}

}  // namespace

TEST(IndicatorParams, EveryRegistryRowHasAFloorPerIntegerArgument)
{
  for (const auto& row : kRows)
  {
    const auto* f = findRowParamFloors(row.snake);
    ASSERT_NE(f, nullptr) << row.snake << " has no entry in lrvx/indicator/params.h";
    EXPECT_EQ(f->count, row.ints) << row.snake;
    for (size_t i = 0; i < f->count; ++i)
    {
      EXPECT_GE(f->floors[i].min, 1u) << row.snake << " " << f->floors[i].param;
    }
  }
  EXPECT_EQ(std::size(lrvx::indicator::kRowParamFloors), std::size(kRows));
}

TEST(IndicatorParams, BelowTheFloorThrowsE_IND_001)
{
  EXPECT_NO_THROW(requireParams("sma", {1}));
  try
  {
    requireParams("chop", {1});
    FAIL() << "chop(1) accepted";
  }
  catch (const lrvx::LrvxError& e)
  {
    EXPECT_EQ(e.code(), "E_IND_001");
    EXPECT_EQ(e.message(), "chop: period must be at least 2, got 1.");
  }
  EXPECT_THROW(requireParams("macd", {12, 0, 9}), lrvx::LrvxError);
  EXPECT_THROW(requireParams("no_such_row", {1}), std::logic_error);
}

TEST(IndicatorParams, CppClassesStayPermissive)
{
  // The binding check is not a C++ constructor check: SMA(0) still builds and
  // still computes all-NaN, the contract test_indicators.cpp pins.
  lrvx::indicator::SMA sma(0);
  const std::vector<double> in{1, 2, 3};
  for (double v : sma.compute(in))
  {
    EXPECT_TRUE(std::isnan(v));
  }
}

TEST(IndicatorParams, StreamingHandleRefusesBelowTheFloor)
{
  for (const auto& row : kRows)
  {
    SCOPED_TRACE(row.snake);
    auto ok = argsFor(row.snake, row.all, -1);
    lrvx_clear_last_error();
    LrvxStreamingIndicatorHandle h =
        lrvx_streaming_indicator_create(row.snake, ok.data(), static_cast<uint32_t>(ok.size()));
    ASSERT_NE(h, nullptr) << lrvx_last_error_message();
    lrvx_streaming_indicator_destroy(h);

    for (long k = 0; k < static_cast<long>(row.ints); ++k)
    {
      auto bad = argsFor(row.snake, row.all, k);
      lrvx_clear_last_error();
      EXPECT_EQ(lrvx_streaming_indicator_create(row.snake, bad.data(),
                                                static_cast<uint32_t>(bad.size())),
                nullptr)
          << "argument " << k;
      EXPECT_EQ(lrvx_last_error_code(), 2);
      EXPECT_NE(std::strstr(lrvx_last_error_message(), "must be at least"), nullptr)
          << lrvx_last_error_message();
    }
  }
}

TEST(IndicatorParams, BatchFunctionsWriteNothingBelowTheFloor)
{
  const std::vector<double> in{1, 2, 3, 4, 5, 6, 7, 8};
  const size_t n = in.size();
  std::vector<double> a(n, 7.0), b(n, 7.0), c(n, 7.0);
  auto untouched = [&](const std::vector<double>& v)
  {
    for (double x : v)
    {
      if (x != 7.0)
      {
        return false;
      }
    }
    return true;
  };
  struct Call
  {
    const char* name;
    std::function<void()> bad;
    std::function<void()> good;
  };
  const double* p = in.data();
  const std::vector<Call> calls = {
      {"sma", [&]
       { lrvx_indicator_sma(p, n, 0, a.data()); }, [&]
       { lrvx_indicator_sma(p, n, 1, a.data()); }},
      {"ema", [&]
       { lrvx_indicator_ema(p, n, 0, a.data()); }, [&]
       { lrvx_indicator_ema(p, n, 1, a.data()); }},
      {"kama", [&]
       { lrvx_indicator_kama(p, n, 3, 0, 30, a.data()); },
       [&]
       { lrvx_indicator_kama(p, n, 3, 1, 30, a.data()); }},
      {"rolling_zscore", [&]
       { lrvx_indicator_rolling_zscore(p, n, 1, a.data()); },
       [&]
       { lrvx_indicator_rolling_zscore(p, n, 2, a.data()); }},
      {"shannon_entropy", [&]
       { lrvx_indicator_shannon_entropy(p, n, 4, 0, a.data()); },
       [&]
       { lrvx_indicator_shannon_entropy(p, n, 4, 2, a.data()); }},
      {"chop", [&]
       { lrvx_indicator_chop(p, p, p, n, 1, a.data()); },
       [&]
       { lrvx_indicator_chop(p, p, p, n, 2, a.data()); }},
      {"correlation", [&]
       { lrvx_indicator_correlation(p, p, n, 1, a.data()); },
       [&]
       { lrvx_indicator_correlation(p, p, n, 2, a.data()); }},
      {"macd", [&]
       { lrvx_indicator_macd(p, n, 2, 3, 0, a.data(), b.data(), c.data()); },
       [&]
       { lrvx_indicator_macd(p, n, 2, 3, 1, a.data(), b.data(), c.data()); }},
      {"stochastic", [&]
       { lrvx_indicator_stochastic(p, p, p, n, 0, 3, a.data(), b.data()); },
       [&]
       { lrvx_indicator_stochastic(p, p, p, n, 1, 3, a.data(), b.data()); }},
      {"bollinger", [&]
       { lrvx_indicator_bollinger(p, n, 0, 2.0, a.data(), b.data(), c.data()); },
       [&]
       { lrvx_indicator_bollinger(p, n, 1, 2.0, a.data(), b.data(), c.data()); }},
  };
  for (const auto& call : calls)
  {
    SCOPED_TRACE(call.name);
    std::fill(a.begin(), a.end(), 7.0);
    std::fill(b.begin(), b.end(), 7.0);
    std::fill(c.begin(), c.end(), 7.0);
    lrvx_clear_last_error();
    call.bad();
    EXPECT_EQ(lrvx_last_error_code(), 2);
    EXPECT_NE(std::strstr(lrvx_last_error_message(), "must be at least"), nullptr)
        << lrvx_last_error_message();
    EXPECT_TRUE(untouched(a) && untouched(b) && untouched(c));

    lrvx_clear_last_error();
    call.good();
    EXPECT_EQ(lrvx_last_error_code(), 0) << lrvx_last_error_message();
    EXPECT_FALSE(untouched(a));
  }
}
