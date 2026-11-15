/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// lrvx.History and lrvx.crossed_above / lrvx.crossed_below, over the C API
// handles (lrvx_history_*), which own a lrvx::History.

#include <pybind11/pybind11.h>

#include "lrvx/capi/lrvx_capi.h"

#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace lrvx
{

class PyHistory
{
 public:
  explicit PyHistory(int64_t capacity)
  {
    if (capacity < 1 || capacity > static_cast<int64_t>(UINT32_MAX))
    {
      throw py::value_error("History capacity must be >= 1 (and fit 32 bits); got " +
                            std::to_string(capacity));
    }
    _h = lrvx_history_create(static_cast<uint32_t>(capacity));
    if (_h == nullptr)
    {
      throw py::value_error(lrvx_last_error_message());
    }
  }
  ~PyHistory() { lrvx_history_destroy(_h); }
  PyHistory(const PyHistory&) = delete;
  PyHistory& operator=(const PyHistory&) = delete;

  void push(double value) { lrvx_history_push(_h, value); }

  double get(int64_t index) const
  {
    const uint32_t n = lrvx_history_size(_h);
    if (index < 0 || index >= static_cast<int64_t>(n))
    {
      throw py::index_error("History index " + std::to_string(index) + " out of range (size " +
                            std::to_string(n) + ")");
    }
    return lrvx_history_get(_h, static_cast<uint32_t>(index));
  }

  uint32_t size() const { return lrvx_history_size(_h); }
  uint32_t capacity() const { return lrvx_history_capacity(_h); }
  bool full() const { return lrvx_history_full(_h) != 0; }

 private:
  LrvxHistoryHandle _h{nullptr};
};

// None reads as no value, so no cross.
inline bool pyCrossed(bool above, std::optional<double> aPrev, std::optional<double> a,
                      std::optional<double> bPrev, std::optional<double> b)
{
  if (!aPrev || !a || !bPrev || !b)
  {
    return false;
  }
  return (above ? lrvx_crossed_above(*aPrev, *a, *bPrev, *b)
                : lrvx_crossed_below(*aPrev, *a, *bPrev, *b)) != 0;
}

inline void bindHistory(py::module_& m)
{
  py::class_<PyHistory>(m, "History",
                        "The last `capacity` values of a series, newest first: h[0] is the "
                        "value pushed last, h[1] the one before. Once full, a push drops the "
                        "oldest. NaN is stored as given.")
      .def(py::init<int64_t>(), py::arg("capacity"),
           "Raises ValueError for a capacity below 1.")
      .def("push", &PyHistory::push, py::arg("value"))
      .def("get", &PyHistory::get, py::arg("index"),
           "0 is the newest value. Raises IndexError outside 0 .. len - 1.")
      .def("__getitem__", &PyHistory::get, py::arg("index"))
      .def("__len__", &PyHistory::size)
      .def_property_readonly("capacity", &PyHistory::capacity)
      .def_property_readonly("full", &PyHistory::full);

  m.def(
      "crossed_above",
      [](std::optional<double> a_prev, std::optional<double> a, std::optional<double> b_prev,
         std::optional<double> b)
      { return pyCrossed(true, a_prev, a, b_prev, b); },
      py::arg("a_prev"), py::arg("a"), py::arg("b_prev"), py::arg("b"),
      "True when a was at or below b on the previous step and is strictly above it now. "
      "For a level, pass it as both b_prev and b. None or NaN anywhere is no cross.");
  m.def(
      "crossed_below",
      [](std::optional<double> a_prev, std::optional<double> a, std::optional<double> b_prev,
         std::optional<double> b)
      { return pyCrossed(false, a_prev, a, b_prev, b); },
      py::arg("a_prev"), py::arg("a"), py::arg("b_prev"), py::arg("b"),
      "True when a was at or above b on the previous step and is strictly below it now. "
      "None or NaN anywhere is no cross.");
}

}  // namespace lrvx
