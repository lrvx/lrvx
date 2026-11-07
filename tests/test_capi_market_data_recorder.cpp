/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Tests for the MarketDataRecorder hook. Recorder receives every
// trade and book update fed into the runner, plus on_start / on_stop
// lifecycle balanced against runner start/stop.

#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>
#include <atomic>
#include <vector>

namespace
{

struct State
{
  std::atomic<int> trade_calls{0};
  std::atomic<int> book_calls{0};
  std::atomic<int> start_calls{0};
  std::atomic<int> stop_calls{0};

  // Order chars: 't' trade, 'b' book, '+' start, '-' stop.
  std::vector<char> order;

  // Last seen payloads — for value verification.
  uint32_t last_trade_symbol{0};
  int64_t last_trade_price_raw{0};
  int64_t last_trade_qty_raw{0};
  uint8_t last_trade_is_buy{0};

  uint32_t last_book_symbol{0};
  uint8_t last_book_is_snapshot{0};
  uint32_t last_n_bids{0};
  uint32_t last_n_asks{0};
  int64_t last_top_bid_price_raw{0};
  int64_t last_top_ask_price_raw{0};
};

void on_trade_cb(void* ud, const LrvxTradeData* trade)
{
  auto* s = static_cast<State*>(ud);
  s->trade_calls.fetch_add(1, std::memory_order_relaxed);
  s->order.push_back('t');
  s->last_trade_symbol = trade->symbol;
  s->last_trade_price_raw = trade->price_raw;
  s->last_trade_qty_raw = trade->quantity_raw;
  s->last_trade_is_buy = trade->is_buy;
}

void on_book_cb(void* ud,
                uint32_t symbol, uint8_t is_snapshot,
                const LrvxBookLevel* bids, uint32_t n_bids,
                const LrvxBookLevel* asks, uint32_t n_asks,
                int64_t /*ts*/)
{
  auto* s = static_cast<State*>(ud);
  s->book_calls.fetch_add(1, std::memory_order_relaxed);
  s->order.push_back('b');
  s->last_book_symbol = symbol;
  s->last_book_is_snapshot = is_snapshot;
  s->last_n_bids = n_bids;
  s->last_n_asks = n_asks;
  s->last_top_bid_price_raw = n_bids > 0 ? bids[0].price_raw : 0;
  s->last_top_ask_price_raw = n_asks > 0 ? asks[0].price_raw : 0;
}

void on_start_cb(void* ud)
{
  auto* s = static_cast<State*>(ud);
  s->start_calls.fetch_add(1, std::memory_order_relaxed);
  s->order.push_back('+');
}

void on_stop_cb(void* ud)
{
  auto* s = static_cast<State*>(ud);
  s->stop_calls.fetch_add(1, std::memory_order_relaxed);
  s->order.push_back('-');
}

struct RunnerCtx
{
  LrvxRegistryHandle registry{nullptr};
  LrvxStrategyHandle strategy{nullptr};
  LrvxRunnerHandle runner{nullptr};
  uint32_t symbol_id{0};

  ~RunnerCtx()
  {
    if (runner)
    {
      lrvx_runner_destroy(runner);
    }
    if (strategy)
    {
      lrvx_strategy_destroy(strategy);
    }
    if (registry)
    {
      lrvx_registry_destroy(registry);
    }
  }
};

void noop_signal(void*, const LrvxSignal*) {}

void make_runner(RunnerCtx& s)
{
  s.registry = lrvx_registry_create();
  ASSERT_NE(s.registry, nullptr);
  s.symbol_id = lrvx_registry_add_symbol(s.registry, "test", "BTC", 0.01);

  LrvxStrategyCallbacks cb{};
  uint32_t syms[] = {s.symbol_id};
  s.strategy = lrvx_strategy_create(/*id=*/1, syms, 1, s.registry, cb);
  ASSERT_NE(s.strategy, nullptr);

  s.runner = lrvx_runner_create(s.registry, noop_signal, nullptr);
  ASSERT_NE(s.runner, nullptr);
  lrvx_runner_add_strategy(s.runner, s.strategy);
}

LrvxMarketDataRecorderCallbacks make_callbacks(State& state)
{
  LrvxMarketDataRecorderCallbacks cb{};
  cb.on_trade = on_trade_cb;
  cb.on_book_update = on_book_cb;
  cb.on_start = on_start_cb;
  cb.on_stop = on_stop_cb;
  cb.user_data = &state;
  return cb;
}

}  // namespace

TEST(CapiMarketDataRecorder, FiresOnTradeAndBookUpdate)
{
  State state;
  RunnerCtx s;
  make_runner(s);

  auto* rec = lrvx_market_data_recorder_create(make_callbacks(state));
  ASSERT_NE(rec, nullptr);
  lrvx_runner_set_market_data_recorder(s.runner, rec);

  lrvx_runner_start(s.runner);

  lrvx_runner_on_trade(s.runner, s.symbol_id, /*price=*/100.5, /*qty=*/2.0,
                       /*is_buy=*/1, /*ts=*/1'000);

  double bid_p[] = {100.0, 99.0};
  double bid_q[] = {1.0, 2.0};
  double ask_p[] = {101.0, 102.0};
  double ask_q[] = {1.5, 2.5};
  lrvx_runner_on_book_snapshot(s.runner, s.symbol_id,
                               bid_p, bid_q, 2, ask_p, ask_q, 2,
                               /*ts=*/2'000);

  lrvx_runner_stop(s.runner);

  EXPECT_EQ(state.trade_calls.load(), 1);
  EXPECT_EQ(state.book_calls.load(), 1);
  EXPECT_EQ(state.start_calls.load(), 1);
  EXPECT_EQ(state.stop_calls.load(), 1);

  // Order: start → trade → book → stop.
  ASSERT_EQ(state.order.size(), 4u);
  EXPECT_EQ(state.order[0], '+');
  EXPECT_EQ(state.order[1], 't');
  EXPECT_EQ(state.order[2], 'b');
  EXPECT_EQ(state.order[3], '-');

  EXPECT_EQ(state.last_trade_symbol, s.symbol_id);
  EXPECT_EQ(state.last_trade_is_buy, 1);
  // 100.5 * 1e8 = 10050000000 (fixed-point Price scale).
  EXPECT_EQ(state.last_trade_price_raw, 10050000000LL);
  EXPECT_EQ(state.last_trade_qty_raw, 200000000LL);

  EXPECT_EQ(state.last_book_symbol, s.symbol_id);
  EXPECT_EQ(state.last_book_is_snapshot, 1);
  EXPECT_EQ(state.last_n_bids, 2u);
  EXPECT_EQ(state.last_n_asks, 2u);
  EXPECT_EQ(state.last_top_bid_price_raw, 10000000000LL);
  EXPECT_EQ(state.last_top_ask_price_raw, 10100000000LL);

  lrvx_market_data_recorder_destroy(rec);
}

// A top frame reaches the recorder as update type 2 (the byte space is
// 1 snapshot, 0 delta, 2 top) with one level per side; a side whose
// has-flag is 0 arrives with zero levels.
TEST(CapiMarketDataRecorder, BookTopArrivesAsUpdateTypeTwo)
{
  State state;
  RunnerCtx s;
  make_runner(s);

  auto* rec = lrvx_market_data_recorder_create(make_callbacks(state));
  ASSERT_NE(rec, nullptr);
  lrvx_runner_set_market_data_recorder(s.runner, rec);
  lrvx_runner_start(s.runner);

  lrvx_runner_on_book_top(s.runner, s.symbol_id, /*has_bid=*/1, 100.02, 4.0,
                          /*has_ask=*/1, 100.03, 3.0, /*ts=*/3'000);
  EXPECT_EQ(state.book_calls.load(), 1);
  EXPECT_EQ(state.last_book_is_snapshot, 2);
  EXPECT_EQ(state.last_n_bids, 1u);
  EXPECT_EQ(state.last_n_asks, 1u);
  EXPECT_EQ(state.last_top_bid_price_raw, 10002000000LL);
  EXPECT_EQ(state.last_top_ask_price_raw, 10003000000LL);

  lrvx_runner_on_book_top(s.runner, s.symbol_id, /*has_bid=*/1, 100.02, 4.0,
                          /*has_ask=*/0, 0.0, 0.0, /*ts=*/4'000);
  EXPECT_EQ(state.book_calls.load(), 2);
  EXPECT_EQ(state.last_n_bids, 1u);
  EXPECT_EQ(state.last_n_asks, 0u);

  lrvx_runner_stop(s.runner);
  lrvx_market_data_recorder_destroy(rec);
}

TEST(CapiMarketDataRecorder, NullDetachesRecorder)
{
  State state;
  RunnerCtx s;
  make_runner(s);

  auto* rec = lrvx_market_data_recorder_create(make_callbacks(state));
  lrvx_runner_set_market_data_recorder(s.runner, rec);
  lrvx_runner_start(s.runner);

  lrvx_runner_on_trade(s.runner, s.symbol_id, 100.0, 1.0, 1, 1'000);
  EXPECT_EQ(state.trade_calls.load(), 1);

  // Detach mid-run. on_stop fires for the outgoing recorder.
  lrvx_runner_set_market_data_recorder(s.runner, nullptr);
  EXPECT_EQ(state.stop_calls.load(), 1) << "detach must fire on_stop";

  lrvx_runner_on_trade(s.runner, s.symbol_id, 101.0, 1.0, 1, 1'000);
  EXPECT_EQ(state.trade_calls.load(), 1) << "no callbacks after detach";

  lrvx_runner_stop(s.runner);
  // No additional on_stop after detach.
  EXPECT_EQ(state.stop_calls.load(), 1);

  lrvx_market_data_recorder_destroy(rec);
}

TEST(CapiMarketDataRecorder, AttachMidRunFiresOnStart)
{
  State state;
  RunnerCtx s;
  make_runner(s);

  lrvx_runner_start(s.runner);
  EXPECT_EQ(state.start_calls.load(), 0) << "no recorder yet";

  auto* rec = lrvx_market_data_recorder_create(make_callbacks(state));
  lrvx_runner_set_market_data_recorder(s.runner, rec);
  EXPECT_EQ(state.start_calls.load(), 1)
      << "attaching while running must fire on_start";

  lrvx_runner_on_trade(s.runner, s.symbol_id, 100.0, 1.0, 1, 1'000);
  EXPECT_EQ(state.trade_calls.load(), 1);

  lrvx_runner_stop(s.runner);
  EXPECT_EQ(state.stop_calls.load(), 1);

  lrvx_market_data_recorder_destroy(rec);
}

TEST(CapiMarketDataRecorder, NullCallbackFunctionsAreNoOp)
{
  State state;
  RunnerCtx s;
  make_runner(s);

  // All-null callbacks: handle is non-null, but every function is null.
  LrvxMarketDataRecorderCallbacks cb{};
  cb.user_data = &state;
  auto* rec = lrvx_market_data_recorder_create(cb);
  lrvx_runner_set_market_data_recorder(s.runner, rec);

  lrvx_runner_start(s.runner);
  lrvx_runner_on_trade(s.runner, s.symbol_id, 100.0, 1.0, 1, 1'000);
  double bid_p[] = {100.0};
  double bid_q[] = {1.0};
  double ask_p[] = {101.0};
  double ask_q[] = {1.0};
  lrvx_runner_on_book_snapshot(s.runner, s.symbol_id, bid_p, bid_q, 1,
                               ask_p, ask_q, 1, 2'000);
  lrvx_runner_stop(s.runner);

  EXPECT_EQ(state.trade_calls.load(), 0);
  EXPECT_EQ(state.book_calls.load(), 0);
  EXPECT_EQ(state.start_calls.load(), 0);
  EXPECT_EQ(state.stop_calls.load(), 0);

  lrvx_market_data_recorder_destroy(rec);
}

TEST(CapiMarketDataRecorder, ReplaceRecorderRotatesLifecycle)
{
  State a;
  State b;
  RunnerCtx s;
  make_runner(s);

  auto* recA = lrvx_market_data_recorder_create(make_callbacks(a));
  auto* recB = lrvx_market_data_recorder_create(make_callbacks(b));

  lrvx_runner_set_market_data_recorder(s.runner, recA);
  lrvx_runner_start(s.runner);
  EXPECT_EQ(a.start_calls.load(), 1);

  lrvx_runner_on_trade(s.runner, s.symbol_id, 100.0, 1.0, 1, 1'000);
  EXPECT_EQ(a.trade_calls.load(), 1);
  EXPECT_EQ(b.trade_calls.load(), 0);

  // Hot-swap: A.on_stop fires, B.on_start fires.
  lrvx_runner_set_market_data_recorder(s.runner, recB);
  EXPECT_EQ(a.stop_calls.load(), 1);
  EXPECT_EQ(b.start_calls.load(), 1);

  lrvx_runner_on_trade(s.runner, s.symbol_id, 101.0, 1.0, 1, 1'000);
  EXPECT_EQ(a.trade_calls.load(), 1);
  EXPECT_EQ(b.trade_calls.load(), 1);

  lrvx_runner_stop(s.runner);
  // Only B sees on_stop on shutdown — A already saw it on swap.
  EXPECT_EQ(a.stop_calls.load(), 1);
  EXPECT_EQ(b.stop_calls.load(), 1);

  lrvx_market_data_recorder_destroy(recA);
  lrvx_market_data_recorder_destroy(recB);
}

// The live engine's publish path mirrors to the recorder on the publisher
// thread, so the frame is observable synchronously even though strategy
// consumers run on their own threads.
TEST(CapiMarketDataRecorder, LiveEnginePublishBookTopMirrorsTypeTwo)
{
  State state;
  auto* registry = lrvx_registry_create();
  uint32_t sym = lrvx_registry_add_symbol(registry, "test", "BTC", 0.01);
  auto* engine = lrvx_live_engine_create(registry);
  auto* rec = lrvx_market_data_recorder_create(make_callbacks(state));
  ASSERT_NE(rec, nullptr);
  lrvx_live_engine_set_market_data_recorder(engine, rec);
  lrvx_live_engine_start(engine);

  lrvx_live_engine_publish_book_top(engine, sym, /*has_bid=*/1, 100.02, 4.0,
                                    /*has_ask=*/1, 100.03, 3.0, /*ts=*/3'000);
  EXPECT_EQ(state.book_calls.load(), 1);
  EXPECT_EQ(state.last_book_is_snapshot, 2);
  EXPECT_EQ(state.last_n_bids, 1u);
  EXPECT_EQ(state.last_n_asks, 1u);
  EXPECT_EQ(state.last_top_bid_price_raw, 10002000000LL);
  EXPECT_EQ(state.last_top_ask_price_raw, 10003000000LL);

  lrvx_live_engine_stop(engine);
  lrvx_live_engine_destroy(engine);
  lrvx_market_data_recorder_destroy(rec);
  lrvx_registry_destroy(registry);
}
