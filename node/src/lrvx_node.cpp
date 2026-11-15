// lrvx_node -- Node.js native addon for lrvx.

#include <napi.h>

#include "abi.h"
#include "account.h"
#include "aggregators.h"
#include "amm_curve.h"
#include "backtest.h"
#include "bar_dispatch.h"
#include "books.h"
#include "data_ops.h"
#include "delta_book.h"
#include "dex_amount.h"
#include "engine.h"
#include "execution_algos.h"
#include "fee_schedule.h"
#include "feed_clock.h"
#include "funding_schedule.h"
#include "graph.h"
#include "heatmap.h"
#include "hooks.h"
#include "indicators.h"
#include "latency.h"
#include "liquidation_engine.h"
#include "live_queue_position.h"
#include "order_group.h"
#include "pool_tape.h"
#include "portfolio_risk.h"
#include "positions.h"
#include "profiles.h"
#include "rate_limit.h"
#include "run_trace.h"
#include "stats.h"
#include "strategy.h"
#include "strategy_helpers.h"
#include "tape_aggregators.h"
#include "tape_diff.h"
#include "targets.h"
#include "venue_availability.h"
#include "venue_stack.h"

Napi::Object Init(Napi::Env env, Napi::Object exports)
{
  // Before anything else: a binding that loaded against the wrong
  // library must not register a surface that reads wrong numbers.
  if (!node_lrvx::registerAbi(env, exports))
  {
    return exports;
  }

  node_lrvx::registerEngine(env, exports);
  node_lrvx::registerIndicators(env, exports);
  node_lrvx::registerBacktest(env, exports);
  node_lrvx::registerBooks(env, exports);
  node_lrvx::registerPositions(env, exports);
  node_lrvx::registerProfiles(env, exports);
  node_lrvx::registerStats(env, exports);
  node_lrvx::registerAggregators(env, exports);
  node_lrvx::registerTapeAggregators(env, exports);
  node_lrvx::registerDataOps(env, exports);
  node_lrvx::registerStrategy(env, exports);
  node_lrvx::registerTargets(env, exports);
  node_lrvx::registerGraph(env, exports);
  node_lrvx::registerHeatmap(env, exports);
  node_lrvx::registerDeltaBook(env, exports);
  node_lrvx::registerExecutionAlgos(env, exports);
  node_lrvx::registerLatency(env, exports);
  node_lrvx::registerPortfolioRisk(env, exports);
  node_lrvx::registerOrderGroup(env, exports);
  node_lrvx::registerRateLimitPolicy(env, exports);
  node_lrvx::registerVenueAvailability(env, exports);
  node_lrvx::registerVenueStack(env, exports);
  node_lrvx::registerFundingSchedule(env, exports);
  node_lrvx::registerAccount(env, exports);
  node_lrvx::registerFeeSchedule(env, exports);
  node_lrvx::registerLiquidationEngine(env, exports);
  node_lrvx::registerLiveQueuePositionEstimator(env, exports);
  node_lrvx::registerBarDispatchRecorder(env, exports);
  node_lrvx::registerFeedClock(env, exports);
  node_lrvx::registerStrategyHelpers(env, exports);
  node_lrvx::registerTapeDiff(env, exports);
  node_lrvx::registerDexAmount(env, exports);
  node_lrvx::registerAmmCurve(env, exports);
  node_lrvx::registerPoolTape(env, exports);
  node_lrvx::registerRunTrace(env, exports);
  lrvx_node::registerHooks(env, exports);
  return exports;
}

NODE_API_MODULE(lrvx_node, Init)
