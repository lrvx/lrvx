/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// History: the last `capacity` values of a series, indexed newest first, so
// h[0] is the current bar's value and h[1] the previous one. A bar strategy
// keeps one per series it compares across bars instead of a hand-written
// ring. crossedAbove / crossedBelow answer whether one series crossed
// another (or a level) between the previous step and this one.

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace lrvx
{

class History
{
 public:
  // Throws std::invalid_argument for a capacity of 0.
  explicit History(std::size_t capacity) : _buf(requireCapacity(capacity)) {}

  // Stores the value as given, NaN included; once full, the oldest drops out.
  void push(double value) noexcept
  {
    _head = _head + 1 == _buf.size() ? 0 : _head + 1;
    _buf[_head] = value;
    if (_size < _buf.size())
    {
      ++_size;
    }
  }

  // 0 is the newest value. Unchecked: index must be below size().
  double operator[](std::size_t index) const noexcept
  {
    return _buf[index <= _head ? _head - index : _head + _buf.size() - index];
  }

  // Throws std::out_of_range when index >= size().
  double at(std::size_t index) const
  {
    if (index >= _size)
    {
      throw std::out_of_range("History index " + std::to_string(index) + " out of range (size " +
                              std::to_string(_size) + ")");
    }
    return (*this)[index];
  }

  std::size_t size() const noexcept { return _size; }
  std::size_t capacity() const noexcept { return _buf.size(); }
  bool full() const noexcept { return _size == _buf.size(); }

 private:
  static std::size_t requireCapacity(std::size_t capacity)
  {
    if (capacity == 0)
    {
      throw std::invalid_argument("History capacity must be >= 1");
    }
    return capacity;
  }

  std::vector<double> _buf;
  // Slot of the newest value; the first push lands in slot 0.
  std::size_t _head{static_cast<std::size_t>(-1)};
  std::size_t _size{0};
};

// a was at or below b on the previous step and is strictly above it now.
// For a level, pass it as both bPrev and b. A NaN anywhere is no cross.
inline bool crossedAbove(double aPrev, double a, double bPrev, double b) noexcept
{
  return aPrev <= bPrev && a > b;
}

// a was at or above b on the previous step and is strictly below it now.
inline bool crossedBelow(double aPrev, double a, double bPrev, double b) noexcept
{
  return aPrev >= bPrev && a < b;
}

}  // namespace lrvx
