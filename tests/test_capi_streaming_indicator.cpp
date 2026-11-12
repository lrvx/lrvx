// tests/test_capi_streaming_indicator.cpp
//
// lrvx_streaming_indicator_* against the C++ streaming classes.
//
// For every row of include/lrvx/indicator/registry.def, a handle and an
// instance of the class are fed the same 400 steps, NaNs included, and must
// agree bit for bit at every step on the value update returns, value(), every
// output, ready() and count(). Then both are reset and fed again, which must
// repeat the first run exactly, and the handle is destroyed.
//
// The registry drives the cases: a row with no constructor arguments in
// kCases (tests/support/streaming_indicator_cases.h) fails
// RegistryRowsAllHaveACase, so a new indicator cannot be
// added without its handle being exercised here.

#include "lrvx/capi/lrvx_capi.h"

#include "support/streaming_indicator_cases.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
std::atomic<size_t> gAllocations{0};
}  // namespace

// Allocation counting replaces the global operator new, which the sanitizers
// interpose themselves; the counting case skips under them.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LRVX_TEST_UNDER_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define LRVX_TEST_UNDER_SANITIZER 1
#endif
#endif

#ifndef LRVX_TEST_UNDER_SANITIZER
#define LRVX_TEST_UNDER_SANITIZER 0
#endif

#if !LRVX_TEST_UNDER_SANITIZER
void* operator new(std::size_t size)
{
  gAllocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(size == 0 ? 1 : size))
  {
    return p;
  }
  throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

using namespace lrvx::test_streaming;

namespace
{

double stepHandle(LrvxStreamingIndicatorHandle h, uint32_t inputs, bool pair, const Step& st)
{
  switch (inputs)
  {
    case 1:
      return lrvx_streaming_indicator_update(h, st.close);
    case 2:
      return pair ? lrvx_streaming_indicator_update_pair(h, st.x, st.y)
                  : lrvx_streaming_indicator_update_high_low(h, st.high, st.low);
    case 3:
      return lrvx_streaming_indicator_update_bar(h, st.high, st.low, st.close);
    default:
      return lrvx_streaming_indicator_update_ohlc(h, st.open, st.high, st.low, st.close);
  }
}

// Feeds `steps` through both and compares everything at every step. Returns
// the per-step outputs of the handle so a second run can be compared too.
std::vector<std::vector<uint64_t>> feedAndCompare(const std::string& snake,
                                                  LrvxStreamingIndicatorHandle h, CppSide& cpp,
                                                  const std::vector<Step>& steps,
                                                  size_t minReadySteps)
{
  const bool pair = isPairRow(snake);
  const uint32_t outputs = lrvx_streaming_indicator_output_count(h);
  std::vector<std::vector<uint64_t>> trace;
  size_t mismatches = 0;
  size_t readySteps = 0;
  for (size_t i = 0; i < steps.size(); ++i)
  {
    const double fromHandle = stepHandle(h, cpp.inputs, pair, steps[i]);
    const double fromCpp = cpp.step(steps[i]);
    const auto expected = cpp.outputs();
    std::vector<uint64_t> row;
    row.push_back(bits(fromHandle));
    if (bits(fromHandle) != bits(fromCpp) ||
        bits(lrvx_streaming_indicator_value(h)) != bits(expected[0]))
    {
      if (mismatches++ < 5)
      {
        ADD_FAILURE() << snake << " step " << i << ": value " << fromHandle << " vs C++ "
                      << fromCpp;
      }
    }
    for (uint32_t o = 0; o < outputs && o < expected.size(); ++o)
    {
      const double got = lrvx_streaming_indicator_output(h, o);
      row.push_back(bits(got));
      if (bits(got) != bits(expected[o]) && mismatches++ < 5)
      {
        ADD_FAILURE() << snake << " step " << i << ": output " << o << " " << got << " vs C++ "
                      << expected[o];
      }
    }
    const bool ready = lrvx_streaming_indicator_ready(h) != 0;
    readySteps += ready ? 1 : 0;
    if (ready != cpp.ready() && mismatches++ < 5)
    {
      ADD_FAILURE() << snake << " step " << i << ": ready " << ready << " vs C++ " << cpp.ready();
    }
    if (lrvx_streaming_indicator_count(h) != cpp.count() && mismatches++ < 5)
    {
      ADD_FAILURE() << snake << " step " << i << ": count " << lrvx_streaming_indicator_count(h)
                    << " vs C++ " << cpp.count();
    }
    trace.push_back(std::move(row));
  }
  EXPECT_EQ(mismatches, 0u) << snake;
  // A case that never becomes ready compares NaN with NaN all the way and
  // proves nothing. Every case warms up well inside the series; some (MACD,
  // KAMA) stay NaN for good after their first NaN input, so the bar is lower
  // on the NaN series.
  EXPECT_GT(readySteps, minReadySteps) << snake << " was ready on too few steps";
  return trace;
}

}  // namespace

namespace
{
void checkEveryRow(const std::vector<Step>& steps, size_t minReadySteps);
}  // namespace

TEST(CapiStreamingIndicator, RegistryRowsAllHaveACase)
{
  EXPECT_EQ(kRegistry.size(), kCases.size());
  for (const auto& row : kRegistry)
  {
    EXPECT_TRUE(kCases.count(row.snake)) << "registry row " << row.cls << " (" << row.snake
                                         << ") has no case in kCases: add its constructor "
                                            "arguments so its handle is tested";
  }
  for (const auto& [snake, args] : kCases)
  {
    bool found = false;
    for (const auto& row : kRegistry)
    {
      found = found || snake == row.snake;
    }
    EXPECT_TRUE(found) << "kCases names " << snake << ", which is not a registry row";
  }
}

TEST(CapiStreamingIndicator, EveryRowMatchesTheCppClassBitForBit)
{
  for (const bool nans : {false, true})
  {
    SCOPED_TRACE(nans ? "series with NaNs" : "clean series");
    checkEveryRow(series(400, nans), nans ? 10 : 200);
  }
}

namespace
{
void checkEveryRow(const std::vector<Step>& steps, size_t minReadySteps)
{
  for (const auto& row : kRegistry)
  {
    const auto it = kCases.find(row.snake);
    ASSERT_NE(it, kCases.end()) << row.snake;
    const auto& args = it->second;

    LrvxStreamingIndicatorHandle h = lrvx_streaming_indicator_create(
        row.snake, args.data(), static_cast<uint32_t>(args.size()));
    ASSERT_NE(h, nullptr) << row.snake << ": " << lrvx_last_error_message();
    CppSide cpp = makeCppSide(row.snake, args);

    EXPECT_EQ(lrvx_streaming_indicator_input_count(h), cpp.inputs) << row.snake;
    EXPECT_EQ(lrvx_streaming_indicator_output_count(h), cpp.outputs().size()) << row.snake;
    EXPECT_EQ(lrvx_streaming_indicator_count(h), 0u) << row.snake;
    EXPECT_EQ(lrvx_streaming_indicator_ready(h), 0) << row.snake;
    EXPECT_TRUE(std::isnan(lrvx_streaming_indicator_value(h))) << row.snake;

    const auto first = feedAndCompare(row.snake, h, cpp, steps, minReadySteps);

    lrvx_streaming_indicator_reset(h);
    cpp.reset();
    EXPECT_EQ(lrvx_streaming_indicator_count(h), 0u) << row.snake;
    EXPECT_EQ(lrvx_streaming_indicator_ready(h), 0) << row.snake;
    EXPECT_TRUE(std::isnan(lrvx_streaming_indicator_value(h))) << row.snake;

    const auto second = feedAndCompare(row.snake, h, cpp, steps, minReadySteps);
    EXPECT_EQ(first, second) << row.snake << ": a reset handle does not repeat its first run";

    lrvx_streaming_indicator_destroy(h);
  }
}
}  // namespace

// One handle per registry row stepped many times must not allocate: the
// class reserves everything at construction, and the C layer adds nothing.
TEST(CapiStreamingIndicator, UpdateAndReadersDoNotAllocate)
{
#if LRVX_TEST_UNDER_SANITIZER
  GTEST_SKIP() << "allocation counting replaces global operator new, which the sanitizers "
                  "interpose";
#else
  const auto steps = series(400, true);
  for (const auto& row : kRegistry)
  {
    const auto& args = kCases.at(row.snake);
    const size_t beforeCreate = gAllocations.load();
    LrvxStreamingIndicatorHandle h = lrvx_streaming_indicator_create(
        row.snake, args.data(), static_cast<uint32_t>(args.size()));
    ASSERT_NE(h, nullptr) << row.snake;
    if (gAllocations.load() == beforeCreate)
    {
      GTEST_SKIP() << "this platform does not route the library's operator new through the "
                      "test's replacement, so allocations cannot be counted here";
    }
    const uint32_t inputs = lrvx_streaming_indicator_input_count(h);
    const uint32_t outputs = lrvx_streaming_indicator_output_count(h);
    const bool pair = isPairRow(row.snake);
    double acc = 0.0;
    const size_t before = gAllocations.load();
    for (int pass = 0; pass < 3; ++pass)
    {
      for (const auto& st : steps)
      {
        acc += stepHandle(h, inputs, pair, st);
        acc += lrvx_streaming_indicator_value(h);
        for (uint32_t o = 0; o < outputs; ++o)
        {
          acc += lrvx_streaming_indicator_output(h, o);
        }
        acc += lrvx_streaming_indicator_ready(h);
        acc += static_cast<double>(lrvx_streaming_indicator_count(h));
      }
      lrvx_streaming_indicator_reset(h);
    }
    EXPECT_EQ(gAllocations.load() - before, 0u) << row.snake << " allocated while streaming";
    lrvx_streaming_indicator_destroy(h);
    volatile double sink = acc;
    (void)sink;
  }
#endif
}

TEST(CapiStreamingIndicator, NullHandleReturnsZeroAndRecordsCode1)
{
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_streaming_indicator_update(nullptr, 1.0), 0.0);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_streaming_indicator_update_bar(nullptr, 1.0, 1.0, 1.0), 0.0);
  EXPECT_EQ(lrvx_streaming_indicator_update_high_low(nullptr, 1.0, 1.0), 0.0);
  EXPECT_EQ(lrvx_streaming_indicator_update_ohlc(nullptr, 1.0, 1.0, 1.0, 1.0), 0.0);
  EXPECT_EQ(lrvx_streaming_indicator_update_pair(nullptr, 1.0, 1.0), 0.0);
  EXPECT_EQ(lrvx_streaming_indicator_value(nullptr), 0.0);
  EXPECT_EQ(lrvx_streaming_indicator_output(nullptr, 0), 0.0);
  EXPECT_EQ(lrvx_streaming_indicator_ready(nullptr), 0);
  EXPECT_EQ(lrvx_streaming_indicator_count(nullptr), 0u);
  EXPECT_EQ(lrvx_streaming_indicator_input_count(nullptr), 0u);
  EXPECT_EQ(lrvx_streaming_indicator_output_count(nullptr), 0u);
  lrvx_streaming_indicator_reset(nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  lrvx_clear_last_error();
  lrvx_streaming_indicator_destroy(nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 0) << "destroying NULL is not an error";
}

TEST(CapiStreamingIndicator, BadCreateReturnsNullWithAReason)
{
  const double one[] = {14};
  const double two[] = {14, 3};

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_streaming_indicator_create(nullptr, one, 1), nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 1);

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_streaming_indicator_create("no_such_indicator", one, 1), nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("no_such_indicator"), std::string::npos);

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_streaming_indicator_create("SMA", one, 1), nullptr)
      << "names are the registry's snake_case names";
  EXPECT_EQ(lrvx_last_error_code(), 2);

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_streaming_indicator_create("sma", two, 2), nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  EXPECT_EQ(lrvx_streaming_indicator_create("sma", nullptr, 1), nullptr);
  EXPECT_EQ(lrvx_streaming_indicator_create("sma", one, 0), nullptr);

  for (double bad : {-1.0, 2.5, std::nan(""), std::numeric_limits<double>::infinity(), 1e300})
  {
    lrvx_clear_last_error();
    EXPECT_EQ(lrvx_streaming_indicator_create("sma", &bad, 1), nullptr) << bad;
    EXPECT_EQ(lrvx_last_error_code(), 2) << bad;
  }

  // A double argument takes any value the class accepts.
  const double boll[] = {20, 2.5};
  LrvxStreamingIndicatorHandle h = lrvx_streaming_indicator_create("bollinger", boll, 2);
  EXPECT_NE(h, nullptr);
  lrvx_streaming_indicator_destroy(h);
}

TEST(CapiStreamingIndicator, WrongUpdateDoesNotStep)
{
  const double args[] = {3};
  LrvxStreamingIndicatorHandle sma = lrvx_streaming_indicator_create("sma", args, 1);
  LrvxStreamingIndicatorHandle pv = lrvx_streaming_indicator_create("parkinson_vol", args, 1);
  ASSERT_NE(sma, nullptr);
  ASSERT_NE(pv, nullptr);

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_streaming_indicator_update_bar(sma, 2.0, 1.0, 1.5), 0.0);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("lrvx_streaming_indicator_update"),
            std::string::npos);
  EXPECT_EQ(lrvx_streaming_indicator_count(sma), 0u);

  // Same arity, different kind: a pair is not a high/low.
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_streaming_indicator_update_pair(pv, 2.0, 1.0), 0.0);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  EXPECT_EQ(lrvx_streaming_indicator_count(pv), 0u);
  lrvx_streaming_indicator_update_high_low(pv, 2.0, 1.0);
  EXPECT_EQ(lrvx_streaming_indicator_count(pv), 1u);

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_streaming_indicator_output(sma, 1), 0.0);
  EXPECT_EQ(lrvx_last_error_code(), 2);

  lrvx_streaming_indicator_destroy(sma);
  lrvx_streaming_indicator_destroy(pv);
}
