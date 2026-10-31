/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Loopback websocket helpers for the offline connector tests. Everything binds
 * 127.0.0.1 on an ephemeral port.
 */

#pragma once

// ix's own header-only Sec-WebSocket-Accept generator, so the server completes
// a real handshake without a hand-rolled SHA1.
#include <ixwebsocket/IXWebSocketHandshakeKeyGen.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lrvx::test
{

inline int bindLoopback(uint16_t& port)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  port = ntohs(addr.sin_port);
  return fd;
}

// A websocket URL on a loopback port that was bound and released, so nothing
// listens there and every connection attempt is refused.
inline std::string refusingUrl()
{
  uint16_t port = 0;
  const int fd = bindLoopback(port);
  ::close(fd);
  return "ws://127.0.0.1:" + std::to_string(port);
}

// Completes the websocket handshake on every connection, then sends the same
// unmasked text frame back to back until the peer goes away. Nothing the
// client sends is read, so its close frame goes unanswered and ix waits out
// its own close timeout: the delivery thread stays busy for the whole stop.
// Writes to a vanished peer fail with EPIPE on this socket instead of raising
// SIGPIPE in the process.
class StreamingServer
{
 public:
  explicit StreamingServer(const std::string& payload) : _frame(encodeFrame(payload))
  {
    _fd = bindLoopback(_port);
    ::listen(_fd, 16);
    _acceptThread = std::thread(
        [this]
        {
          while (_running.load())
          {
            const int c = ::accept(_fd, nullptr, nullptr);
            if (c < 0)
            {
              break;
            }
#ifdef SO_NOSIGPIPE
            int one = 1;
            ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            std::lock_guard<std::mutex> lk(_m);
            _clients.push_back(c);
            _streams.emplace_back(
                [this, c]
                {
                  stream(c);
                });
          }
        });
  }

  ~StreamingServer()
  {
    _running.store(false);
    ::shutdown(_fd, SHUT_RDWR);
    ::close(_fd);
    if (_acceptThread.joinable())
    {
      _acceptThread.join();
    }
    std::lock_guard<std::mutex> lk(_m);
    for (int c : _clients)
    {
      ::shutdown(c, SHUT_RDWR);
    }
    for (auto& t : _streams)
    {
      t.join();
    }
    for (int c : _clients)
    {
      ::close(c);
    }
  }

  StreamingServer(const StreamingServer&) = delete;
  StreamingServer& operator=(const StreamingServer&) = delete;

  std::string url() const { return "ws://127.0.0.1:" + std::to_string(_port); }

 private:
  static std::string encodeFrame(const std::string& payload)
  {
    std::string f;
    f.push_back(static_cast<char>(0x81));  // FIN + text
    if (payload.size() < 126)
    {
      f.push_back(static_cast<char>(payload.size()));
    }
    else
    {
      f.push_back(static_cast<char>(126));
      f.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
      f.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    f += payload;
    return f;
  }

  static int sendFlags()
  {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
  }

  static bool answerHandshake(int c)
  {
    std::string req;
    char buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos)
    {
      const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
      if (n <= 0)
      {
        return false;
      }
      req.append(buf, static_cast<size_t>(n));
    }
    const std::string marker = "Sec-WebSocket-Key: ";
    const size_t at = req.find(marker);
    if (at == std::string::npos)
    {
      return false;
    }
    const size_t end = req.find("\r\n", at);
    const std::string key = req.substr(at + marker.size(), end - (at + marker.size()));

    char accept[29] = {};
    WebSocketHandshakeKeyGen::generate(key, accept);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    resp += accept;
    resp += "\r\n\r\n";
    return ::send(c, resp.data(), resp.size(), sendFlags()) > 0;
  }

  void stream(int c)
  {
    if (!answerHandshake(c))
    {
      return;
    }
    while (_running.load())
    {
      if (::send(c, _frame.data(), _frame.size(), sendFlags()) <= 0)
      {
        return;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }

  const std::string _frame;
  int _fd{-1};
  uint16_t _port{0};
  std::atomic<bool> _running{true};
  std::thread _acceptThread;
  std::mutex _m;
  std::vector<int> _clients;
  std::vector<std::thread> _streams;
};

}  // namespace lrvx::test
