/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// python/bindings_common.h
//
// Small helpers shared across the pybind11 binding headers, pulled out
// of indicator_bindings.h so aggregator_bindings.h, optimizer_bindings.h,
// profile_bindings.h, book_bindings.h, and composite_book_bindings.h can
// use them too without redefining the same function in lrvx.cpp's
// translation unit (every one of these headers opens an anonymous
// namespace; within one .cpp file those are all the SAME namespace, so
// two headers each defining `checkSameSize` would collide).
//
// The pattern below fixes one recurring defect class found across the
// binding layer: a numpy array argument declared as a bare
// `py::array_t<T>` accepts a non-contiguous view (a `[::2]` slice, a
// structured-array field, a 2D column) without copying it, and code
// that then walks the buffer with a raw pointer and a stride-1
// assumption reads the wrong elements -- or, for a reversed slice,
// reads off the front of the allocation entirely. `py::array_t<T,
// py::array::c_style | py::array::forcecast>` on the parameter type
// fixes that at the pybind11 layer by materialising a contiguous copy
// whenever the caller's array is not already one; checkSameSize below
// closes the companion defect, where a function reads its element
// count from a single one of several same-length input arrays and
// never checks that the others actually match.

#pragma once

#include <pybind11/pybind11.h>

#include "lrvx/common.h"
#include "lrvx/error/lrvx_error.h"

#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

// Deliberately an unnamed namespace, not `namespace lrvx`, to match
// how the binding headers that need this already declare their other
// file-local helpers (see aggregator_bindings.h, indicator_bindings.h).
// An unnamed namespace is unique per translation unit, not per header,
// so as long as every header that wants `checkSameSize` includes this
// one -- and `#pragma once` keeps it a single definition even though
// several headers include it -- plain unqualified `checkSameSize(...)`
// resolves correctly from any of them inside lrvx.cpp, the one .cpp
// file that pulls all of these headers together.
namespace
{

inline void checkSameSize(size_t a, size_t b, const char* msg)
{
  if (a != b)
  {
    throw lrvx::LrvxError("E_LEN_001", msg);
  }
}

// Fill a freshly allocated numpy buffer from a contiguous container.
//
// Every one of these copies used to be a bare `std::memcpy(out.mutable_data(),
// v.data(), v.size() * sizeof(T))`, and every one of them was undefined
// behaviour whenever the container was empty. A vector that has never
// allocated returns a null pointer from data(), and the standard requires
// memcpy's pointer arguments to be valid even when the length is zero
// (C23 7.26.1p3). It is not a theoretical reading: glibc declares memcpy
// __nonnull, so a build with the undefined-behaviour sanitizer turned on
// stops the process at the call instead of returning the empty array the
// release build returns.
//
// Empty is an ordinary result here, not a corner nobody reaches: an
// indicator over an empty series, a tape window that matched no rows, a
// graph field read after reset() cleared the bars. The length guard makes
// those paths give back an empty array in every build.
template <typename T>
inline void fillArray(T* dst, const T* src, size_t count)
{
  if (count == 0)
  {
    return;
  }
  std::memcpy(dst, src, count * sizeof(T));
}

// A string that names one of a fixed set of values, read strictly.
//
// The bindings used to read these with a comparison and a fallback:
// anything but "buy" was a sell, anything but "ioc", "fok", "gtd" or
// "post_only" a GTC order, anything but "lifo" or "average" FIFO. A typo
// then did something nobody asked for and said nothing. Every such string
// goes through one of these, which raise ValueError naming the value and
// what was expected.
template <typename E>
inline E parseChoice(const char* what, const std::string& value,
                     std::initializer_list<std::pair<const char*, E>> choices)
{
  std::string expected;
  for (const auto& [name, choice] : choices)
  {
    if (value == name)
    {
      return choice;
    }
    expected += expected.empty() ? "'" : ", '";
    expected += name;
    expected += "'";
  }
  throw pybind11::value_error(std::string("unknown ") + what + " '" + value +
                              "'; expected one of: " + expected);
}

inline lrvx::Side parseSide(const std::string& s)
{
  if (s == "buy")
  {
    return lrvx::Side::BUY;
  }
  if (s == "sell")
  {
    return lrvx::Side::SELL;
  }
  throw pybind11::value_error("unknown side '" + s + "'; expected 'buy' or 'sell'");
}

// For a call that can say when a GTD order expires.
inline lrvx::TimeInForce parseTimeInForceWithExpiry(const std::string& tif)
{
  return parseChoice<lrvx::TimeInForce>("tif", tif,
                                        {{"gtc", lrvx::TimeInForce::GTC},
                                         {"ioc", lrvx::TimeInForce::IOC},
                                         {"fok", lrvx::TimeInForce::FOK},
                                         {"gtd", lrvx::TimeInForce::GTD},
                                         {"post_only", lrvx::TimeInForce::POST_ONLY}});
}

// For a call that takes no expiry time, where GTD cannot be expressed.
inline lrvx::TimeInForce parseTimeInForce(const std::string& tif)
{
  if (tif == "gtc")
  {
    return lrvx::TimeInForce::GTC;
  }
  if (tif == "ioc")
  {
    return lrvx::TimeInForce::IOC;
  }
  if (tif == "fok")
  {
    return lrvx::TimeInForce::FOK;
  }
  if (tif == "post_only")
  {
    return lrvx::TimeInForce::POST_ONLY;
  }
  if (tif == "gtd")
  {
    throw pybind11::value_error(
        "tif 'gtd' needs an expiry time, which this method does not take; "
        "expected 'gtc', 'ioc', 'fok' or 'post_only'");
  }
  throw pybind11::value_error("unknown tif '" + tif +
                              "'; expected 'gtc', 'ioc', 'fok' or 'post_only'");
}

}  // namespace
