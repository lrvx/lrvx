/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include <ixwebsocket/IXWebSocket.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <lrvx/log/abstract_logger.h>
#include <lrvx/net/abstract_websocket_client.h>
#include <lrvx/util/base/move_only_function.h>

namespace lrvx
{

// Callback threading. Every onOpen, onMessage and onClose callback runs on
// ix's worker thread; run() owns the socket on a thread of its own and joins
// that worker whenever it stops a socket.
//
//  - stop() from any other thread returns once run() has exited: no callback
//    is running or will run after it, so the owner may release anything the
//    callbacks capture.
//  - stop() from inside a callback only requests the stop, since the thread
//    cannot wait for itself. The next start() or the destructor collects the
//    run() thread.
//  - start() and the onOpen/onMessage/onClose setters called from inside a
//    callback are refused with an error log: they would replace or restart
//    what is executing on that very thread.
//  - The destructor may run inside a callback (an owner that stops and
//    releases the client from its own disconnect handler). Every piece of
//    state run() and the ix worker use lives in a shared Core that the run()
//    thread co-owns, so the destructor marks the core orphaned, requests the
//    stop and detaches run(). An orphaned core invokes no callback and
//    writes no log line, so nothing reaches the owner or its logger after
//    the destructor returns; run() stops the socket, joins the worker and
//    releases the core.
class IxWebSocketClient : public IWebSocketClient
{
 public:
  IxWebSocketClient(std::string url, std::string origin, int reconnectDelayMs, ILogger* logger,
                    int pingIntervalSec = 1, std::string userAgent = "");
  ~IxWebSocketClient() override;

  void onOpen(MoveOnlyFunction<void()> cb) override;
  void onMessage(MoveOnlyFunction<void(std::string_view)> cb) override;
  void onClose(MoveOnlyFunction<void(int, std::string_view)> cb) override;

  // Returns whether the frame was actually handed to the socket
  // (ix::WebSocket::send()'s own success flag). false on a closed/not-yet-open
  // socket -- callers that need the frame delivered (e.g. gap resync) must
  // check it instead of assuming the write always lands.
  bool send(const std::string& data) override;
  void start() override;
  void stop() override;

  struct Core;

 private:
  bool refuseFromCallback(const char* what) const;

  std::shared_ptr<Core> _core;
  std::thread _thread;
  // Serialises start() and stop() from outside callbacks, so two threads
  // never join _thread at once, start() never overwrites a joinable one, and
  // a start() never re-arms a run() that a stop() is collecting.
  std::mutex _lifecycleMutex;
};

}  // namespace lrvx