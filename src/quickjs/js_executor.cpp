#include "js_executor.h"
#include "lrvx/util/concurrency/thread_body.h"

namespace lrvx
{

const std::vector<uint32_t> LrvxJsExecutor::kEmptySymbolIds;

LrvxJsExecutor::LrvxJsExecutor(const std::string& scriptPath, SymbolRegistry& registry,
                               size_t queueCapacity)
    : _scriptPath(scriptPath), _registry(registry), _capacity(queueCapacity)
{
  _thread = makeThread(
      "lrvx.quickjs", [this]
      { run(); }, [this]
      { releaseWaiters(); });

  std::unique_lock<std::mutex> lock(_initMu);
  _initCv.wait(lock, [this]
               { return _initDone; });
  if (_initError)
  {
    lock.unlock();
    if (_thread.joinable())
    {
      _thread.join();
    }
    std::rethrow_exception(_initError);
  }
}

LrvxJsExecutor::~LrvxJsExecutor()
{
  {
    std::lock_guard<std::mutex> lock(_queueMu);
    _closed = true;
  }
  _notEmpty.notify_all();
  _notFull.notify_all();
  if (_thread.joinable())
  {
    _thread.join();
  }
}

void LrvxJsExecutor::releaseWaiters() noexcept
{
  std::deque<Event> pending;
  {
    std::lock_guard<std::mutex> lock(_queueMu);
    _closed = true;
    pending.swap(_queue);
  }
  // The same two notifications the destructor sends: push() blocks a bus
  // consumer thread when the queue is full, and that thread must not wait on
  // a worker that is no longer there.
  _notEmpty.notify_all();
  _notFull.notify_all();

  // Clearing the queue is not enough for the callers parked on a future: they
  // hold the promise themselves, so it stays alive and unsatisfied. Break it
  // explicitly, and they wake with an error instead of never waking.
  const auto err = std::make_exception_ptr(
      std::runtime_error("lrvx: the QuickJS executor thread died; request abandoned"));
  for (auto& ev : pending)
  {
    if (ev.barrier)
    {
      ev.barrier->set_exception(err);
    }
    if (ev.intResult)
    {
      ev.intResult->set_exception(err);
    }
  }
}

void LrvxJsExecutor::push(Event&& ev)
{
  std::unique_lock<std::mutex> lock(_queueMu);
  // Bounded queue, backpressure on overflow. This fix must never trade
  // a memory-corruption bug for a silent-data-loss one: a full queue
  // blocks the producer (a bus consumer thread) rather than dropping the
  // event.
  _notFull.wait(lock, [this]
                { return _closed || _queue.size() < _capacity; });
  if (_closed)
  {
    return;
  }
  _queue.push_back(std::move(ev));
  lock.unlock();
  _notEmpty.notify_one();
}

void LrvxJsExecutor::run()
{
  try
  {
    _strategy = std::make_unique<LrvxJsStrategy>(_scriptPath, _registry);
    // The strategy (and its LrvxJsEngine) were constructed on this thread,
    // so LrvxJsEngine's default owner-thread binding is already correct.
    // rebindOwnerThread() is only needed for embeddings that move
    // construction across threads; call it anyway to make the invariant
    // explicit rather than relying on constructor-thread coincidence.
    _strategy->engine().rebindOwnerThread();
  }
  catch (...)
  {
    std::lock_guard<std::mutex> lock(_initMu);
    _initError = std::current_exception();
    _initDone = true;
    _initCv.notify_all();
    return;
  }

  {
    std::lock_guard<std::mutex> lock(_initMu);
    _initDone = true;
  }
  _initCv.notify_all();

  auto cb = _strategy->getCallbacks();

  for (;;)
  {
    Event ev;
    {
      std::unique_lock<std::mutex> lock(_queueMu);
      _notEmpty.wait(lock, [this]
                     { return _closed || !_queue.empty(); });
      if (_queue.empty())
      {
        // Closed and fully drained.
        break;
      }
      ev = std::move(_queue.front());
      _queue.pop_front();
      lock.unlock();
      _notFull.notify_one();
    }
    dispatchOne(ev, cb);
  }

  // LrvxJsStrategy's destructor touches the runtime (it evaluates a
  // cleanup script to drop JS-held references before teardown). It must
  // run here, on the thread that owns the engine -- not later, whenever
  // LrvxJsExecutor's own destructor happens to run on the caller's
  // thread. std::unique_ptr's implicit destruction would get that wrong.
  _strategy.reset();
}

void LrvxJsExecutor::dispatchOne(Event& ev, const LrvxStrategyCallbacks& cb)
{
  switch (ev.kind)
  {
    case Kind::Trade:
      if (cb.on_trade)
      {
        cb.on_trade(cb.user_data, &ev.ctx, &ev.trade);
      }
      break;
    case Kind::Book:
      if (cb.on_book)
      {
        cb.on_book(cb.user_data, &ev.ctx, &ev.book);
      }
      break;
    case Kind::Touch:
      if (cb.on_touch)
      {
        cb.on_touch(cb.user_data, &ev.ctx, &ev.book);
      }
      break;
    case Kind::Bar:
      if (cb.on_bar)
      {
        cb.on_bar(cb.user_data, &ev.ctx, &ev.bar);
      }
      break;
    case Kind::Fill:
    case Kind::OrderUpdate:
    case Kind::QueuePositionChange:
    case Kind::MarketPositionChange:
      ev.orderEv.reject_reason =
          ev.rejectReasonStorage.empty() ? nullptr : ev.rejectReasonStorage.c_str();
      if (ev.kind == Kind::Fill && cb.on_fill)
      {
        cb.on_fill(cb.user_data, &ev.ctx, &ev.orderEv);
      }
      else if (ev.kind == Kind::OrderUpdate && cb.on_order_update)
      {
        cb.on_order_update(cb.user_data, &ev.ctx, &ev.orderEv);
      }
      else if (ev.kind == Kind::QueuePositionChange && cb.on_queue_position_change)
      {
        cb.on_queue_position_change(cb.user_data, &ev.ctx, &ev.orderEv);
      }
      else if (ev.kind == Kind::MarketPositionChange && cb.on_market_position_change)
      {
        cb.on_market_position_change(cb.user_data, &ev.ctx, &ev.orderEv);
      }
      break;
    case Kind::Start:
      if (cb.on_start)
      {
        cb.on_start(cb.user_data);
      }
      break;
    case Kind::Stop:
      if (cb.on_stop)
      {
        cb.on_stop(cb.user_data);
      }
      break;
    case Kind::InjectHandle:
      _strategy->injectHandle(ev.handle);
      break;
    case Kind::Barrier:
      break;
    case Kind::ReadGlobalInt32:
    {
      auto* ctx = _strategy->engine().context();
      JSValue v = _strategy->engine().getGlobalProperty(ev.globalName.c_str());
      int32_t out = 0;
      JS_ToInt32(ctx, &out, v);
      JS_FreeValue(ctx, v);
      if (ev.intResult)
      {
        ev.intResult->set_value(out);
      }
      break;
    }
  }
  if (ev.barrier)
  {
    ev.barrier->set_value();
  }
}

LrvxStrategyCallbacks LrvxJsExecutor::getCallbacks()
{
  LrvxStrategyCallbacks cb{};
  cb.on_trade = &LrvxJsExecutor::onTrade;
  cb.on_book = &LrvxJsExecutor::onBook;
  cb.on_bar = &LrvxJsExecutor::onBar;
  cb.on_start = &LrvxJsExecutor::onStart;
  cb.on_stop = &LrvxJsExecutor::onStop;
  cb.on_fill = &LrvxJsExecutor::onFill;
  cb.on_order_update = &LrvxJsExecutor::onOrderUpdate;
  cb.on_queue_position_change = &LrvxJsExecutor::onQueuePositionChange;
  cb.on_market_position_change = &LrvxJsExecutor::onMarketPositionChange;
  cb.on_touch = (_strategy && _strategy->wantsTouch()) ? &LrvxJsExecutor::onTouch : nullptr;
  cb.user_data = this;
  return cb;
}

const std::vector<uint32_t>& LrvxJsExecutor::symbolIds() const
{
  return _strategy ? _strategy->symbolIds() : kEmptySymbolIds;
}

void LrvxJsExecutor::injectHandle(LrvxStrategyHandle handle)
{
  Event ev;
  ev.kind = Kind::InjectHandle;
  ev.handle = handle;
  push(std::move(ev));
  waitIdle();
}

void LrvxJsExecutor::waitIdle()
{
  auto prom = std::make_shared<std::promise<void>>();
  auto fut = prom->get_future();
  Event ev;
  ev.kind = Kind::Barrier;
  ev.barrier = prom;
  push(std::move(ev));
  fut.wait();
}

size_t LrvxJsExecutor::queueDepthApprox() const
{
  std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(_queueMu));
  return _queue.size();
}

int32_t LrvxJsExecutor::getGlobalInt32(const std::string& name)
{
  auto prom = std::make_shared<std::promise<int32_t>>();
  auto fut = prom->get_future();
  Event ev;
  ev.kind = Kind::ReadGlobalInt32;
  ev.globalName = name;
  ev.intResult = prom;
  push(std::move(ev));
  return fut.get();
}

void LrvxJsExecutor::onTrade(void* ud, const LrvxSymbolContext* ctx, const LrvxTradeData* trade)
{
  auto* self = static_cast<LrvxJsExecutor*>(ud);
  Event ev;
  ev.kind = Kind::Trade;
  ev.ctx = *ctx;
  ev.trade = *trade;
  self->push(std::move(ev));
}

void LrvxJsExecutor::onBook(void* ud, const LrvxSymbolContext* ctx, const LrvxBookData* book)
{
  auto* self = static_cast<LrvxJsExecutor*>(ud);
  Event ev;
  ev.kind = Kind::Book;
  ev.ctx = *ctx;
  ev.book = *book;
  self->push(std::move(ev));
}

void LrvxJsExecutor::onTouch(void* ud, const LrvxSymbolContext* ctx, const LrvxBookData* book)
{
  auto* self = static_cast<LrvxJsExecutor*>(ud);
  Event ev;
  ev.kind = Kind::Touch;
  ev.ctx = *ctx;
  ev.book = *book;
  self->push(std::move(ev));
}

void LrvxJsExecutor::onBar(void* ud, const LrvxSymbolContext* ctx, const LrvxBarData* bar)
{
  auto* self = static_cast<LrvxJsExecutor*>(ud);
  Event ev;
  ev.kind = Kind::Bar;
  ev.ctx = *ctx;
  ev.bar = *bar;
  self->push(std::move(ev));
}

LrvxJsExecutor::Event LrvxJsExecutor::makeOrderEvent(const LrvxSymbolContext* ctx,
                                                     const LrvxOrderEventData* data)
{
  Event ev;
  ev.ctx = *ctx;
  ev.orderEv = *data;
  if (data->reject_reason)
  {
    ev.rejectReasonStorage = data->reject_reason;
  }
  return ev;
}

void LrvxJsExecutor::onFill(void* ud, const LrvxSymbolContext* ctx, const LrvxOrderEventData* ev)
{
  auto* self = static_cast<LrvxJsExecutor*>(ud);
  auto e = makeOrderEvent(ctx, ev);
  e.kind = Kind::Fill;
  self->push(std::move(e));
}

void LrvxJsExecutor::onOrderUpdate(void* ud, const LrvxSymbolContext* ctx,
                                   const LrvxOrderEventData* ev)
{
  auto* self = static_cast<LrvxJsExecutor*>(ud);
  auto e = makeOrderEvent(ctx, ev);
  e.kind = Kind::OrderUpdate;
  self->push(std::move(e));
}

void LrvxJsExecutor::onQueuePositionChange(void* ud, const LrvxSymbolContext* ctx,
                                           const LrvxOrderEventData* ev)
{
  auto* self = static_cast<LrvxJsExecutor*>(ud);
  auto e = makeOrderEvent(ctx, ev);
  e.kind = Kind::QueuePositionChange;
  self->push(std::move(e));
}

void LrvxJsExecutor::onMarketPositionChange(void* ud, const LrvxSymbolContext* ctx,
                                            const LrvxOrderEventData* ev)
{
  auto* self = static_cast<LrvxJsExecutor*>(ud);
  auto e = makeOrderEvent(ctx, ev);
  e.kind = Kind::MarketPositionChange;
  self->push(std::move(e));
}

void LrvxJsExecutor::onStart(void* ud)
{
  auto* self = static_cast<LrvxJsExecutor*>(ud);
  Event ev;
  ev.kind = Kind::Start;
  self->push(std::move(ev));
  // start() is setup-time, not a hot-path market-data callback; wait so
  // callers that assume "started" means "onStart already ran" (the
  // contract a bare LrvxJsStrategy has always had) keep seeing that.
  self->waitIdle();
}

void LrvxJsExecutor::onStop(void* ud)
{
  auto* self = static_cast<LrvxJsExecutor*>(ud);
  Event ev;
  ev.kind = Kind::Stop;
  self->push(std::move(ev));
  // BridgeStrategy::stop() (and process exit right after it, in
  // lrvx_js_runner) does not wait for the callback to actually run. Drain
  // here so onStop -- and anything still ahead of it in the queue -- has
  // completed by the time stop() returns, matching the single-threaded
  // LrvxJsStrategy contract callers already depend on.
  self->waitIdle();
}

}  // namespace lrvx
