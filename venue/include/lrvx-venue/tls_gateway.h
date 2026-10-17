/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include "lrvx-venue/cancel_on_disconnect.h"
#include "lrvx-venue/fix_session.h"
#include "lrvx-venue/messages.h"
#include "lrvx-venue/metrics.h"
#include "lrvx-venue/session.h"
#include "lrvx-venue/session_registry.h"
#include "lrvx-venue/socket_acceptor.h"
#include "lrvx-venue/tcp_gateway.h"  // setRecvTimeoutMs / wasRecvTimeout
#include "lrvx/util/transport.h"

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "lrvx/net/socket.h"

#include <csignal>

namespace lrvx::venue
{
namespace tls
{

// Server context with a fresh in-memory self-signed P-256 certificate.
inline SSL_CTX* serverCtx()
{
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  EVP_PKEY* pkey = EVP_EC_gen("P-256");
  X509* x = X509_new();
  ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
  X509_gmtime_adj(X509_getm_notBefore(x), 0);
  X509_gmtime_adj(X509_getm_notAfter(x), 31536000L);
  X509_set_pubkey(x, pkey);
  // The subject is built on its own and set, not edited through the
  // certificate's getter: OpenSSL 4 hands that name out const. The setters
  // copy it, so the local is freed here.
  X509_NAME* name = X509_NAME_new();
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
  X509_set_subject_name(x, name);
  X509_set_issuer_name(x, name);
  X509_NAME_free(name);
  X509_sign(x, pkey, EVP_sha256());
  SSL_CTX_use_certificate(ctx, x);
  SSL_CTX_use_PrivateKey(ctx, pkey);
  X509_free(x);
  EVP_PKEY_free(pkey);
  return ctx;
}

inline SSL_CTX* clientCtx()
{
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);  // self-signed test cert
  return ctx;
}

inline bool writeAll(SSL* s, const uint8_t* p, size_t n)
{
  size_t o = 0;
  while (o < n)
  {
    const int w = SSL_write(s, p + o, static_cast<int>(n - o));
    if (w <= 0)
    {
      return false;
    }
    o += static_cast<size_t>(w);
  }
  return true;
}
inline bool readAll(SSL* s, uint8_t* p, size_t n)
{
  size_t o = 0;
  while (o < n)
  {
    const int r = SSL_read(s, p + o, static_cast<int>(n - o));
    if (r <= 0)
    {
      return false;
    }
    o += static_cast<size_t>(r);
  }
  return true;
}
inline bool writeFrame(SSL* s, const uint8_t* p, size_t n)
{
  const uint8_t h[4] = {static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
                        static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
  return writeAll(s, h, 4) && writeAll(s, p, n);
}
inline bool readFrame(SSL* s, std::vector<uint8_t>& out)
{
  uint8_t h[4];
  if (!readAll(s, h, 4))
  {
    return false;
  }
  const uint32_t len = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
  // Reject a hostile length prefix before allocating: a 4-byte header must not
  // reserve up to 4 GiB over the TLS channel. Shares the core framing cap so the
  // two cannot drift apart.
  if (len > lrvx::net::kMaxFrame)
  {
    return false;
  }
  out.resize(len);
  return len == 0 || readAll(s, out.data(), len);
}

}  // namespace tls

// The one place in the venue that still has to touch a process-wide signal
// disposition, and it is worth saying exactly why rather than leaving it as
// folklore.
//
// Everywhere else, a write to a peer that has gone away goes through
// net::sendNoSignal, which passes MSG_NOSIGNAL on Linux and relies on the
// SO_NOSIGPIPE set at accept on macOS -- no signal is raised and the writer
// gets EPIPE. Here the write is SSL_write: OpenSSL calls the socket itself
// and there is no way to hand it that flag. On macOS the per-socket option
// still covers it. On Linux there is no per-socket option at all, so the
// only remaining choices are to ignore the signal for the process or to mask
// it per thread -- and per thread would have to cover the SessionWriter's
// thread too, which this class does not own and does not create.
//
// So: once, on the first TLS gateway a process starts, and not before. A
// process that never speaks TLS never has its disposition changed. Without
// it a client that vanishes mid-write kills the venue outright, which is
// what the test suite showed the moment this was left out.
inline void suppressSigpipeForOpenSsl() noexcept
{
#if !defined(_WIN32)
  static const bool once = []
  {
    ::signal(SIGPIPE, SIG_IGN);
    return true;
  }();
  (void)once;
#endif
}

// Delivery over TLS: OpenSSL forbids CONCURRENT SSL_read/SSL_write on one
// SSL*, not serialized use from two threads. Each connection carries a
// std::mutex over its SSL*: the read loop establishes readiness OUTSIDE the
// lock (::poll on the fd / SSL_has_pending) and takes it only for the actual
// SSL_read; the SessionWriter's write callback takes it for the SSL_write of
// one frame -- the two interleave, never overlap, and an idle reader cannot
// starve the writer by camping on the mutex. SSL_read keeps its own
// partial-record state across WANT_READ, so a frame split across polls is
// resumed, never lost. Without a registry the gateway stays in the embedded
// per-frame responder mode, exactly like TcpGateway.
class TlsGateway
{
 public:
  using Responder = std::function<void(const uint8_t*, size_t)>;
  using Handler = DisconnectCanceller::Handler;  // (cmd, responder, recvMonoNs)

  // See TcpGateway: `account` binds each session so a client cannot spoof
  // another account's id (setAccounts for an endpoint that serves several).
  // account == 0 is single-tenant-trusted mode.
  explicit TlsGateway(GatewaySession::Decoder decoder, uint64_t account = 0)
      : decoder_(std::move(decoder)), account_(account), accounts_{account}, ctx_(tls::serverCtx())
  {
  }
  ~TlsGateway()
  {
    stop();
    if (ctx_)
    {
      SSL_CTX_free(ctx_);
    }
  }

  void setCancelOnDisconnect(bool on) noexcept { cancelOnDisconnect_.store(on); }

  // The accounts this endpoint serves, identity first. The ordinary shape of a
  // client bridge: one connection carrying the flow of many customers, each
  // with its own account at the venue. A command naming an account outside the
  // set is refused (`Unauthenticated`); reports for any account in it reach
  // this connection, and an event naming two of them arrives once.
  void setAccounts(std::vector<uint64_t> accounts)
  {
    accounts_ = std::move(accounts);
    if (accounts_.empty())
    {
      accounts_.push_back(0);
    }
    account_ = accounts_.front();
  }

  // Client rate limit for NEW sessions. A setting, not one venue's published
  // profile: `SessionRateLimit::off()` turns the limiter off outright, which
  // is the right answer for a bridge whose ordinary traffic is bursty and for
  // a venue that budgets admission somewhere else. The default is the profile
  // this used to hardwire, so an existing deployment is unchanged.
  void setRateLimit(SessionRateLimit limit) { rateLimit_ = limit; }

  // Encoder for a refusal that travels with text (see
  // SessionRegistry::RejectEncoder). Without one a rate-limit refusal still
  // reaches the client, just without the wait it has to observe.
  void setRejectEncoder(SessionRegistry::RejectEncoder enc) { rejectEncoder_ = std::move(enc); }

  // Delivery mode (see TcpGateway::setDelivery): register every connection in
  // `registry` and deliver exec reports through per-session bounded queues.
  void setDelivery(SessionRegistry* registry, SessionRegistry::Encoder encoder)
  {
    registry_ = registry;
    encoder_ = std::move(encoder);
  }

  // Session-layer verbs (SBE resend/snapshot; see session_verbs.h). Only
  // consulted in delivery mode.
  void setSessionVerbs(SessionVerbHandler verbs) { verbs_ = std::move(verbs); }

  // Wire negotiation of per-session config (SBE SetSessionConfig -> COD; see
  // session_verbs.h makeSbeSessionConfigVerb). Fire-and-forget.
  void setSessionConfigVerb(SessionConfigHandler h) { sessionCfg_ = std::move(h); }

  // FIX session layer over TLS (fix_session.h; requires delivery mode). Same
  // wire shape as TcpGateway -- one FIX message per length-prefixed frame,
  // here inside the TLS stream: outbound goes through the existing
  // per-connection sslMu writer path (writeFrameLocked adds the length
  // prefix, so the FixConnection needs no wire wrap), inbound through the
  // poll-before-lock read loop. The FIX timers run on the read loop's poll
  // tick; FIX liveness (TestRequest death) replaces the plain idle timeout.
  void setFixSession(FixSessionHost* host) noexcept { fixHost_ = host; }

  void setCounters(GatewayCounters* counters) noexcept { counters_ = counters; }

  // Liveness: no inbound bytes for this long closes the session (COD fires).
  void setIdleTimeout(std::chrono::milliseconds t) noexcept { idleTimeoutMs_.store(t.count()); }

  int start(uint16_t port, Handler handler)
  {
    handler_ = std::move(handler);
    suppressSigpipeForOpenSsl();
    // Contain any exception to this one connection -- connLoop is a thread body,
    // so an escape would reach std::terminate and take down the whole venue.
    // The acceptor owns the fd (closes it after the handler returns).
    return acceptor_.start(port, [this](net::Handle fd)
                           {
                             try { connLoop(fd); }
                             catch (...) {} });
  }
  void stop() { acceptor_.stop(); }
  int port() const noexcept { return acceptor_.port(); }

 private:
  // Delivery mode polls SSL_read at this interval so the session writer can
  // take the SSL mutex between polls (and the shutdown sweep is noticed).
  static constexpr int64_t kPollMs = 20;

  enum class ReadResult : uint8_t
  {
    Frame,
    IdleTimeout,
    Closed,
  };

  // One length-prefixed frame over TLS. The mutex is taken only while there is
  // something to read: readiness is established OUTSIDE the lock (::poll on
  // the fd, or SSL_has_pending for records already buffered inside the SSL),
  // so an idle reader holds the mutex for microseconds per poll interval and
  // the writer thread is never starved by an unfair unlock/relock cycle.
  // SSL_read's internal record state and the running byte offset preserve a
  // frame split across polls; SO_RCVTIMEO bounds the lock hold when poll
  // reported a partial record. SSL_ERROR_ZERO_RETURN / SSL_ERROR_SYSCALL
  // (peer close, shutdown sweep) end the session.
  // `tick` (FIX mode): run on every idle poll interval; returning false ends
  // the session (FIX liveness lost -- reported as IdleTimeout so the caller's
  // counter accounting matches the plain-idle path).
  ReadResult readFrameLocked(net::Handle fd, SSL* ssl, std::mutex& mu, std::vector<uint8_t>& out,
                             int64_t idleMs, std::chrono::steady_clock::time_point& lastInbound,
                             const std::function<bool()>* tick = nullptr)
  {
    uint8_t h[4];
    uint32_t len = 0;
    bool haveLen = false;
    size_t got = 0;
    out.clear();
    while (acceptor_.running())
    {
      bool ready;
      {
        std::lock_guard<std::mutex> lk(mu);
        ready = SSL_has_pending(ssl) != 0;
      }
      if (!ready)
      {
        const net::PollResult pr = net::pollRead(fd, static_cast<int>(kPollMs));
        if (pr.error)
        {
          return ReadResult::Closed;
        }
        // A signal cut the wait short and says nothing about the socket; an
        // elapsed wait is the idle tick below. Neither ends the session.
        ready = pr.readable || pr.hangup;
      }
      if (!ready)
      {
        if (tick != nullptr)
        {
          if (!(*tick)())
          {
            return ReadResult::IdleTimeout;  // FIX liveness lost (TestRequest death)
          }
        }
        else if (idleMs > 0)
        {
          const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - lastInbound)
                                .count();
          if (idle >= idleMs)
          {
            return ReadResult::IdleTimeout;
          }
        }
        continue;
      }
      uint8_t* dst = haveLen ? out.data() + got : h + got;
      const size_t want = (haveLen ? len : sizeof h) - got;
      int r;
      int err = SSL_ERROR_NONE;
      {
        std::lock_guard<std::mutex> lk(mu);
        net::clearLastError();
        r = SSL_read(ssl, dst, static_cast<int>(want));
        if (r <= 0)
        {
          err = SSL_get_error(ssl, r);  // before any other call touches the SSL*
        }
      }
      if (r > 0)
      {
        lastInbound = std::chrono::steady_clock::now();
        got += static_cast<size_t>(r);
        if (!haveLen)
        {
          if (got < sizeof h)
          {
            continue;
          }
          len = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
          if (len > lrvx::net::kMaxFrame)
          {
            return ReadResult::Closed;  // hostile length prefix (see tls::readFrame)
          }
          out.resize(len);
          haveLen = true;
          got = 0;
        }
        if (haveLen && got == len)
        {
          return ReadResult::Frame;
        }
        continue;
      }
      const bool retryable = err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE ||
                             (err == SSL_ERROR_SYSCALL && wasRecvTimeout());
      if (!retryable)
      {
        return ReadResult::Closed;
      }
    }
    return ReadResult::Closed;
  }

  // One frame under one mutex acquisition: header and body leave as a single
  // SSL_write so the writer holds the SSL* for a bounded, contiguous burst.
  static bool writeFrameLocked(SSL* ssl, std::mutex& mu, const uint8_t* p, size_t n)
  {
    std::vector<uint8_t> buf(4 + n);
    buf[0] = static_cast<uint8_t>(n >> 24);
    buf[1] = static_cast<uint8_t>(n >> 16);
    buf[2] = static_cast<uint8_t>(n >> 8);
    buf[3] = static_cast<uint8_t>(n);
    std::memcpy(buf.data() + 4, p, n);
    std::lock_guard<std::mutex> lk(mu);
    return tls::writeAll(ssl, buf.data(), buf.size());
  }

  void connLoop(net::Handle fd)
  {
    const int64_t idleMs = idleTimeoutMs_.load();
    setRecvTimeoutMs(fd, idleMs);
    SSL* ssl = SSL_new(ctx_);
    SSL_set_fd(ssl, fd);
    if (SSL_accept(ssl) <= 0)
    {
      SSL_free(ssl);
      return;  // acceptor owns the fd
    }
    GatewaySession session(accounts_, decoder_, rateLimit_.policy());
    session.setName("tls/" + std::to_string(account_));
    session.authenticate(true);
    session.setCancelOnDisconnect(cancelOnDisconnect_.load());
    DisconnectCanceller cod(session.cancelOnDisconnect());

    std::mutex sslMu;  // serializes SSL_read (reader) vs SSL_write (writer thread)
    std::shared_ptr<SessionWriter> writer;
    if (registry_ != nullptr)
    {
      // Handshake done: switch to the short poll so a blocked SSL_read never
      // holds the mutex longer than one poll interval.
      setRecvTimeoutMs(fd, idleMs > 0 ? std::min<int64_t>(idleMs, kPollMs) : kPollMs);
      writer = registry_->attach(
          session.accounts(), encoder_,
          [ssl, &sslMu](const uint8_t* p, size_t n)
          { return writeFrameLocked(ssl, sslMu, p, n); },
          [fd]
          { net::shutdownBoth(fd); },
          [&cod](const OutboundEvent& e)
          { cod.observe(e); },
          rejectEncoder_);
    }
    // In delivery mode the responder feeds the same per-session queue as the
    // routed events (single writer owns the SSL*'s write side).
    const Responder responder =
        (writer != nullptr)
            ? Responder([w = writer.get()](const uint8_t* p, size_t n)
                        { w->enqueue(std::vector<uint8_t>(p, p + n)); })
            : Responder([ssl](const uint8_t* p, size_t n)
                        { tls::writeFrame(ssl, p, n); });

    // FIX mode (see setFixSession): the FixConnection gates every inbound
    // frame; its timers run on the read loop's poll tick via the tick hook.
    const bool fixMode = fixHost_ != nullptr && writer != nullptr;
    std::unique_ptr<FixConnection> fix;
    std::function<bool()> fixTick;
    if (fixMode)
    {
      fix = std::make_unique<FixConnection>(*fixHost_, *registry_, session.account(),
                                            wallClockNs());
      fix->setCodListener([&session, &cod](bool on)
                          {
                            session.setCancelOnDisconnect(on);
                            cod.setEnabled(on); });
      fixTick = [&fix]
      { return fix->onTick(wallClockNs()); };
    }

    auto lastInbound = std::chrono::steady_clock::now();
    std::vector<uint8_t> frame;
    while (acceptor_.running())
    {
      const ReadResult rr = readFrameLocked(fd, ssl, sslMu, frame, idleMs, lastInbound,
                                            fixMode ? &fixTick : nullptr);
      if (rr != ReadResult::Frame)
      {
        if (rr == ReadResult::IdleTimeout && counters_ != nullptr)
        {
          counters_->idleDisconnects.fetch_add(1, std::memory_order_relaxed);
        }
        break;
      }
      if (fixMode)
      {
        const auto verdict =
            fix->onFrame(std::string(frame.begin(), frame.end()), wallClockNs());
        if (verdict == FixConnection::Verdict::Disconnect)
        {
          break;  // Logout exchanged / fatal session error; COD sweeps below
        }
        if (verdict == FixConnection::Verdict::Handled)
        {
          continue;  // session-layer message, fully consumed
        }
        // Verdict::App: fall through to the decoder / admission path.
      }
      if (sessionCfg_)
      {
        if (const auto u = sessionCfg_(frame.data(), frame.size()))
        {
          session.setCancelOnDisconnect(u->cancelOnDisconnect);
          cod.setEnabled(u->cancelOnDisconnect);
          continue;
        }
      }
      if (writer != nullptr && verbs_ &&
          verbs_(frame.data(), frame.size(), session.account(), *registry_))
      {
        continue;  // session-layer verb (resend / snapshot), fully handled
      }
      SessionReject rej{};
      RejectEcho echo{};
      std::string rejectText;
      // Real monotonic nanoseconds (rate-limit windows are wall-clock); the old
      // ++clock_ frame counter never advanced time -> permanent bans.
      const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
      auto cmd = session.handle(frame.data(), frame.size(), nowNs, rej, &echo, &rejectText);
      if (cmd)
      {
        cod.track(*cmd);
        handler_(*cmd, responder, nowNs);
      }
      else if (rej != SessionReject::None && registry_ != nullptr)
      {
        // A rejected frame answers with a sequenced exec-report reject instead
        // of the old silence -- same policy as TcpGateway, and it names the
        // frame it refused for every reason (GatewaySession::handle fills
        // `echo` from the decoded command, or from the ClOrdID legible in the
        // raw bytes when the frame did not decode).
        registry_->send(session.account(),
                        OutboundEvent{OrderRejected{echo.id, echo.symbol, toRejectReason(rej),
                                                    session.account(), echo.clientOrderId}},
                        rejectText);
      }
    }
    if (writer != nullptr)
    {
      // Order matters: detach (no new frames are routed here), stop (drain and
      // JOIN the writer thread -- its last SSL_write finishes) and only then
      // SSL_shutdown/SSL_free below. The writer never sees a freed SSL*.
      registry_->detach(session.accounts(), writer);
      writer->stop();
    }
    cod.flush(handler_);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    net::shutdownBoth(fd);  // acceptor owns the close
  }

  GatewaySession::Decoder decoder_;
  uint64_t account_{0};
  std::vector<uint64_t> accounts_{0};
  SessionRateLimit rateLimit_{};
  SessionRegistry::RejectEncoder rejectEncoder_;
  SSL_CTX* ctx_{nullptr};
  Handler handler_;
  SocketAcceptor acceptor_;
  std::atomic<bool> cancelOnDisconnect_{false};
  SessionRegistry* registry_{nullptr};
  SessionRegistry::Encoder encoder_;
  SessionVerbHandler verbs_;
  SessionConfigHandler sessionCfg_;
  FixSessionHost* fixHost_{nullptr};
  GatewayCounters* counters_{nullptr};
  std::atomic<int64_t> idleTimeoutMs_{30'000};
};

}  // namespace lrvx::venue
