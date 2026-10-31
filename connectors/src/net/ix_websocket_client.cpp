/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx-connectors/net/ix_websocket_client.h"
#include "lrvx/util/concurrency/thread_body.h"

#include <atomic>
#include <condition_variable>
#include <string>

namespace lrvx
{

struct IxWebSocketClient::Core
{
  Core(std::string url, std::string origin, int reconnectDelayMs, ILogger* logger,
       int pingIntervalSec, std::string userAgent)
      : _url(std::move(url)),
        _origin(std::move(origin)),
        _reconnectDelayMs(reconnectDelayMs),
        _pingIntervalSec(pingIntervalSec),
        _userAgent(std::move(userAgent)),
        _logger(logger),
        _ws(std::make_unique<ix::WebSocket>())
  {
  }

  void run();

  // Clears _running and wakes the reconnect backoff. Passing through
  // _backoffMutex first means run() is either already waiting or has yet to
  // test its predicate, so the notification cannot fall between the two.
  void requestStop()
  {
    _running.store(false);
    {
      std::lock_guard backoff(_backoffMutex);
    }
    _backoffCv.notify_all();
  }

  const std::string _url;
  const std::string _origin;
  const int _reconnectDelayMs;
  const int _pingIntervalSec;
  const std::string _userAgent;
  ILogger* const _logger;

  std::atomic<bool> _running{false};
  // Set by a destructor that runs inside a callback: the owner is gone, so
  // no callback is invoked and no log line is written from then on.
  std::atomic<bool> _orphaned{false};

  // Replaced by run() under _sendMutex on every connection attempt.
  std::unique_ptr<ix::WebSocket> _ws;
  std::mutex _sendMutex;

  // Guards the reconnect backoff wait, which stop() interrupts through
  // _backoffCv so shutdown never sleeps out a backoff.
  std::mutex _backoffMutex;
  std::condition_variable _backoffCv;

  MoveOnlyFunction<void()> _onOpen;
  MoveOnlyFunction<void(std::string_view)> _onMessage;
  MoveOnlyFunction<void(int, std::string_view)> _onClose;

  // Reset to 0 on Open from the ix callback thread, read and incremented
  // from run().
  std::atomic<int> _consecutiveFailures{0};
};

namespace
{

// The core whose callback this thread is running, if any. It tells a call
// from inside a callback, which cannot wait for the thread it is running on,
// from a call on any other thread.
thread_local const IxWebSocketClient::Core* tlDispatching = nullptr;

struct DispatchScope
{
  explicit DispatchScope(const IxWebSocketClient::Core* core) : _prev(tlDispatching)
  {
    tlDispatching = core;
  }
  ~DispatchScope() { tlDispatching = _prev; }
  DispatchScope(const DispatchScope&) = delete;
  DispatchScope& operator=(const DispatchScope&) = delete;

 private:
  const IxWebSocketClient::Core* _prev;
};

}  // namespace

IxWebSocketClient::IxWebSocketClient(std::string url, std::string origin, int reconnectDelayMs,
                                     ILogger* logger, int pingIntervalSec, std::string userAgent)
    : _core(std::make_shared<Core>(std::move(url), std::move(origin), reconnectDelayMs, logger,
                                   pingIntervalSec, std::move(userAgent)))
{
}

IxWebSocketClient::~IxWebSocketClient()
{
  if (tlDispatching != _core.get())
  {
    stop();
    return;
  }

  // Inside one of this client's callbacks: the run() thread is waiting for
  // this very thread, so it cannot be joined here. The run() thread co-owns
  // the core and touches nothing else, so it may outlive this object.
  _core->_orphaned.store(true, std::memory_order_release);
  _core->requestStop();
  if (_thread.joinable())
  {
    _thread.detach();
  }
}

bool IxWebSocketClient::refuseFromCallback(const char* what) const
{
  if (tlDispatching != _core.get())
  {
    return false;
  }
  _core->_logger->error(std::string("IxWebSocketClient::") + what +
                        " called from inside a callback; ignored");
  return true;
}

void IxWebSocketClient::onOpen(MoveOnlyFunction<void()> cb)
{
  if (!refuseFromCallback("onOpen"))
  {
    _core->_onOpen = std::move(cb);
  }
}

void IxWebSocketClient::onMessage(MoveOnlyFunction<void(std::string_view)> cb)
{
  if (!refuseFromCallback("onMessage"))
  {
    _core->_onMessage = std::move(cb);
  }
}

void IxWebSocketClient::onClose(MoveOnlyFunction<void(int, std::string_view)> cb)
{
  if (!refuseFromCallback("onClose"))
  {
    _core->_onClose = std::move(cb);
  }
}

void IxWebSocketClient::start()
{
  if (refuseFromCallback("start"))
  {
    return;
  }

  std::lock_guard lifecycle(_lifecycleMutex);
  if (_thread.joinable())
  {
    if (_core->_running.load())
    {
      return;
    }
    // A stop() issued from inside a callback left run() to finish on its
    // own; collect it before arming the next one.
    _thread.join();
  }

  _core->_running.store(true);
  _thread = makeThread("conn.ws",
                       [core = _core]
                       {
                         core->run();
                       });
}

void IxWebSocketClient::stop()
{
  // stop() does not touch the socket. The run() thread owns it: it replaces
  // it on every reconnect attempt and stops it once its wait loop observes
  // _running == false, and ix::WebSocket::stop() joins ix's worker thread,
  // which is the thread every callback runs on. Joining run() here therefore
  // means that once stop() returns no callback is running or will run again.
  // The cost is one wait-loop tick plus ix's own close.
  if (tlDispatching == _core.get())
  {
    _core->requestStop();
    return;
  }

  std::lock_guard lifecycle(_lifecycleMutex);
  _core->requestStop();
  if (_thread.joinable())
  {
    _thread.join();
  }
}

bool IxWebSocketClient::send(const std::string& data)
{
  std::lock_guard lock(_core->_sendMutex);
  return _core->_ws->send(data).success;
}

void IxWebSocketClient::Core::run()
{
  constexpr int MAX_BACKOFF_MS = 30000;

  while (_running)
  {
    // Fresh socket every attempt — avoids stale TLS/frame state
    {
      std::lock_guard lock(_sendMutex);
      _ws = std::make_unique<ix::WebSocket>();
    }

    _ws->disableAutomaticReconnection();
    _ws->setUrl(_url);
    if (!_origin.empty())
    {
      if (_userAgent.empty())
      {
        _ws->setExtraHeaders({{"Origin", _origin}});
      }
      else
      {
        _ws->setExtraHeaders({{"Origin", _origin}, {"User-Agent", _userAgent}});
      }
    }
    else if (!_userAgent.empty())
    {
      _ws->setExtraHeaders({{"User-Agent", _userAgent}});
    }
    _ws->disablePerMessageDeflate();
    if (_pingIntervalSec > 0)
    {
      _ws->setPingInterval(_pingIntervalSec);
    }
    else
    {
      _ws->setPingInterval(-1);
    }

    // Local flag — only THIS iteration's callbacks can set it.
    // Eliminates race between old socket callbacks and new socket state.
    std::atomic<bool> connectionClosed{false};

    _ws->setOnMessageCallback(
        [this, &connectionClosed](const ix::WebSocketMessagePtr& msg)
        {
          DispatchScope dispatching(this);
          if (_orphaned.load(std::memory_order_acquire))
          {
            if (msg->type == ix::WebSocketMessageType::Close ||
                msg->type == ix::WebSocketMessageType::Error)
            {
              connectionClosed.store(true, std::memory_order_release);
            }
            return;
          }
          switch (msg->type)
          {
            case ix::WebSocketMessageType::Open:
              _logger->info("WebSocket connected to " + _url);
              _consecutiveFailures = 0;
              if (_onOpen)
              {
                _onOpen();
              }
              break;

            case ix::WebSocketMessageType::Message:
              if (_onMessage)
              {
                _onMessage(msg->str);
              }
              break;

            case ix::WebSocketMessageType::Close:
              if (_onClose)
              {
                _onClose(msg->closeInfo.code, msg->closeInfo.reason);
              }
              connectionClosed.store(true, std::memory_order_release);
              break;

            case ix::WebSocketMessageType::Error:
              _logger->error("WebSocket error connecting to " + _url + ": " +
                             msg->errorInfo.reason);
              connectionClosed.store(true, std::memory_order_release);
              break;

            default:
              break;
          }
        });

    _ws->start();

    // Wait for close/error callback or stop signal — NOT polling getReadyState()
    while (_running && !connectionClosed.load(std::memory_order_acquire))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Ensure socket is fully stopped before creating a new one. Sole owner of
    // this call -- see stop(). The catch is belt-and-braces: an exception
    // escaping a thread body is std::terminate, and no teardown problem on one
    // socket is worth the process.
    try
    {
      _ws->stop();
    }
    catch (const std::exception& e)
    {
      if (!_orphaned.load(std::memory_order_acquire))
      {
        _logger->error("WebSocket stop threw (continuing): " + std::string(e.what()));
      }
    }

    if (_running)
    {
      const int failures = ++_consecutiveFailures;
      int backoffMs = std::min(_reconnectDelayMs * (1 << std::min(failures, 4)), MAX_BACKOFF_MS);
      _logger->warn("WebSocket disconnected, retrying in " + std::to_string(backoffMs) +
                    "ms... (attempt " + std::to_string(failures) + ")");
      // Interruptible wait: stop() calls _backoffCv.notify_all() so shutdown
      // does not have to sleep out the remainder of a backoff up to
      // MAX_BACKOFF_MS -- it returns as soon as _running goes
      // false, same tick-latency budget as the connectionClosed wait above.
      std::unique_lock<std::mutex> backoffLock(_backoffMutex);
      _backoffCv.wait_for(backoffLock, std::chrono::milliseconds(backoffMs),
                          [this]
                          {
                            return !_running.load();
                          });
    }
  }
}

}  // namespace lrvx
