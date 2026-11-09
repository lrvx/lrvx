#pragma once

#include "js_engine.h"

#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/engine/symbol_registry.h"

#include <memory>
#include <string>
#include <vector>

namespace lrvx
{

class LrvxJsStrategy
{
 public:
  LrvxJsStrategy(const std::string& scriptPath, SymbolRegistry& registry);
  ~LrvxJsStrategy();

  LrvxStrategyCallbacks getCallbacks();
  const std::vector<uint32_t>& symbolIds() const { return _symbolIds; }
  // Whether the registered strategy has its own onTouch. Without one it gets
  // no on_touch slot: most depth deltas move the touch, and each call would
  // build a context and a book object for the base class's empty method.
  bool wantsTouch() const { return _wantsTouch; }
  LrvxJsEngine& engine() { return _engine; }
  void injectHandle(LrvxStrategyHandle handle);

 private:
  void loadStdlib();
  void loadScript(const std::string& path);
  void resolveSymbols();

  // Common body for every JS-side dispatch: looks up `jsPropName` on the
  // registered strategy object, skips silently if the script never called
  // `lrvx.register()` (previously every dispatcher threw and clobbered
  // whatever exception the last one left pending), resets the
  // interrupt-handler clock so a runaway callback is interrupted rather
  // than hanging forever, calls it, reports a thrown exception the same
  // way every other dispatcher does, and drains the microtask queue so
  // `await`/`.then()` inside the callback actually completes.
  void invokeMethod(const char* jsPropName, int argc, JSValue* argv, const char* errLabel);

  // C callbacks dispatched from BridgeStrategy
  static void onTrade(void* userData, const LrvxSymbolContext* ctx, const LrvxTradeData* trade);
  static void onBook(void* userData, const LrvxSymbolContext* ctx, const LrvxBookData* book);
  static void onTouch(void* userData, const LrvxSymbolContext* ctx, const LrvxBookData* book);
  static void onBar(void* userData, const LrvxSymbolContext* ctx, const LrvxBarData* bar);
  static void onFill(void* userData, const LrvxSymbolContext* ctx,
                     const LrvxOrderEventData* ev);
  static void onOrderUpdate(void* userData, const LrvxSymbolContext* ctx,
                            const LrvxOrderEventData* ev);
  static void onQueuePositionChange(void* userData, const LrvxSymbolContext* ctx,
                                    const LrvxOrderEventData* ev);
  static void onMarketPositionChange(void* userData, const LrvxSymbolContext* ctx,
                                     const LrvxOrderEventData* ev);
  static void onStart(void* userData);
  static void onStop(void* userData);

  JSValue makeCtxObject(const LrvxSymbolContext* ctx);
  JSValue makeTradeObject(const LrvxTradeData* trade);
  JSValue makeBookObject(const LrvxBookData* book);
  JSValue makeBookSnapshotObject(const LrvxBookSnapshot* snap);
  JSValue makeBarObject(const LrvxBarData* bar);
  JSValue makeOrderEventObject(const LrvxOrderEventData* ev);

  LrvxJsEngine _engine;
  SymbolRegistry& _registry;
  JSValue _strategyObj = JS_UNDEFINED;
  std::vector<uint32_t> _symbolIds;
  std::vector<std::string> _symbolNames;
  bool _wantsTouch = false;
};

}  // namespace lrvx
