/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// pybind11 wrappers for the C-ABI extension hooks.
//
// Pattern (used for every hook):
//
//   PyXxx           — Pythonic base class users subclass; virtual methods.
//   PyXxxTrampoline — pybind11 trampoline for PYBIND11_OVERRIDE dispatch.
//   xxxBridge*      — C-ABI callbacks that acquire the GIL, invoke virtual.
//   PyXxxOwner      — RAII over LrvxXxxHandle; non-copyable.
//
// PyRunner / PyBacktestRunner own the Owner, holding both the C-ABI handle
// and a shared_ptr to the Python object so it survives the engine's
// non-owning use of `user_data`.

#pragma once

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "callback_guard.h"
#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/capi/order_type_names.hpp"
#include "lrvx/common.h"
#include "lrvx/execution/abstract_execution_listener.h"
#include "lrvx/execution/abstract_executor.h"
#include "lrvx/execution/exchange_capabilities.h"
#include "lrvx/execution/order.h"
#include "lrvx/killswitch/abstract_killswitch.h"
#include "lrvx/metrics/abstract_pnl_tracker.h"
#include "lrvx/risk/abstract_risk_manager.h"
#include "lrvx/validation/abstract_order_validator.h"
#include "types_bindings.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace lrvx
{

// ── Shared Py-side mirrors of C-ABI structs ─────────────────────────────

struct PyOrder
{
  uint64_t id{0};
  uint64_t client_order_id{0};
  uint32_t symbol{0};
  uint16_t strategy_id{0};
  uint16_t order_tag{0};
  std::string side;        // "buy" / "sell"
  std::string order_type;  // "market", "limit", ...
  std::string time_in_force;
  bool reduce_only{false};
  bool post_only{false};
  bool close_position{false};
  double price{0.0};
  double quantity{0.0};
  double filled_quantity{0.0};
  double trigger_price{0.0};
  double trailing_offset{0.0};
  int64_t created_at_ns{0};
  int64_t exchange_ts_ns{0};
};

inline PyOrder pyOrderFromC(const LrvxOrder* o)
{
  static constexpr const char* kTif[] = {"gtc", "ioc", "fok", "gtd", "post_only"};
  PyOrder po{};
  po.id = o->id;
  po.client_order_id = o->client_order_id;
  po.symbol = o->symbol;
  po.strategy_id = o->strategy_id;
  po.order_tag = o->order_tag;
  po.side = o->side == 0 ? "buy" : "sell";
  po.order_type = lrvx::capi::orderTypeNameLower(o->type);
  po.time_in_force = o->time_in_force < 5 ? kTif[o->time_in_force] : "unknown";
  po.reduce_only = (o->flags & 0x01) != 0;
  po.close_position = (o->flags & 0x02) != 0;
  po.post_only = (o->flags & 0x04) != 0;
  po.price = static_cast<double>(o->price_raw) / 1e8;
  po.quantity = static_cast<double>(o->quantity_raw) / 1e8;
  po.filled_quantity = static_cast<double>(o->filled_quantity_raw) / 1e8;
  po.trigger_price = static_cast<double>(o->trigger_price_raw) / 1e8;
  po.trailing_offset = static_cast<double>(o->trailing_offset_raw) / 1e8;
  po.created_at_ns = o->created_at_ns;
  po.exchange_ts_ns = o->exchange_ts_ns;
  return po;
}

inline LrvxOrder cOrderFromPy(const PyOrder& po)
{
  LrvxOrder o{};
  o.id = po.id;
  o.client_order_id = po.client_order_id;
  o.symbol = po.symbol;
  o.strategy_id = po.strategy_id;
  o.order_tag = po.order_tag;
  o.side = po.side == "buy" ? 0u : 1u;
  lrvx::capi::orderTypeCodeFromName(po.order_type, &o.type);
  if (po.time_in_force == "gtc")
  {
    o.time_in_force = 0;
  }
  else if (po.time_in_force == "ioc")
  {
    o.time_in_force = 1;
  }
  else if (po.time_in_force == "fok")
  {
    o.time_in_force = 2;
  }
  else if (po.time_in_force == "gtd")
  {
    o.time_in_force = 3;
  }
  else if (po.time_in_force == "post_only")
  {
    o.time_in_force = 4;
  }
  uint8_t flags = 0;
  if (po.reduce_only)
  {
    flags |= 0x01;
  }
  if (po.close_position)
  {
    flags |= 0x02;
  }
  if (po.post_only)
  {
    flags |= 0x04;
  }
  o.flags = flags;
  o.price_raw = static_cast<int64_t>(std::llround((po.price) * 1e8));
  o.quantity_raw = static_cast<int64_t>(std::llround((po.quantity) * 1e8));
  o.filled_quantity_raw = static_cast<int64_t>(std::llround((po.filled_quantity) * 1e8));
  o.trigger_price_raw = static_cast<int64_t>(std::llround((po.trigger_price) * 1e8));
  o.trailing_offset_raw = static_cast<int64_t>(std::llround((po.trailing_offset) * 1e8));
  o.created_at_ns = po.created_at_ns;
  o.exchange_ts_ns = po.exchange_ts_ns;
  return o;
}

struct PyExchangeCapabilities
{
  bool stop_market{false};
  bool stop_limit{false};
  bool take_profit_market{false};
  bool take_profit_limit{false};
  bool trailing_stop{false};
  bool iceberg{false};
  bool oco{false};
  bool gtc{true};
  bool ioc{true};
  bool fok{false};
  bool gtd{false};
  bool post_only{false};
  bool reduce_only{false};
  bool close_position{false};
  bool batch_submit{false};
};

inline LrvxExchangeCapabilities cCapsFromPy(const PyExchangeCapabilities& p)
{
  LrvxExchangeCapabilities c{};
  c.supports_stop_market = p.stop_market ? 1 : 0;
  c.supports_stop_limit = p.stop_limit ? 1 : 0;
  c.supports_take_profit_market = p.take_profit_market ? 1 : 0;
  c.supports_take_profit_limit = p.take_profit_limit ? 1 : 0;
  c.supports_trailing_stop = p.trailing_stop ? 1 : 0;
  c.supports_iceberg = p.iceberg ? 1 : 0;
  c.supports_oco = p.oco ? 1 : 0;
  c.supports_gtc = p.gtc ? 1 : 0;
  c.supports_ioc = p.ioc ? 1 : 0;
  c.supports_fok = p.fok ? 1 : 0;
  c.supports_gtd = p.gtd ? 1 : 0;
  c.supports_post_only = p.post_only ? 1 : 0;
  c.supports_reduce_only = p.reduce_only ? 1 : 0;
  c.supports_close_position = p.close_position ? 1 : 0;
  c.supports_batch_submit = p.batch_submit ? 1 : 0;
  return c;
}

inline PyExchangeCapabilities pyCapsFromC(const LrvxExchangeCapabilities& c)
{
  PyExchangeCapabilities p{};
  p.stop_market = c.supports_stop_market != 0;
  p.stop_limit = c.supports_stop_limit != 0;
  p.take_profit_market = c.supports_take_profit_market != 0;
  p.take_profit_limit = c.supports_take_profit_limit != 0;
  p.trailing_stop = c.supports_trailing_stop != 0;
  p.iceberg = c.supports_iceberg != 0;
  p.oco = c.supports_oco != 0;
  p.gtc = c.supports_gtc != 0;
  p.ioc = c.supports_ioc != 0;
  p.fok = c.supports_fok != 0;
  p.gtd = c.supports_gtd != 0;
  p.post_only = c.supports_post_only != 0;
  p.reduce_only = c.supports_reduce_only != 0;
  p.close_position = c.supports_close_position != 0;
  p.batch_submit = c.supports_batch_submit != 0;
  return p;
}

// ── Helper: invoke Python callback under GIL, contain exceptions ────────
//
// Hooks fire from C-ABI consumer threads (live engine) or the publisher
// thread (sync runner). Either way we must hold the GIL while invoking
// virtual dispatch. An exception from Python is contained here and its
// text is reported through the engine log — propagating it across the C
// ABI boundary is undefined behaviour. callback_guard.h states the whole
// policy. Returns true when the callback completed; `source` names it in
// the report and defaults to the enclosing bridge function.

template <typename Fn>
inline bool invokeUnderGil(Fn fn, const char* source = nullptr,
                           const std::source_location loc = std::source_location::current())
{
  py::gil_scoped_acquire gil;
  return guardCallback(fn, source, loc);
}

// ── PnLTracker hook ──────────────────────────────────────────────────────

class PyPnLTracker
{
 public:
  virtual ~PyPnLTracker() = default;
  virtual void on_signal(const PySignal& /*signal*/) {}
};

class PyPnLTrackerTrampoline : public PyPnLTracker
{
 public:
  using PyPnLTracker::PyPnLTracker;
  void on_signal(const PySignal& sig) override
  {
    PYBIND11_OVERRIDE(void, PyPnLTracker, on_signal, sig);
  }
};

inline void pnlOnSignalBridge(void* ud, const LrvxSignal* sig)
{
  auto* py_obj = static_cast<PyPnLTracker*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_signal(pySignalFromC(sig)); });
}

class PyPnLTrackerOwner
{
 public:
  explicit PyPnLTrackerOwner(std::shared_ptr<PyPnLTracker> delegate)
      : _delegate(std::move(delegate))
  {
    LrvxPnLTrackerCallbacks cb{};
    cb.on_signal = pnlOnSignalBridge;
    cb.user_data = _delegate.get();
    _handle = lrvx_pnl_tracker_create(cb);
  }
  ~PyPnLTrackerOwner()
  {
    if (_handle)
    {
      lrvx_pnl_tracker_destroy(_handle);
    }
  }
  PyPnLTrackerOwner(const PyPnLTrackerOwner&) = delete;
  PyPnLTrackerOwner& operator=(const PyPnLTrackerOwner&) = delete;
  LrvxPnLTrackerHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyPnLTracker> _delegate;
  LrvxPnLTrackerHandle _handle{nullptr};
};

// ── StorageSink hook ────────────────────────────────────────────────────

class PyStorageSink
{
 public:
  virtual ~PyStorageSink() = default;
  virtual void store(const PySignal& /*signal*/) {}
};

class PyStorageSinkTrampoline : public PyStorageSink
{
 public:
  using PyStorageSink::PyStorageSink;
  void store(const PySignal& sig) override
  {
    PYBIND11_OVERRIDE(void, PyStorageSink, store, sig);
  }
};

inline void storageStoreBridge(void* ud, const LrvxSignal* sig)
{
  auto* py_obj = static_cast<PyStorageSink*>(ud);
  invokeUnderGil([&]
                 { py_obj->store(pySignalFromC(sig)); });
}

class PyStorageSinkOwner
{
 public:
  explicit PyStorageSinkOwner(std::shared_ptr<PyStorageSink> delegate)
      : _delegate(std::move(delegate))
  {
    LrvxStorageSinkCallbacks cb{};
    cb.store = storageStoreBridge;
    cb.user_data = _delegate.get();
    _handle = lrvx_storage_sink_create(cb);
  }
  ~PyStorageSinkOwner()
  {
    if (_handle)
    {
      lrvx_storage_sink_destroy(_handle);
    }
  }
  PyStorageSinkOwner(const PyStorageSinkOwner&) = delete;
  PyStorageSinkOwner& operator=(const PyStorageSinkOwner&) = delete;
  LrvxStorageSinkHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyStorageSink> _delegate;
  LrvxStorageSinkHandle _handle{nullptr};
};

// ── RiskManager hook ────────────────────────────────────────────────────

class PyRiskManager
{
 public:
  virtual ~PyRiskManager() = default;
  // Return True to allow the signal, False to drop it.
  virtual bool allow(const PySignal& /*signal*/) { return true; }
};

class PyRiskManagerTrampoline : public PyRiskManager
{
 public:
  using PyRiskManager::PyRiskManager;
  bool allow(const PySignal& sig) override
  {
    PYBIND11_OVERRIDE(bool, PyRiskManager, allow, sig);
  }
};

inline uint8_t riskAllowBridge(void* ud, const LrvxSignal* sig)
{
  auto* py_obj = static_cast<PyRiskManager*>(ud);
  // Deny unless the callable says otherwise: a gate that raised, or
  // answered with something that is not a bool, did not let the signal
  // through. See callback_guard.h.
  uint8_t result = kGateDenied;
  invokeUnderGil([&]
                 { result = py_obj->allow(pySignalFromC(sig)) ? kGateAllowed : kGateDenied; },
                 "RiskManager.allow");
  return result;
}

class PyRiskManagerOwner
{
 public:
  explicit PyRiskManagerOwner(std::shared_ptr<PyRiskManager> delegate)
      : _delegate(std::move(delegate))
  {
    LrvxRiskManagerCallbacks cb{};
    cb.allow = riskAllowBridge;
    cb.user_data = _delegate.get();
    _handle = lrvx_risk_manager_create(cb);
  }
  ~PyRiskManagerOwner()
  {
    if (_handle)
    {
      lrvx_risk_manager_destroy(_handle);
    }
  }
  PyRiskManagerOwner(const PyRiskManagerOwner&) = delete;
  PyRiskManagerOwner& operator=(const PyRiskManagerOwner&) = delete;
  LrvxRiskManagerHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyRiskManager> _delegate;
  LrvxRiskManagerHandle _handle{nullptr};
};

// ── KillSwitch hook ─────────────────────────────────────────────────────

class PyKillSwitch
{
 public:
  virtual ~PyKillSwitch() = default;
  // Return True to let the signal through, False to halt trading.
  virtual bool check(const PySignal& /*signal*/) { return true; }
};

class PyKillSwitchTrampoline : public PyKillSwitch
{
 public:
  using PyKillSwitch::PyKillSwitch;
  bool check(const PySignal& sig) override
  {
    PYBIND11_OVERRIDE(bool, PyKillSwitch, check, sig);
  }
};

inline uint8_t killCheckBridge(void* ud, const LrvxSignal* sig)
{
  auto* py_obj = static_cast<PyKillSwitch*>(ud);
  // Deny unless the callable says otherwise: a gate that raised, or
  // answered with something that is not a bool, did not let the signal
  // through. See callback_guard.h.
  uint8_t result = kGateDenied;
  invokeUnderGil([&]
                 { result = py_obj->check(pySignalFromC(sig)) ? kGateAllowed : kGateDenied; },
                 "KillSwitch.check");
  return result;
}

class PyKillSwitchOwner
{
 public:
  explicit PyKillSwitchOwner(std::shared_ptr<PyKillSwitch> delegate)
      : _delegate(std::move(delegate))
  {
    LrvxKillSwitchCallbacks cb{};
    cb.check = killCheckBridge;
    cb.user_data = _delegate.get();
    _handle = lrvx_kill_switch_create(cb);
  }
  ~PyKillSwitchOwner()
  {
    if (_handle)
    {
      lrvx_kill_switch_destroy(_handle);
    }
  }
  PyKillSwitchOwner(const PyKillSwitchOwner&) = delete;
  PyKillSwitchOwner& operator=(const PyKillSwitchOwner&) = delete;
  LrvxKillSwitchHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyKillSwitch> _delegate;
  LrvxKillSwitchHandle _handle{nullptr};
};

// ── OrderValidator hook ─────────────────────────────────────────────────

class PyOrderValidator
{
 public:
  virtual ~PyOrderValidator() = default;
  virtual bool validate(const PySignal& /*signal*/) { return true; }
};

class PyOrderValidatorTrampoline : public PyOrderValidator
{
 public:
  using PyOrderValidator::PyOrderValidator;
  bool validate(const PySignal& sig) override
  {
    PYBIND11_OVERRIDE(bool, PyOrderValidator, validate, sig);
  }
};

inline uint8_t orderValidateBridge(void* ud, const LrvxSignal* sig)
{
  auto* py_obj = static_cast<PyOrderValidator*>(ud);
  // Deny unless the callable says otherwise: a gate that raised, or
  // answered with something that is not a bool, did not let the signal
  // through. See callback_guard.h.
  uint8_t result = kGateDenied;
  invokeUnderGil([&]
                 { result = py_obj->validate(pySignalFromC(sig)) ? kGateAllowed : kGateDenied; },
                 "OrderValidator.validate");
  return result;
}

class PyOrderValidatorOwner
{
 public:
  explicit PyOrderValidatorOwner(std::shared_ptr<PyOrderValidator> delegate)
      : _delegate(std::move(delegate))
  {
    LrvxOrderValidatorCallbacks cb{};
    cb.validate = orderValidateBridge;
    cb.user_data = _delegate.get();
    _handle = lrvx_order_validator_create(cb);
  }
  ~PyOrderValidatorOwner()
  {
    if (_handle)
    {
      lrvx_order_validator_destroy(_handle);
    }
  }
  PyOrderValidatorOwner(const PyOrderValidatorOwner&) = delete;
  PyOrderValidatorOwner& operator=(const PyOrderValidatorOwner&) = delete;
  LrvxOrderValidatorHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyOrderValidator> _delegate;
  LrvxOrderValidatorHandle _handle{nullptr};
};

// ── MarketDataRecorder hook ─────────────────────────────────────────────

class PyMarketDataRecorderHook
{
 public:
  virtual ~PyMarketDataRecorderHook() = default;
  virtual void on_trade(const PyTradeData& /*trade*/) {}
  virtual void on_book_update(uint32_t /*symbol*/, bool /*is_snapshot*/,
                              const std::vector<std::pair<double, double>>& /*bids*/,
                              const std::vector<std::pair<double, double>>& /*asks*/,
                              int64_t /*ts_ns*/) {}
  virtual void on_start() {}
  virtual void on_stop() {}
};

class PyMarketDataRecorderHookTrampoline : public PyMarketDataRecorderHook
{
 public:
  using PyMarketDataRecorderHook::PyMarketDataRecorderHook;
  void on_trade(const PyTradeData& t) override
  {
    PYBIND11_OVERRIDE(void, PyMarketDataRecorderHook, on_trade, t);
  }
  void on_book_update(uint32_t symbol, bool is_snapshot,
                      const std::vector<std::pair<double, double>>& bids,
                      const std::vector<std::pair<double, double>>& asks,
                      int64_t ts) override
  {
    PYBIND11_OVERRIDE(void, PyMarketDataRecorderHook, on_book_update,
                      symbol, is_snapshot, bids, asks, ts);
  }
  void on_start() override
  {
    PYBIND11_OVERRIDE(void, PyMarketDataRecorderHook, on_start);
  }
  void on_stop() override
  {
    PYBIND11_OVERRIDE(void, PyMarketDataRecorderHook, on_stop);
  }
};

inline void recOnTradeBridge(void* ud, const LrvxTradeData* t)
{
  auto* py_obj = static_cast<PyMarketDataRecorderHook*>(ud);
  invokeUnderGil([&]
                 {
    PyTradeData pt{};
    pt.symbol = t->symbol;
    pt.price = static_cast<double>(t->price_raw) / 1e8;
    pt.quantity = static_cast<double>(t->quantity_raw) / 1e8;
    pt.is_buy = t->is_buy != 0;
    pt.exchange_ts_ns = t->exchange_ts_ns;
    py_obj->on_trade(pt); });
}

inline void recOnBookBridge(void* ud, uint32_t symbol, uint8_t is_snap,
                            const LrvxBookLevel* bids, uint32_t n_bids,
                            const LrvxBookLevel* asks, uint32_t n_asks,
                            int64_t ts)
{
  auto* py_obj = static_cast<PyMarketDataRecorderHook*>(ud);
  invokeUnderGil([&]
                 {
    std::vector<std::pair<double, double>> b, a;
    b.reserve(n_bids);
    a.reserve(n_asks);
    for (uint32_t i = 0; i < n_bids; ++i){
      b.emplace_back(static_cast<double>(bids[i].price_raw) / 1e8,
                     static_cast<double>(bids[i].quantity_raw) / 1e8);
}
    for (uint32_t i = 0; i < n_asks; ++i){
      a.emplace_back(static_cast<double>(asks[i].price_raw) / 1e8,
                     static_cast<double>(asks[i].quantity_raw) / 1e8);
}
    // The byte is 1 snapshot, 0 delta, 2 top-of-book; the Python hook's
    // signature is a bool, so a top degrades to "snapshot" here — it is the
    // venue's complete word on the touch, which is what a snapshot promises.
    py_obj->on_book_update(symbol, is_snap != 0, b, a, ts); });
}

inline void recOnStartBridge(void* ud)
{
  auto* py_obj = static_cast<PyMarketDataRecorderHook*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_start(); });
}
inline void recOnStopBridge(void* ud)
{
  auto* py_obj = static_cast<PyMarketDataRecorderHook*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_stop(); });
}

class PyMarketDataRecorderHookOwner
{
 public:
  explicit PyMarketDataRecorderHookOwner(std::shared_ptr<PyMarketDataRecorderHook> delegate)
      : _delegate(std::move(delegate))
  {
    LrvxMarketDataRecorderCallbacks cb{};
    cb.on_trade = recOnTradeBridge;
    cb.on_book_update = recOnBookBridge;
    cb.on_start = recOnStartBridge;
    cb.on_stop = recOnStopBridge;
    cb.user_data = _delegate.get();
    _handle = lrvx_market_data_recorder_create(cb);
  }
  ~PyMarketDataRecorderHookOwner()
  {
    if (_handle)
    {
      lrvx_market_data_recorder_destroy(_handle);
    }
  }
  PyMarketDataRecorderHookOwner(const PyMarketDataRecorderHookOwner&) = delete;
  PyMarketDataRecorderHookOwner& operator=(const PyMarketDataRecorderHookOwner&) = delete;
  LrvxMarketDataRecorderHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyMarketDataRecorderHook> _delegate;
  LrvxMarketDataRecorderHandle _handle{nullptr};
};

// ── ReplaySource hook ───────────────────────────────────────────────────

struct PyReplayEvent
{
  std::string type;  // "trade", "book_snapshot", "book_delta"
  int64_t timestamp_ns{0};
  // Trade
  uint32_t trade_symbol{0};
  bool trade_is_buy{false};
  double trade_price{0.0};
  double trade_quantity{0.0};
  // Book
  uint32_t book_symbol{0};
  std::vector<std::pair<double, double>> bids;
  std::vector<std::pair<double, double>> asks;
};

class PyReplaySource
{
 public:
  virtual ~PyReplaySource() = default;
  virtual void on_start() {}
  virtual void on_stop() {}
  virtual bool seek_to(int64_t /*ts_ns*/) { return false; }
  // Return None to signal end of stream.
  virtual std::optional<PyReplayEvent> next() { return std::nullopt; }
};

class PyReplaySourceTrampoline : public PyReplaySource
{
 public:
  using PyReplaySource::PyReplaySource;
  void on_start() override { PYBIND11_OVERRIDE(void, PyReplaySource, on_start); }
  void on_stop() override { PYBIND11_OVERRIDE(void, PyReplaySource, on_stop); }
  bool seek_to(int64_t ts) override
  {
    PYBIND11_OVERRIDE(bool, PyReplaySource, seek_to, ts);
  }
  std::optional<PyReplayEvent> next() override
  {
    PYBIND11_OVERRIDE(std::optional<PyReplayEvent>, PyReplaySource, next);
  }
};

// Storage for book level arrays — must outlive the `next()` callback so
// pointers remain valid until the engine consumes them.
struct PyReplaySourceBookBuffers
{
  std::vector<LrvxBookLevel> bids;
  std::vector<LrvxBookLevel> asks;
};

inline uint8_t replayNextBridge(void* ud, LrvxReplayEvent* out)
{
  // Bind the buffers to the Python object so they survive across calls.
  // This works because the engine consumes each event before calling
  // next() again (single-threaded forEach contract).
  struct Bound : PyReplaySourceBookBuffers
  {
  };
  static thread_local Bound buf;

  auto* py_obj = static_cast<PyReplaySource*>(ud);
  uint8_t produced = 0;
  invokeUnderGil([&]
                 {
    auto opt = py_obj->next();
    if (!opt.has_value())
    {
      return;
    }
    const PyReplayEvent& ev = *opt;
    out->timestamp_ns = ev.timestamp_ns;
    if (ev.type == "trade")
    {
      out->type = 1;
      out->trade_symbol = ev.trade_symbol;
      out->trade_is_buy = ev.trade_is_buy ? 1 : 0;
      out->trade_price_raw = static_cast<int64_t>(std::llround((ev.trade_price ) * 1e8));
      out->trade_quantity_raw = static_cast<int64_t>(std::llround((ev.trade_quantity ) * 1e8));
      out->n_bids = 0;
      out->n_asks = 0;
      out->bids = nullptr;
      out->asks = nullptr;
    }
    else
    {
      out->type = (ev.type == "book_snapshot") ? 2 : 3;
      out->book_symbol = ev.book_symbol;
      buf.bids.clear();
      buf.asks.clear();
      buf.bids.reserve(ev.bids.size());
      buf.asks.reserve(ev.asks.size());
      for (const auto& [p, q] : ev.bids){
        buf.bids.push_back({static_cast<int64_t>(std::llround((p ) * 1e8)), static_cast<int64_t>(std::llround((q ) * 1e8))});
}
      for (const auto& [p, q] : ev.asks){
        buf.asks.push_back({static_cast<int64_t>(std::llround((p ) * 1e8)), static_cast<int64_t>(std::llround((q ) * 1e8))});
}
      out->n_bids = static_cast<uint32_t>(buf.bids.size());
      out->n_asks = static_cast<uint32_t>(buf.asks.size());
      out->bids = buf.bids.empty() ? nullptr : buf.bids.data();
      out->asks = buf.asks.empty() ? nullptr : buf.asks.data();
      out->trade_symbol = 0;
      out->trade_is_buy = 0;
      out->trade_price_raw = 0;
      out->trade_quantity_raw = 0;
    }
    produced = 1; });
  return produced;
}

inline void replayOnStartBridge(void* ud)
{
  auto* py_obj = static_cast<PyReplaySource*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_start(); });
}
inline void replayOnStopBridge(void* ud)
{
  auto* py_obj = static_cast<PyReplaySource*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_stop(); });
}
inline uint8_t replaySeekBridge(void* ud, int64_t ts)
{
  auto* py_obj = static_cast<PyReplaySource*>(ud);
  uint8_t result = 0;
  invokeUnderGil([&]
                 { result = py_obj->seek_to(ts) ? 1u : 0u; });
  return result;
}

class PyReplaySourceOwner
{
 public:
  explicit PyReplaySourceOwner(std::shared_ptr<PyReplaySource> delegate)
      : _delegate(std::move(delegate))
  {
    LrvxReplaySourceCallbacks cb{};
    cb.on_start = replayOnStartBridge;
    cb.on_stop = replayOnStopBridge;
    cb.seek_to = replaySeekBridge;
    cb.next = replayNextBridge;
    cb.user_data = _delegate.get();
    _handle = lrvx_replay_source_create(cb);
  }
  ~PyReplaySourceOwner()
  {
    if (_handle)
    {
      lrvx_replay_source_destroy(_handle);
    }
  }
  PyReplaySourceOwner(const PyReplaySourceOwner&) = delete;
  PyReplaySourceOwner& operator=(const PyReplaySourceOwner&) = delete;
  LrvxReplaySourceHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyReplaySource> _delegate;
  LrvxReplaySourceHandle _handle{nullptr};
};

// ── ExecutionListener hook ──────────────────────────────────────────────

class PyExecutionListener
{
 public:
  virtual ~PyExecutionListener() = default;
  virtual void on_submitted(const PyOrder&) {}
  virtual void on_accepted(const PyOrder&) {}
  virtual void on_partially_filled(const PyOrder&, double /*fill_qty*/) {}
  virtual void on_filled(const PyOrder&) {}
  virtual void on_pending_cancel(const PyOrder&) {}
  virtual void on_canceled(const PyOrder&) {}
  virtual void on_expired(const PyOrder&) {}
  virtual void on_rejected(const PyOrder&, const std::string& /*reason*/) {}
  virtual void on_replaced(const PyOrder&, const PyOrder&) {}
  virtual void on_pending_trigger(const PyOrder&) {}
  virtual void on_triggered(const PyOrder&) {}
  virtual void on_trailing_stop_updated(const PyOrder&, double /*new_trigger*/) {}
};

class PyExecutionListenerTrampoline : public PyExecutionListener
{
 public:
  using PyExecutionListener::PyExecutionListener;

  void on_submitted(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_submitted, order);
  }
  void on_accepted(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_accepted, order);
  }
  void on_partially_filled(const PyOrder& order, double fill_qty) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_partially_filled, order, fill_qty);
  }
  void on_filled(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_filled, order);
  }
  void on_pending_cancel(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_pending_cancel, order);
  }
  void on_canceled(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_canceled, order);
  }
  void on_expired(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_expired, order);
  }
  void on_rejected(const PyOrder& order, const std::string& reason) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_rejected, order, reason);
  }
  void on_replaced(const PyOrder& old_order, const PyOrder& new_order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_replaced, old_order, new_order);
  }
  void on_pending_trigger(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_pending_trigger, order);
  }
  void on_triggered(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_triggered, order);
  }
  void on_trailing_stop_updated(const PyOrder& order, double new_trigger) override
  {
    PYBIND11_OVERRIDE(void, PyExecutionListener, on_trailing_stop_updated, order, new_trigger);
  }
};

#define _BRIDGE_ORDER_FN(NAME, METHOD)                        \
  inline void NAME(void* ud, const LrvxOrder* o)              \
  {                                                           \
    auto* py_obj = static_cast<PyExecutionListener*>(ud);     \
    invokeUnderGil([&] { py_obj->METHOD(pyOrderFromC(o)); }); \
  }

_BRIDGE_ORDER_FN(execOnSubmittedBridge, on_submitted)
_BRIDGE_ORDER_FN(execOnAcceptedBridge, on_accepted)
_BRIDGE_ORDER_FN(execOnFilledBridge, on_filled)
_BRIDGE_ORDER_FN(execOnPendingCancelBridge, on_pending_cancel)
_BRIDGE_ORDER_FN(execOnCanceledBridge, on_canceled)
_BRIDGE_ORDER_FN(execOnExpiredBridge, on_expired)
_BRIDGE_ORDER_FN(execOnPendingTriggerBridge, on_pending_trigger)
_BRIDGE_ORDER_FN(execOnTriggeredBridge, on_triggered)
#undef _BRIDGE_ORDER_FN

inline void execOnPartialFillBridge(void* ud, const LrvxOrder* o, int64_t fill_qty_raw)
{
  auto* py_obj = static_cast<PyExecutionListener*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_partially_filled(pyOrderFromC(o), static_cast<double>(fill_qty_raw) / 1e8); });
}

inline void execOnRejectedBridge(void* ud, const LrvxOrder* o, const char* reason)
{
  auto* py_obj = static_cast<PyExecutionListener*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_rejected(pyOrderFromC(o), reason ? std::string(reason) : std::string{}); });
}

inline void execOnReplacedBridge(void* ud, const LrvxOrder* old_o, const LrvxOrder* new_o)
{
  auto* py_obj = static_cast<PyExecutionListener*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_replaced(pyOrderFromC(old_o), pyOrderFromC(new_o)); });
}

inline void execOnTrailingUpdateBridge(void* ud, const LrvxOrder* o, int64_t new_trigger_raw)
{
  auto* py_obj = static_cast<PyExecutionListener*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_trailing_stop_updated(pyOrderFromC(o),
                                                    static_cast<double>(new_trigger_raw) / 1e8); });
}

class PyExecutionListenerOwner
{
 public:
  explicit PyExecutionListenerOwner(std::shared_ptr<PyExecutionListener> delegate)
      : _delegate(std::move(delegate))
  {
    LrvxExecutionListenerCallbacks cb{};
    cb.on_submitted = execOnSubmittedBridge;
    cb.on_accepted = execOnAcceptedBridge;
    cb.on_partially_filled = execOnPartialFillBridge;
    cb.on_filled = execOnFilledBridge;
    cb.on_pending_cancel = execOnPendingCancelBridge;
    cb.on_canceled = execOnCanceledBridge;
    cb.on_expired = execOnExpiredBridge;
    cb.on_rejected = execOnRejectedBridge;
    cb.on_replaced = execOnReplacedBridge;
    cb.on_pending_trigger = execOnPendingTriggerBridge;
    cb.on_triggered = execOnTriggeredBridge;
    cb.on_trailing_stop_updated = execOnTrailingUpdateBridge;
    cb.user_data = _delegate.get();
    _handle = lrvx_execution_listener_create(cb);
  }
  ~PyExecutionListenerOwner()
  {
    if (_handle)
    {
      lrvx_execution_listener_destroy(_handle);
    }
  }
  PyExecutionListenerOwner(const PyExecutionListenerOwner&) = delete;
  PyExecutionListenerOwner& operator=(const PyExecutionListenerOwner&) = delete;
  LrvxExecutionListenerHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyExecutionListener> _delegate;
  LrvxExecutionListenerHandle _handle{nullptr};
};

// ── Executor hook ───────────────────────────────────────────────────────

class PyExecutor
{
 public:
  virtual ~PyExecutor() = default;
  virtual void submit(const PyOrder&) {}
  virtual void cancel(uint64_t /*order_id*/) {}
  virtual void cancel_all(uint32_t /*symbol*/) {}
  virtual void replace(uint64_t /*old_id*/, const PyOrder&) {}
  virtual void submit_oco(const PyOrder&, const PyOrder&) {}
  virtual PyExchangeCapabilities capabilities() { return {}; }
  virtual void on_start() {}
  virtual void on_stop() {}
};

class PyExecutorTrampoline : public PyExecutor
{
 public:
  using PyExecutor::PyExecutor;
  void submit(const PyOrder& order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutor, submit, order);
  }
  void cancel(uint64_t order_id) override
  {
    PYBIND11_OVERRIDE(void, PyExecutor, cancel, order_id);
  }
  void cancel_all(uint32_t symbol) override
  {
    PYBIND11_OVERRIDE(void, PyExecutor, cancel_all, symbol);
  }
  void replace(uint64_t old_id, const PyOrder& new_order) override
  {
    PYBIND11_OVERRIDE(void, PyExecutor, replace, old_id, new_order);
  }
  void submit_oco(const PyOrder& order1, const PyOrder& order2) override
  {
    PYBIND11_OVERRIDE(void, PyExecutor, submit_oco, order1, order2);
  }
  PyExchangeCapabilities capabilities() override
  {
    PYBIND11_OVERRIDE(PyExchangeCapabilities, PyExecutor, capabilities);
  }
  void on_start() override { PYBIND11_OVERRIDE(void, PyExecutor, on_start); }
  void on_stop() override { PYBIND11_OVERRIDE(void, PyExecutor, on_stop); }
};

inline void execSubmitBridge(void* ud, const LrvxOrder* o)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->submit(pyOrderFromC(o)); });
}
inline void execCancelBridge(void* ud, uint64_t id)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->cancel(id); });
}
inline void execCancelAllBridge(void* ud, uint32_t s)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->cancel_all(s); });
}
inline void execReplaceBridge(void* ud, uint64_t old_id, const LrvxOrder* o)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->replace(old_id, pyOrderFromC(o)); });
}
inline void execSubmitOcoBridge(void* ud, const LrvxOrder* a, const LrvxOrder* b)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->submit_oco(pyOrderFromC(a), pyOrderFromC(b)); });
}
inline void execCapsBridge(void* ud, LrvxExchangeCapabilities* out)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { *out = cCapsFromPy(py_obj->capabilities()); });
}
inline void execorOnStartBridge(void* ud)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_start(); });
}
inline void execorOnStopBridge(void* ud)
{
  auto* py_obj = static_cast<PyExecutor*>(ud);
  invokeUnderGil([&]
                 { py_obj->on_stop(); });
}

class PyExecutorOwner
{
 public:
  explicit PyExecutorOwner(std::shared_ptr<PyExecutor> delegate)
      : _delegate(std::move(delegate))
  {
    LrvxExecutorCallbacks cb{};
    cb.submit = execSubmitBridge;
    cb.cancel = execCancelBridge;
    cb.cancel_all = execCancelAllBridge;
    cb.replace = execReplaceBridge;
    cb.submit_oco = execSubmitOcoBridge;
    cb.capabilities = execCapsBridge;
    cb.on_start = execorOnStartBridge;
    cb.on_stop = execorOnStopBridge;
    cb.user_data = _delegate.get();
    _handle = lrvx_executor_create(cb);
  }
  ~PyExecutorOwner()
  {
    if (_handle)
    {
      lrvx_executor_destroy(_handle);
    }
  }
  PyExecutorOwner(const PyExecutorOwner&) = delete;
  PyExecutorOwner& operator=(const PyExecutorOwner&) = delete;
  LrvxExecutorHandle handle() const noexcept { return _handle; }

 private:
  std::shared_ptr<PyExecutor> _delegate;
  LrvxExecutorHandle _handle{nullptr};
};

// ── Logger callback ─────────────────────────────────────────────────────

// Deliberately leaked: the slot holds a Python object, and a static
// destructor runs after the interpreter is gone, where releasing it
// segfaults. logCallbackTeardown() empties it while Python is still up.
inline py::object& globalLogCallback()
{
  static py::object* cb = new py::object();
  return *cb;
}

inline void loggerBridge(void* /*ud*/, int32_t level, const char* msg)
{
  invokeUnderGil([&]
                 {
    auto& cb = globalLogCallback();
    if (!cb.is_none())
    {
      cb(level, msg ? std::string(msg) : std::string{});
    } },
                 "the log callback");
}

// Detach the sink before the interpreter tears down. A Python callable
// left installed at that point is released — and can still be called by
// a C++ thread — after finalisation, which crashes the process on exit.
// Registered with atexit at module init.
inline void logCallbackTeardown()
{
  lrvx_set_log_callback(nullptr, nullptr);
  globalLogCallback() = py::none();
}

inline void setPythonLogCallback(py::object cb)
{
  if (cb.is_none())
  {
    lrvx_set_log_callback(nullptr, nullptr);
    globalLogCallback() = py::none();
  }
  else
  {
    globalLogCallback() = std::move(cb);
    lrvx_set_log_callback(loggerBridge, nullptr);
  }
}

// ── Direct-C++ adapters for BacktestRunner ──────────────────────────────
//
// PyBacktestRunner currently wraps lrvx::BacktestRunner directly (no C-ABI
// indirection). Its setExecutor / addExecutionListener methods take
// lrvx::IOrderExecutor* and lrvx::IOrderExecutionListener* — so we bridge
// the Pythonic delegate to those C++ interfaces here, mirroring the
// CapiExecutor / CapiExecutionListener adapters that live in
// src/capi/lrvx_capi.cpp.

namespace cxx_adapters
{

inline lrvx::Order pyOrderToLrvx(const PyOrder& po)
{
  lrvx::Order o{};
  o.id = po.id;
  o.clientOrderId = po.client_order_id;
  o.symbol = po.symbol;
  o.strategyId = po.strategy_id;
  o.orderTag = po.order_tag;
  o.side = (po.side == "buy") ? lrvx::Side::BUY : lrvx::Side::SELL;
  if (po.order_type == "limit")
  {
    o.type = lrvx::OrderType::LIMIT;
  }
  else if (po.order_type == "market")
  {
    o.type = lrvx::OrderType::MARKET;
  }
  else if (po.order_type == "stop_market")
  {
    o.type = lrvx::OrderType::STOP_MARKET;
  }
  else if (po.order_type == "stop_limit")
  {
    o.type = lrvx::OrderType::STOP_LIMIT;
  }
  else if (po.order_type == "tp_market")
  {
    o.type = lrvx::OrderType::TAKE_PROFIT_MARKET;
  }
  else if (po.order_type == "tp_limit")
  {
    o.type = lrvx::OrderType::TAKE_PROFIT_LIMIT;
  }
  else if (po.order_type == "trailing_stop")
  {
    o.type = lrvx::OrderType::TRAILING_STOP;
  }
  else if (po.order_type == "iceberg")
  {
    o.type = lrvx::OrderType::ICEBERG;
  }
  if (po.time_in_force == "ioc")
  {
    o.timeInForce = lrvx::TimeInForce::IOC;
  }
  else if (po.time_in_force == "fok")
  {
    o.timeInForce = lrvx::TimeInForce::FOK;
  }
  else if (po.time_in_force == "gtd")
  {
    o.timeInForce = lrvx::TimeInForce::GTD;
  }
  else if (po.time_in_force == "post_only")
  {
    o.timeInForce = lrvx::TimeInForce::POST_ONLY;
  }
  else
  {
    o.timeInForce = lrvx::TimeInForce::GTC;
  }
  o.flags.reduceOnly = po.reduce_only ? 1 : 0;
  o.flags.closePosition = po.close_position ? 1 : 0;
  o.flags.postOnly = po.post_only ? 1 : 0;
  o.price = lrvx::Price::fromDouble(po.price);
  o.quantity = lrvx::Quantity::fromDouble(po.quantity);
  o.filledQuantity = lrvx::Quantity::fromDouble(po.filled_quantity);
  o.triggerPrice = lrvx::Price::fromDouble(po.trigger_price);
  o.trailingOffset = lrvx::Price::fromDouble(po.trailing_offset);
  return o;
}

inline PyOrder lrvxOrderToPy(const lrvx::Order& o)
{
  PyOrder po{};
  po.id = o.id;
  po.client_order_id = o.clientOrderId;
  po.symbol = o.symbol;
  po.strategy_id = o.strategyId;
  po.order_tag = o.orderTag;
  po.side = (o.side == lrvx::Side::BUY) ? "buy" : "sell";
  switch (o.type)
  {
    case lrvx::OrderType::LIMIT:
      po.order_type = "limit";
      break;
    case lrvx::OrderType::MARKET:
      po.order_type = "market";
      break;
    case lrvx::OrderType::STOP_MARKET:
      po.order_type = "stop_market";
      break;
    case lrvx::OrderType::STOP_LIMIT:
      po.order_type = "stop_limit";
      break;
    case lrvx::OrderType::TAKE_PROFIT_MARKET:
      po.order_type = "tp_market";
      break;
    case lrvx::OrderType::TAKE_PROFIT_LIMIT:
      po.order_type = "tp_limit";
      break;
    case lrvx::OrderType::TRAILING_STOP:
      po.order_type = "trailing_stop";
      break;
    case lrvx::OrderType::ICEBERG:
      po.order_type = "iceberg";
      break;
  }
  switch (o.timeInForce)
  {
    case lrvx::TimeInForce::GTC:
      po.time_in_force = "gtc";
      break;
    case lrvx::TimeInForce::IOC:
      po.time_in_force = "ioc";
      break;
    case lrvx::TimeInForce::FOK:
      po.time_in_force = "fok";
      break;
    case lrvx::TimeInForce::GTD:
      po.time_in_force = "gtd";
      break;
    case lrvx::TimeInForce::POST_ONLY:
      po.time_in_force = "post_only";
      break;
  }
  po.reduce_only = o.flags.reduceOnly != 0;
  po.close_position = o.flags.closePosition != 0;
  po.post_only = o.flags.postOnly != 0;
  po.price = o.price.toDouble();
  po.quantity = o.quantity.toDouble();
  po.filled_quantity = o.filledQuantity.toDouble();
  po.trigger_price = o.triggerPrice.toDouble();
  po.trailing_offset = o.trailingOffset.toDouble();
  return po;
}

class PyExecutorCxxAdapter : public lrvx::IOrderExecutor
{
 public:
  explicit PyExecutorCxxAdapter(std::shared_ptr<PyExecutor> delegate)
      : _delegate(std::move(delegate))
  {
  }
  void start() override
  {
    invokeUnderGil([&]
                   { _delegate->on_start(); });
  }
  void stop() override
  {
    invokeUnderGil([&]
                   { _delegate->on_stop(); });
  }
  void submitOrder(const lrvx::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->submit(lrvxOrderToPy(o)); });
  }
  void cancelOrder(lrvx::OrderId id) override
  {
    invokeUnderGil([&]
                   { _delegate->cancel(static_cast<uint64_t>(id)); });
  }
  void cancelAllOrders(lrvx::SymbolId sym) override
  {
    invokeUnderGil([&]
                   { _delegate->cancel_all(static_cast<uint32_t>(sym)); });
  }
  void replaceOrder(lrvx::OrderId old_id, const lrvx::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->replace(static_cast<uint64_t>(old_id), lrvxOrderToPy(o)); });
  }
  void submitOCO(const lrvx::OCOParams& p) override
  {
    invokeUnderGil([&]
                   { _delegate->submit_oco(lrvxOrderToPy(p.order1), lrvxOrderToPy(p.order2)); });
  }
  lrvx::ExchangeCapabilities capabilities() const override
  {
    lrvx::ExchangeCapabilities out{};
    invokeUnderGil([&]
                   {
      auto pc = _delegate->capabilities();
      out.supportsStopMarket = pc.stop_market;
      out.supportsStopLimit = pc.stop_limit;
      out.supportsTakeProfitMarket = pc.take_profit_market;
      out.supportsTakeProfitLimit = pc.take_profit_limit;
      out.supportsTrailingStop = pc.trailing_stop;
      out.supportsIceberg = pc.iceberg;
      out.supportsOCO = pc.oco;
      out.supportsGTC = pc.gtc;
      out.supportsIOC = pc.ioc;
      out.supportsFOK = pc.fok;
      out.supportsGTD = pc.gtd;
      out.supportsPostOnly = pc.post_only;
      out.supportsReduceOnly = pc.reduce_only;
      out.supportsClosePosition = pc.close_position;
      out.supportsBatchSubmit = pc.batch_submit; });
    return out;
  }

 private:
  std::shared_ptr<PyExecutor> _delegate;
};

class PyExecutionListenerCxxAdapter : public lrvx::IOrderExecutionListener
{
 public:
  PyExecutionListenerCxxAdapter(lrvx::SubscriberId id,
                                std::shared_ptr<PyExecutionListener> delegate)
      : lrvx::IOrderExecutionListener(id), _delegate(std::move(delegate))
  {
  }
  void onOrderSubmitted(const lrvx::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_submitted(lrvxOrderToPy(o)); });
  }
  void onOrderAccepted(const lrvx::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_accepted(lrvxOrderToPy(o)); });
  }
  void onOrderFilled(const lrvx::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_filled(lrvxOrderToPy(o)); });
  }
  void onOrderPendingCancel(const lrvx::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_pending_cancel(lrvxOrderToPy(o)); });
  }
  void onOrderCanceled(const lrvx::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_canceled(lrvxOrderToPy(o)); });
  }
  void onOrderExpired(const lrvx::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_expired(lrvxOrderToPy(o)); });
  }
  void onOrderPendingTrigger(const lrvx::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_pending_trigger(lrvxOrderToPy(o)); });
  }
  void onOrderTriggered(const lrvx::Order& o) override
  {
    invokeUnderGil([&]
                   { _delegate->on_triggered(lrvxOrderToPy(o)); });
  }

  void onOrderPartiallyFilled(const lrvx::Order& o, lrvx::Quantity q) override
  {
    invokeUnderGil([&]
                   { _delegate->on_partially_filled(lrvxOrderToPy(o), q.toDouble()); });
  }
  void onOrderRejected(const lrvx::Order& o, const std::string& reason) override
  {
    invokeUnderGil([&]
                   { _delegate->on_rejected(lrvxOrderToPy(o), reason); });
  }
  void onOrderReplaced(const lrvx::Order& a, const lrvx::Order& b) override
  {
    invokeUnderGil([&]
                   { _delegate->on_replaced(lrvxOrderToPy(a), lrvxOrderToPy(b)); });
  }
  void onTrailingStopUpdated(const lrvx::Order& o, lrvx::Price t) override
  {
    invokeUnderGil([&]
                   { _delegate->on_trailing_stop_updated(lrvxOrderToPy(o), t.toDouble()); });
  }

 private:
  std::shared_ptr<PyExecutionListener> _delegate;
};

// PyExecutionListenerCxxAdapter doesn't override on_submitted etc. — it
// uses the C++ method names. The Python class uses on_submitted,
// on_accepted, ..., which the adapter forwards to via the _BR1 macros.
// (Mapping is one-to-one: onOrderSubmitted → on_submitted, etc., done in
// each macro instance.)

// ── Pre-trade gate adapters ────────────────────────────────
//
// Bridge the existing PyRiskManager / PyKillSwitch / PyOrderValidator /
// PyPnLTracker (which work on PySignal) into the engine-side
// IRiskManager / IKillSwitch / IOrderValidator / IPnLTracker
// interfaces (which work on Order). Conversion is one-shot per call —
// orders ship the same fields as signals plus side/type as Order
// enums, so we re-emit a PySignal shape so existing user code keeps
// working unchanged.

inline PySignal pySignalFromOrder(const lrvx::Order& o)
{
  PySignal ps{};
  ps.order_id = o.id;
  ps.symbol = o.symbol;
  ps.side = (o.side == lrvx::Side::BUY) ? "buy" : "sell";
  switch (o.type)
  {
    case lrvx::OrderType::LIMIT:
      ps.order_type = "limit";
      break;
    case lrvx::OrderType::MARKET:
      ps.order_type = "market";
      break;
    case lrvx::OrderType::STOP_MARKET:
      ps.order_type = "stop_market";
      break;
    case lrvx::OrderType::STOP_LIMIT:
      ps.order_type = "stop_limit";
      break;
    case lrvx::OrderType::TAKE_PROFIT_MARKET:
      ps.order_type = "tp_market";
      break;
    case lrvx::OrderType::TAKE_PROFIT_LIMIT:
      ps.order_type = "tp_limit";
      break;
    case lrvx::OrderType::TRAILING_STOP:
      ps.order_type = "trailing_stop";
      break;
    case lrvx::OrderType::ICEBERG:
      ps.order_type = "iceberg";
      break;
  }
  ps.price = o.price.toDouble();
  ps.quantity = o.quantity.toDouble();
  ps.trigger_price = o.triggerPrice.toDouble();
  ps.trailing_offset = o.trailingOffset.toDouble();
  return ps;
}

class PyRiskManagerCxxAdapter : public lrvx::IRiskManager
{
 public:
  explicit PyRiskManagerCxxAdapter(std::shared_ptr<PyRiskManager> delegate)
      : _delegate(std::move(delegate))
  {
  }
  bool allow(const lrvx::Order& order) const override
  {
    // Same rule as the C-ABI bridge: no answer means deny.
    bool result = false;
    invokeUnderGil([&]
                   { result = _delegate->allow(pySignalFromOrder(order)); },
                   "RiskManager.allow");
    return result;
  }

 private:
  std::shared_ptr<PyRiskManager> _delegate;
};

class PyKillSwitchCxxAdapter : public lrvx::IKillSwitch
{
 public:
  explicit PyKillSwitchCxxAdapter(std::shared_ptr<PyKillSwitch> delegate)
      : _delegate(std::move(delegate))
  {
  }
  // Engine contract: check(order) may flip state; isTriggered() reads
  // it. The Python check() returns True=allow / False=halt, so we
  // store its inverse as `_triggered`.
  void check(const lrvx::Order& order) override
  {
    // A check that raised did not clear trading, so it halts — and this
    // interface latches, so a broken kill switch stops the run rather
    // than being asked again on the next order.
    bool allowed = false;
    invokeUnderGil([&]
                   { allowed = _delegate->check(pySignalFromOrder(order)); },
                   "KillSwitch.check");
    if (!allowed)
    {
      _triggered = true;
    }
  }
  void trigger(const std::string& reason) override
  {
    _triggered = true;
    _reason = reason;
  }
  bool isTriggered() const override { return _triggered; }
  std::string reason() const override { return _reason; }

 private:
  std::shared_ptr<PyKillSwitch> _delegate;
  bool _triggered{false};
  std::string _reason;
};

class PyOrderValidatorCxxAdapter : public lrvx::IOrderValidator
{
 public:
  explicit PyOrderValidatorCxxAdapter(std::shared_ptr<PyOrderValidator> delegate)
      : _delegate(std::move(delegate))
  {
  }
  // The Python validator returns True=valid / False=reject. We do not
  // expose a reason channel — bindings that want richer messaging
  // can raise via the engine's REJECTED order event.
  bool validate(const lrvx::Order& order, std::string& /*reason*/) const override
  {
    // Same rule as the C-ABI bridge: no answer means reject.
    bool valid = false;
    invokeUnderGil([&]
                   { valid = _delegate->validate(pySignalFromOrder(order)); },
                   "OrderValidator.validate");
    return valid;
  }

 private:
  std::shared_ptr<PyOrderValidator> _delegate;
};

class PyPnLTrackerCxxAdapter : public lrvx::IPnLTracker
{
 public:
  explicit PyPnLTrackerCxxAdapter(std::shared_ptr<PyPnLTracker> delegate)
      : _delegate(std::move(delegate))
  {
  }
  void onOrderFilled(const lrvx::Order& order) override
  {
    invokeUnderGil([&]
                   { _delegate->on_signal(pySignalFromOrder(order)); });
  }

 private:
  std::shared_ptr<PyPnLTracker> _delegate;
};

}  // namespace cxx_adapters

}  // namespace lrvx
