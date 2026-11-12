/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/backtest/backtest_config.h"

#include <cstdint>
#include <memory>

namespace lrvx
{

class LatencyModel;

/// The executor settings the binding runners (Python BacktestRunner, the C
/// API's lrvx_backtest_runner_create_with_fill_settings, Node and Codon on
/// top of it) take as arguments. Every field is a BacktestConfig field and
/// means what it means there; applyTo() writes them into a config after
/// checking them, so each binding refuses the same inputs with the same
/// message. The messages name the fields by the snake_case keyword the
/// bindings document.
///
/// The defaults are BacktestConfig's defaults: a runner built from a
/// default RunnerFillSettings fills exactly like one built without it.
struct RunnerFillSettings
{
  SlippageModel slippageModel{SlippageModel::NONE};
  int32_t slippageTicks{0};
  double slippageTickSize{0.0};
  double slippageBps{0.0};
  double slippageImpactCoeff{0.0};
  int64_t submitAckLatencyNs{0};
  int64_t submitAckJitterNs{0};
  int64_t cancelAckLatencyNs{0};
  int64_t cancelAckJitterNs{0};
  uint64_t cancelAckSeed{42};
  int64_t replaceAckLatencyNs{0};
  int64_t replaceAckJitterNs{0};
  std::shared_ptr<LatencyModel> latency{};

  /// True when any setting reaches the executor differently from the
  /// defaults. The seed alone is not counted: without a jittered latency it
  /// draws nothing.
  bool customized() const noexcept;

  /// Checks every field and writes them into `cfg` (defaultSlippage, the
  /// three ack latencies and jitters, cancelAckSeed, latency). Throws
  /// std::invalid_argument when
  ///   - a latency or jitter is negative;
  ///   - the model's own parameter is missing or not positive and finite
  ///     (fixed_ticks: slippage_ticks and slippage_tick_size, fixed_bps:
  ///     slippage_bps, volume_impact: slippage_impact_coeff);
  ///   - a parameter the chosen model does not read is set, which would
  ///     otherwise be dropped without a word.
  /// `cfg` is left untouched when it throws.
  void applyTo(BacktestConfig& cfg) const;
};

/// Snake_case name of a slippage model as the bindings spell it: none,
/// fixed_ticks, fixed_bps, volume_impact.
const char* slippageModelName(SlippageModel model) noexcept;

}  // namespace lrvx
