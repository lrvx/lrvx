// python/types_bindings.h
//
// Shared Pythonic mirrors of small C-ABI structs. Used by both
// strategy_bindings.h and hook_bindings.h; broken out to avoid the
// circular include that would otherwise arise when hook trampolines
// receive PySignal / PyTradeData arguments.

#pragma once

#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/capi/order_type_names.hpp"
#include "lrvx/strategy/signal.h"

#include <cstdint>
#include <string>

namespace
{

struct PyTradeData
{
  uint32_t symbol{0};
  std::string symbol_name;
  double price{0.0};
  double quantity{0.0};
  bool is_buy{false};
  std::string side;
  int64_t timestamp_ns{0};
  // For hook-side use (no symbol_name lookup).
  int64_t exchange_ts_ns{0};
};

struct PySignal
{
  uint64_t order_id{0};
  uint32_t symbol{0};
  std::string side;
  std::string order_type;
  double price{0.0};
  double quantity{0.0};
  double trigger_price{0.0};
  double trailing_offset{0.0};
  int32_t trailing_bps{0};
  double new_price{0.0};
  double new_quantity{0.0};
  double range_lower{0.0};
  double range_upper{0.0};
  double liquidity{0.0};
};

inline PySignal pySignalFromC(const LrvxSignal* s)
{
  PySignal ps{};
  ps.order_id = s->order_id;
  ps.symbol = s->symbol;
  ps.side = s->side == 0 ? "buy" : "sell";
  ps.order_type = lrvx::capi::signalTypeName(s->order_type);
  ps.price = s->price;
  ps.quantity = s->quantity;
  ps.trigger_price = s->trigger_price;
  ps.trailing_offset = s->trailing_offset;
  ps.trailing_bps = s->trailing_bps;
  ps.new_price = s->new_price;
  ps.new_quantity = s->new_quantity;
  ps.range_lower = s->range_lower;
  ps.range_upper = s->range_upper;
  ps.liquidity = s->liquidity;
  return ps;
}

// The same Signal, read from the C++ struct rather than the C ABI copy.
inline PySignal pySignalFromCxx(const lrvx::Signal& s)
{
  using lrvx::SignalType;
  uint8_t code = LRVX_SIGNAL_TYPE_MARKET;
  switch (s.type)
  {
    case SignalType::Market:
      code = LRVX_SIGNAL_TYPE_MARKET;
      break;
    case SignalType::Limit:
      code = LRVX_SIGNAL_TYPE_LIMIT;
      break;
    case SignalType::StopMarket:
      code = LRVX_SIGNAL_TYPE_STOP_MARKET;
      break;
    case SignalType::StopLimit:
      code = LRVX_SIGNAL_TYPE_STOP_LIMIT;
      break;
    case SignalType::TakeProfitMarket:
      code = LRVX_SIGNAL_TYPE_TAKE_PROFIT_MARKET;
      break;
    case SignalType::TakeProfitLimit:
      code = LRVX_SIGNAL_TYPE_TAKE_PROFIT_LIMIT;
      break;
    case SignalType::TrailingStop:
      code = LRVX_SIGNAL_TYPE_TRAILING_STOP;
      break;
    case SignalType::Cancel:
      code = LRVX_SIGNAL_TYPE_CANCEL;
      break;
    case SignalType::CancelAll:
      code = LRVX_SIGNAL_TYPE_CANCEL_ALL;
      break;
    case SignalType::Modify:
      code = LRVX_SIGNAL_TYPE_MODIFY;
      break;
    case SignalType::OCO:
      code = LRVX_SIGNAL_TYPE_OCO;
      break;
    case SignalType::ProvideLiquidity:
      code = LRVX_SIGNAL_TYPE_PROVIDE_LIQUIDITY;
      break;
    case SignalType::WithdrawLiquidity:
      code = LRVX_SIGNAL_TYPE_WITHDRAW_LIQUIDITY;
      break;
  }
  LrvxSignal c{};
  c.order_id = s.orderId;
  c.symbol = s.symbol;
  c.side = s.side == lrvx::Side::BUY ? 0 : 1;
  c.order_type = code;
  c.price = s.price.toDouble();
  c.quantity = s.quantity.toDouble();
  c.trigger_price = s.triggerPrice.toDouble();
  c.trailing_offset = s.trailingOffset.toDouble();
  c.trailing_bps = s.trailingCallbackRate;
  c.new_price = s.newPrice.toDouble();
  c.new_quantity = s.newQuantity.toDouble();
  c.range_lower = s.priceLower.toDouble();
  c.range_upper = s.priceUpper.toDouble();
  c.liquidity = s.liquidity.toDouble();
  return pySignalFromC(&c);
}

}  // namespace
