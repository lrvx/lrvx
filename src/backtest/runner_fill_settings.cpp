/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/backtest/runner_fill_settings.h"

#include "lrvx/backtest/latency_model.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace lrvx
{

const char* slippageModelName(SlippageModel model) noexcept
{
  switch (model)
  {
    case SlippageModel::NONE:
      return "none";
    case SlippageModel::FIXED_TICKS:
      return "fixed_ticks";
    case SlippageModel::FIXED_BPS:
      return "fixed_bps";
    case SlippageModel::VOLUME_IMPACT:
      return "volume_impact";
  }
  return "unknown";
}

namespace
{
constexpr SlippageModel kSlippageModels[] = {SlippageModel::NONE, SlippageModel::FIXED_TICKS,
                                             SlippageModel::FIXED_BPS,
                                             SlippageModel::VOLUME_IMPACT};
}  // namespace

std::optional<SlippageModel> slippageModelFromName(std::string_view name) noexcept
{
  for (SlippageModel m : kSlippageModels)
  {
    if (name == slippageModelName(m))
    {
      return m;
    }
  }
  return std::nullopt;
}

std::optional<SlippageModel> slippageModelFromCode(int32_t code) noexcept
{
  for (SlippageModel m : kSlippageModels)
  {
    if (code == static_cast<int32_t>(m))
    {
      return m;
    }
  }
  return std::nullopt;
}

std::string slippageModelNameList()
{
  std::string out;
  for (SlippageModel m : kSlippageModels)
  {
    if (!out.empty())
    {
      out += ", ";
    }
    out += "'";
    out += slippageModelName(m);
    out += "'";
  }
  return out;
}

const char* queueModelName(QueueModel model) noexcept
{
  switch (model)
  {
    case QueueModel::NONE:
      return "none";
    case QueueModel::TOB:
      return "tob";
    case QueueModel::FULL:
      return "full";
    case QueueModel::PRO_RATA:
      return "pro_rata";
    case QueueModel::PRO_RATA_WITH_FIFO:
      return "pro_rata_with_fifo";
    case QueueModel::TOP_PRO_LMM:
      return "top_pro_lmm";
    case QueueModel::PRO_RATA_WITH_PRIORITY:
      return "pro_rata_with_priority";
  }
  return "unknown";
}

namespace
{
constexpr QueueModel kQueueModels[] = {QueueModel::NONE,
                                       QueueModel::TOB,
                                       QueueModel::FULL,
                                       QueueModel::PRO_RATA,
                                       QueueModel::PRO_RATA_WITH_FIFO,
                                       QueueModel::TOP_PRO_LMM,
                                       QueueModel::PRO_RATA_WITH_PRIORITY};
}  // namespace

std::optional<QueueModel> queueModelFromName(std::string_view name) noexcept
{
  for (QueueModel m : kQueueModels)
  {
    if (name == queueModelName(m))
    {
      return m;
    }
  }
  return std::nullopt;
}

std::optional<QueueModel> queueModelFromCode(int32_t code) noexcept
{
  for (QueueModel m : kQueueModels)
  {
    if (code == static_cast<int32_t>(m))
    {
      return m;
    }
  }
  return std::nullopt;
}

std::string queueModelNameList()
{
  std::string out;
  for (QueueModel m : kQueueModels)
  {
    if (!out.empty())
    {
      out += ", ";
    }
    out += "'";
    out += queueModelName(m);
    out += "'";
  }
  return out;
}

bool RunnerFillSettings::customized() const noexcept
{
  return slippageModel != SlippageModel::NONE || slippageTicks != 0 || slippageTickSize != 0.0 ||
         slippageBps != 0.0 || slippageImpactCoeff != 0.0 || submitAckLatencyNs != 0 ||
         submitAckJitterNs != 0 || cancelAckLatencyNs != 0 || cancelAckJitterNs != 0 ||
         replaceAckLatencyNs != 0 || replaceAckJitterNs != 0 || latency != nullptr;
}

namespace
{

[[noreturn]] void invalid(const std::string& msg) { throw std::invalid_argument(msg); }

void nonNegative(const char* name, int64_t v)
{
  if (v < 0)
  {
    invalid(std::string(name) + " must be >= 0 nanoseconds, got " + std::to_string(v));
  }
}

bool positiveFinite(double v) { return std::isfinite(v) && v > 0.0; }

}  // namespace

void RunnerFillSettings::applyTo(BacktestConfig& cfg) const
{
  const SlippageModel m = slippageModel;
  if (m != SlippageModel::NONE && m != SlippageModel::FIXED_TICKS &&
      m != SlippageModel::FIXED_BPS && m != SlippageModel::VOLUME_IMPACT)
  {
    invalid("slippage_model must be one of none, fixed_ticks, fixed_bps, volume_impact; got " +
            std::to_string(static_cast<int>(m)));
  }
  const std::string modelName = slippageModelName(m);
  const auto unused = [&](bool reads, const char* name, double value)
  {
    if (!reads && value != 0.0)
    {
      invalid(std::string(name) + " has no effect with slippage_model='" + modelName +
              "'; leave it at 0 or pick the model that reads it");
    }
  };
  const bool ticks = m == SlippageModel::FIXED_TICKS;
  const bool bps = m == SlippageModel::FIXED_BPS;
  const bool impact = m == SlippageModel::VOLUME_IMPACT;
  unused(ticks, "slippage_ticks", static_cast<double>(slippageTicks));
  unused(ticks, "slippage_tick_size", slippageTickSize);
  unused(bps, "slippage_bps", slippageBps);
  unused(impact, "slippage_impact_coeff", slippageImpactCoeff);

  SlippageProfile profile{};
  profile.model = m;
  if (ticks)
  {
    if (slippageTicks <= 0)
    {
      invalid("slippage_ticks must be positive with slippage_model='fixed_ticks', got " +
              std::to_string(slippageTicks));
    }
    if (!positiveFinite(slippageTickSize))
    {
      invalid("slippage_tick_size must be a positive price with slippage_model='fixed_ticks', got " +
              std::to_string(slippageTickSize));
    }
    profile.ticks = slippageTicks;
    profile.tickSize = Price::fromDouble(slippageTickSize);
  }
  else if (bps)
  {
    if (!positiveFinite(slippageBps))
    {
      invalid(
          "slippage_bps must be a positive, finite number of basis points with "
          "slippage_model='fixed_bps', got " +
          std::to_string(slippageBps));
    }
    profile.bps = slippageBps;
  }
  else if (impact)
  {
    if (!positiveFinite(slippageImpactCoeff))
    {
      invalid(
          "slippage_impact_coeff must be positive and finite with "
          "slippage_model='volume_impact', got " +
          std::to_string(slippageImpactCoeff));
    }
    profile.impactCoeff = slippageImpactCoeff;
  }

  nonNegative("submit_ack_latency_ns", submitAckLatencyNs);
  nonNegative("submit_ack_jitter_ns", submitAckJitterNs);
  nonNegative("cancel_ack_latency_ns", cancelAckLatencyNs);
  nonNegative("cancel_ack_jitter_ns", cancelAckJitterNs);
  nonNegative("replace_ack_latency_ns", replaceAckLatencyNs);
  nonNegative("replace_ack_jitter_ns", replaceAckJitterNs);

  cfg.defaultSlippage = profile;
  cfg.submitAckLatencyNs = submitAckLatencyNs;
  cfg.submitAckJitterNs = submitAckJitterNs;
  cfg.cancelAckLatencyNs = cancelAckLatencyNs;
  cfg.cancelAckJitterNs = cancelAckJitterNs;
  cfg.cancelAckSeed = cancelAckSeed;
  cfg.replaceAckLatencyNs = replaceAckLatencyNs;
  cfg.replaceAckJitterNs = replaceAckJitterNs;
  cfg.latency = latency;
}

}  // namespace lrvx
