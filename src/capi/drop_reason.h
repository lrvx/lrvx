/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/strategy/signal.h"

#include <string_view>

// The reason a pre-trade gate dropped a signal, left in the C API's last
// error of the calling thread:
//   "emit: <type> signal for order <id> on symbol <n> was dropped by the
//    <gate>[ (<detail>)]; <outcome> [<token>]"
// Defined in lrvx_capi.cpp. A binding that drives a lrvx::BacktestRunner
// itself, without the C API's runner handle, installs
// lrvxCapiNoteBacktestDrop as that runner's drop handler so its drops read
// the same.
void lrvxCapiNoteDroppedSignal(const lrvx::Signal& signal, std::string_view gate,
                               std::string_view token, std::string_view detail,
                               std::string_view outcome) noexcept;

void lrvxCapiNoteBacktestDrop(const lrvx::BacktestRunner::SignalDrop& drop) noexcept;
