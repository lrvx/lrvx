/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// The neighbourhood of every point of a parameter grid: the mean of the
// neighbouring cells' values and how many neighbours there are. A sweep's
// best point is worth something when its neighbours are good too; an
// isolated peak is the signature of a fit to noise. The decision ("keep
// points whose neighbourhood mean is within x of their own value") stays with
// the caller; this is the primitive under it.
//
// values is row-major over shape (the last axis varies fastest), ndim axes.
// The neighbours of a cell are the cells whose index differs by at most
// radius on every axis, excluding the cell itself and anything outside the
// grid, so a corner of a 2-D grid has 3 neighbours at radius 1 and an interior
// cell has 8. A cell with no neighbour (radius 0, or a 1-cell grid) gets a
// mean of NaN and a count of 0. NaN values are skipped and not counted.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace lrvx::stats
{

inline void gridNeighborhood(const double* values, const std::size_t* shape, std::size_t ndim,
                             std::size_t radius, double* neighborMeanOut,
                             std::uint32_t* neighborCountOut)
{
  if (ndim == 0 || shape == nullptr || values == nullptr)
  {
    return;
  }
  std::size_t total = 1;
  for (std::size_t i = 0; i < ndim; ++i)
  {
    if (shape[i] == 0)
    {
      return;
    }
    total *= shape[i];
  }

  // Strides for row-major indexing.
  std::vector<std::size_t> stride(ndim, 1);
  for (std::size_t i = ndim - 1; i > 0; --i)
  {
    stride[i - 1] = stride[i] * shape[i];
  }

  const std::size_t span = 2 * radius + 1;
  std::vector<std::size_t> index(ndim, 0);
  std::vector<std::size_t> offset(ndim, 0);
  for (std::size_t cell = 0; cell < total; ++cell)
  {
    // The cell's multi-index.
    std::size_t rem = cell;
    for (std::size_t i = 0; i < ndim; ++i)
    {
      index[i] = rem / stride[i];
      rem %= stride[i];
    }

    double sum = 0.0;
    std::uint32_t count = 0;
    // Every offset vector in [-radius, radius]^ndim, as a mixed-radix
    // counter over span^ndim.
    for (auto& o : offset)
    {
      o = 0;
    }
    for (;;)
    {
      bool self = true;
      bool inside = true;
      std::size_t flat = 0;
      for (std::size_t i = 0; i < ndim && inside; ++i)
      {
        const long long delta = static_cast<long long>(offset[i]) - static_cast<long long>(radius);
        if (delta != 0)
        {
          self = false;
        }
        const long long pos = static_cast<long long>(index[i]) + delta;
        if (pos < 0 || pos >= static_cast<long long>(shape[i]))
        {
          inside = false;
        }
        else
        {
          flat += static_cast<std::size_t>(pos) * stride[i];
        }
      }
      if (inside && !self)
      {
        const double v = values[flat];
        if (!std::isnan(v))
        {
          sum += v;
          ++count;
        }
      }
      // Advance the counter.
      std::size_t i = 0;
      for (; i < ndim; ++i)
      {
        if (++offset[i] < span)
        {
          break;
        }
        offset[i] = 0;
      }
      if (i == ndim)
      {
        break;
      }
    }

    if (neighborMeanOut)
    {
      neighborMeanOut[cell] =
          count ? sum / static_cast<double>(count) : std::numeric_limits<double>::quiet_NaN();
    }
    if (neighborCountOut)
    {
      neighborCountOut[cell] = count;
    }
  }
}

}  // namespace lrvx::stats
