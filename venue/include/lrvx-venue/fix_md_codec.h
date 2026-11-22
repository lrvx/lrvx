/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * FIX 4.4 market data: MarketDataRequest (35=V) in,
 * MarketDataSnapshotFullRefresh (35=W), MarketDataIncrementalRefresh (35=X)
 * and MarketDataRequestReject (35=Y) out -- the encoding that lets an
 * integration which already speaks FIX consume the feed without implementing
 * the binary one.
 *
 * Why its own header rather than more of fix_codec.h: market data is a
 * different message family with a different shape. Order entry is flat
 * tag=value, and FixCodec::parseFields deliberately collapses repeats into a
 * map (last occurrence wins). Market data is built on REPEATING GROUPS (146
 * NoRelatedSym, 267 NoMDEntryTypes, 268 NoMDEntries), where the repeats are
 * the payload, so it needs an order-preserving parse. The framing, the
 * checksum, the exact decimal printing and the UTCTimestamp all come from
 * fix_codec.h unchanged -- prices and sizes go through decwire exactly as
 * order entry prints them, never through a double.
 *
 * Session layer: deliberately a light one, NOT FixSessionHost/FixConnection.
 * That session layer is built around order entry -- accounts, the
 * SessionRegistry exec-report log, application-level resend with PossDup --
 * and none of it has a meaning for a broadcast feed: market data is not
 * account-scoped, and a market-data consumer recovers from a hole with the
 * feed's own tools (a fresh MarketDataRequest -> a new full refresh, or a
 * seq-based resend on the binary encoding), not by replaying a per-account
 * message log. So this is Logon / Heartbeat / TestRequest / Logout with a
 * monotonic outbound MsgSeqNum, and nothing else.
 */
#pragma once

#include "lrvx-venue/decimal_wire.h"
#include "lrvx-venue/fix_codec.h"
#include "lrvx-venue/md_encoder.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lrvx::venue
{

class FixMdCodec
{
 public:
  static constexpr char SOH = FixCodec::SOH;

  // MDUpdateAction (279) on an incremental refresh.
  static constexpr char kNew = '0';
  static constexpr char kChange = '1';
  static constexpr char kDelete = '2';

  // MDEntryType (269). '6' SettlementPrice carries the derivatives mark and
  // 'C' OpenInterest the open interest -- both standard FIX 4.4 entry types, so
  // an off-the-shelf consumer reads them without a private extension.
  static constexpr char kBid = '0';
  static constexpr char kOffer = '1';
  static constexpr char kTrade = '2';
  static constexpr char kSettlement = '6';
  static constexpr char kOpenInterest = 'C';

  // SecurityTradingStatus (326) values used by 35=f SecurityStatus.
  static constexpr const char* kStatusHalted = "2";    // Trading Halt
  static constexpr const char* kStatusTrading = "17";  // Ready to Trade
  // 18 "Not available for trading" is FIX 4.4's end-of-session value, so a
  // closed session maps onto a standard code that means exactly that -- not
  // onto the halt code, which would erase the distinction the state exists for.
  static constexpr const char* kStatusClosed = "18";      // Not Available For Trading
  static constexpr const char* kStatusPreOpen = "21";     // Pre-open
  static constexpr const char* kStatusUncrossing = "22";  // Opening Rotation

  // FIX 4.4 has no field for a perpetual funding rate or a funding calendar:
  // the protocol predates the instrument. They travel in the user-defined tag
  // range (5000-9999), documented in docs/venue/market-data.md, rather than as
  // an abuse of a standard tag that means something else.
  static constexpr int kTagFundingRate = 5001;
  static constexpr int kTagNextFundingTime = 5002;
  static constexpr int kTagHaltUntilTime = 5003;

  // Length of the complete FIX message at the head of [p, p+n).
  // 1 = complete (`len` set), 0 = need more bytes, -1 = malformed.
  static int messageLength(const uint8_t* p, size_t n, size_t& len)
  {
    static constexpr char kBegin[] = "8=FIX";
    const size_t probe = n < sizeof(kBegin) - 1 ? n : sizeof(kBegin) - 1;
    if (std::memcmp(p, kBegin, probe) != 0)
    {
      return -1;  // not FIX at all: never wait for bytes that cannot help
    }
    if (n < sizeof(kBegin) - 1)
    {
      return 0;
    }
    const uint8_t* soh1 = static_cast<const uint8_t*>(std::memchr(p, SOH, n));
    if (soh1 == nullptr)
    {
      return n > kMaxHeader ? -1 : 0;
    }
    const size_t lenTag = static_cast<size_t>(soh1 - p) + 1;  // start of "9="
    if (n < lenTag + 2)
    {
      return 0;
    }
    if (p[lenTag] != '9' || p[lenTag + 1] != '=')
    {
      return -1;  // BodyLength must be the second field
    }
    const uint8_t* soh2 =
        static_cast<const uint8_t*>(std::memchr(p + lenTag, SOH, n - lenTag));
    if (soh2 == nullptr)
    {
      return n > kMaxHeader ? -1 : 0;
    }
    uint64_t body = 0;
    size_t digits = 0;
    for (const uint8_t* q = p + lenTag + 2; q < soh2; ++q)
    {
      if (*q < '0' || *q > '9' || ++digits > 9)
      {
        return -1;
      }
      body = body * 10 + static_cast<uint64_t>(*q - '0');
    }
    if (digits == 0)
    {
      return -1;
    }
    // body counts the bytes between the SOH after BodyLength and the SOH
    // before the CheckSum field; the trailer is a fixed "10=nnn" + SOH.
    const size_t total = static_cast<size_t>(soh2 - p) + 1 + static_cast<size_t>(body) + 7;
    if (total > kMaxMessage)
    {
      return -1;
    }
    if (n < total)
    {
      return 0;
    }
    len = total;
    return 1;
  }

  // Order-preserving tag/value list: repeating groups ARE the payload of a
  // market-data message, so nothing may be collapsed.
  static std::vector<std::pair<int, std::string>> parseOrdered(std::string_view msg)
  {
    std::vector<std::pair<int, std::string>> f;
    size_t i = 0;
    while (i < msg.size())
    {
      const size_t eq = msg.find('=', i);
      if (eq == std::string_view::npos)
      {
        break;
      }
      size_t soh = msg.find(SOH, eq + 1);
      if (soh == std::string_view::npos)
      {
        soh = msg.size();
      }
      f.emplace_back(std::atoi(std::string(msg.substr(i, eq - i)).c_str()),
                     std::string(msg.substr(eq + 1, soh - eq - 1)));
      i = soh + 1;
    }
    return f;
  }

  // First value of `tag`, empty when absent.
  static std::string first(const std::vector<std::pair<int, std::string>>& f, int tag)
  {
    for (const auto& [t, v] : f)
    {
      if (t == tag)
      {
        return v;
      }
    }
    return {};
  }

  static bool hasTag(const std::vector<std::pair<int, std::string>>& f, int tag)
  {
    for (const auto& [t, v] : f)
    {
      (void)v;
      if (t == tag)
      {
        return true;
      }
    }
    return false;
  }

  static void add(std::string& b, int tag, std::string_view val)
  {
    b += std::to_string(tag);
    b += '=';
    b.append(val.data(), val.size());
    b += SOH;
  }

  static void addPrice(std::string& b, int tag, Price p)
  {
    b += std::to_string(tag);
    b += '=';
    decwire::append(b, p.raw());
    b += SOH;
  }

  static void addQty(std::string& b, int tag, Quantity q)
  {
    b += std::to_string(tag);
    b += '=';
    decwire::append(b, q.raw());
    b += SOH;
  }

  // UTCTimestamp for tag 52, from the wall clock.
  static std::string nowStamp()
  {
    return FixSession::sendingTime(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
  }

  // UTCTimestamp for a nanosecond timestamp carried by a feed message. 0 (an
  // unstamped message from an older publisher) prints nothing -- the tag is
  // omitted rather than filled with the epoch.
  static void addTime(std::string& b, int tag, int64_t tsNs)
  {
    if (tsNs != 0)
    {
      add(b, tag, FixSession::sendingTime(tsNs));
    }
  }

 private:
  static constexpr size_t kMaxHeader = 64;          // 8=...|9=...| can never be longer
  static constexpr size_t kMaxMessage = 64u << 20;  // bound a hostile BodyLength
};

struct FixMdConfig
{
  std::string senderCompId{"VENUE"};
  std::string targetCompId{"CLIENT"};
  uint32_t defaultHeartBtIntSec{30};
};

// FIX 4.4 market-data encoding for the unicast distribution server. One
// instance per connection; the session serializes every call.
class FixMdEncoder final : public MdEncoder
{
 public:
  // Lead byte of a FIX stream: '8', the first byte of BeginString "8=FIX.4.4".
  static constexpr uint8_t kLeadByte = static_cast<uint8_t>('8');

  explicit FixMdEncoder(FixMdConfig cfg = {}) : cfg_(std::move(cfg)) {}

  const char* name() const noexcept override { return "FIX"; }

  Parse parse(const uint8_t* in, size_t n, size_t& consumed, std::vector<MdRequest>& out,
              std::vector<uint8_t>& reply) override
  {
    size_t total = 0;
    const int st = FixMdCodec::messageLength(in, n, total);
    if (st == 0)
    {
      return Parse::Need;
    }
    if (st < 0)
    {
      return Parse::Bad;
    }
    consumed = total;
    const std::string msg(reinterpret_cast<const char*>(in), total);
    if (!FixCodec::checksumValid(msg))
    {
      return Parse::Bad;
    }
    const auto f = FixMdCodec::parseOrdered(msg);
    const std::string type = FixMdCodec::first(f, 35);
    if (type.empty())
    {
      return Parse::Bad;
    }
    const std::string inSeq = FixMdCodec::first(f, 34);

    if (type == "A")  // Logon
    {
      const std::string hb = FixMdCodec::first(f, 108);
      heartBtInt_ = hb.empty() ? cfg_.defaultHeartBtIntSec
                               : static_cast<uint32_t>(std::strtoul(hb.c_str(), nullptr, 10));
      loggedOn_ = true;
      std::string b = header("A");
      FixMdCodec::add(b, 98, "0");  // EncryptMethod: none, the transport does that
      FixMdCodec::add(b, 108, std::to_string(heartBtInt_));
      emit(reply, b);
      return Parse::Ok;
    }
    if (!loggedOn_)
    {
      // A feed session starts with a Logon, like any other FIX session.
      std::string b = header("5");
      FixMdCodec::add(b, 58, "Logon required");
      emit(reply, b);
      out.push_back(MdRequest{MdRequestKind::Close, 0, 0, false});
      return Parse::Ok;
    }
    if (type == "0")  // Heartbeat
    {
      return Parse::Ok;
    }
    if (type == "1")  // TestRequest
    {
      std::string b = header("0");
      const std::string id = FixMdCodec::first(f, 112);
      if (!id.empty())
      {
        FixMdCodec::add(b, 112, id);
      }
      emit(reply, b);
      return Parse::Ok;
    }
    if (type == "5")  // Logout
    {
      std::string b = header("5");
      emit(reply, b);
      out.push_back(MdRequest{MdRequestKind::Close, 0, 0, false});
      return Parse::Ok;
    }
    if (type == "V")  // MarketDataRequest
    {
      marketDataRequest(f, out, reply);
      return Parse::Ok;
    }
    // Anything else gets an honest session Reject rather than silence.
    std::string b = header("3");
    FixMdCodec::add(b, 45, inSeq.empty() ? "0" : inSeq);
    FixMdCodec::add(b, 372, type);
    FixMdCodec::add(b, 58, "Unsupported MsgType on the market-data session");
    emit(reply, b);
    return Parse::Ok;
  }

  // 35=W MarketDataSnapshotFullRefresh: the whole displayed book as MDEntries.
  // The feed is order-level, so each entry carries its MDEntryID (278) and its
  // MDEntryTime (273) -- the engine time the order was accepted at.
  //
  // The snapshot is preceded by the instrument's current state: a 35=f
  // SecurityStatus and, when the venue publishes one, the derivatives entries.
  // A FIX consumer joining a halted instrument is told it is halted instead of
  // reading an idle book.
  void snapshot(SymbolId symbol, const MdSnapshot& snap, std::vector<uint8_t>& out) override
  {
    const Sub* s = find(symbol);
    if (s == nullptr)
    {
      return;
    }
    if (snap.hasStatus)
    {
      securityStatus(snap.status, out);
    }
    if (snap.hasDerivatives)
    {
      derivatives(snap.derivatives, *s, out);
    }
    size_t entries = 0;
    for (const MdMessage& m : snap.orders)
    {
      if (m.symbol == symbol && wants(*s, entryType(m)))
      {
        ++entries;
      }
    }
    std::string b = header("W", snap.hasStatus ? snap.status.sendTsNs : 0);
    FixMdCodec::add(b, 262, s->reqId);
    FixMdCodec::add(b, 55, std::to_string(symbol));
    FixMdCodec::add(b, 268, std::to_string(entries));
    for (const MdMessage& m : snap.orders)
    {
      if (m.symbol != symbol)
      {
        continue;
      }
      const char et = entryType(m);
      if (!wants(*s, et))
      {
        continue;
      }
      FixMdCodec::add(b, 269, std::string(1, et));
      FixMdCodec::addPrice(b, 270, m.price);
      FixMdCodec::addQty(b, 271, m.qty);
      FixMdCodec::addTime(b, 273, m.engineTsNs);
      FixMdCodec::add(b, 278, std::to_string(m.id));
    }
    emit(out, b);
  }

  // 35=X MarketDataIncrementalRefresh, one MDEntry per message -- except the
  // trading status, which has its own standard message (35=f SecurityStatus).
  void increment(const MdMessage& m, std::vector<uint8_t>& out) override
  {
    const Sub* s = find(m.symbol);
    if (s == nullptr || m.type == MdType::Triggered)
    {
      return;  // a stop firing is not a book or trade event on the FIX feed
    }
    if (m.type == MdType::TradingStatus)
    {
      securityStatus(m, out);
      return;
    }
    if (m.type == MdType::DerivativesUpdate)
    {
      derivatives(m, *s, out);
      return;
    }
    const char et = entryType(m);
    if (!wants(*s, et))
    {
      return;
    }
    std::string b = header("X", m.sendTsNs);
    FixMdCodec::add(b, 262, s->reqId);
    // TransactTime: when the ENGINE produced this event, not when the message
    // was framed. The two differ by the venue's own outbound latency, which is
    // exactly what a consumer measuring it needs.
    FixMdCodec::addTime(b, 60, m.engineTsNs);
    FixMdCodec::add(b, 268, "1");
    FixMdCodec::add(b, 279, std::string(1, updateAction(m)));
    FixMdCodec::add(b, 269, std::string(1, et));
    FixMdCodec::add(b, 55, std::to_string(m.symbol));
    FixMdCodec::addPrice(b, 270, m.price);
    FixMdCodec::addQty(b, 271, m.qty);
    FixMdCodec::addTime(b, 273, m.engineTsNs);
    FixMdCodec::add(b, 278, std::to_string(m.id));
    emit(out, b);
  }

  // FIX has no "your resend is too old" message: the full refresh that follows
  // IS the answer, and it replaces the client's book wholesale.
  void snapshotRequired(SymbolId symbol, uint64_t epoch, uint64_t lastSeq,
                        std::vector<uint8_t>& out) override
  {
    (void)symbol;
    (void)epoch;
    (void)lastSeq;
    (void)out;
  }

  // 35=Y MarketDataRequestReject.
  void reject(SymbolId symbol, MdRejectReason reason, std::vector<uint8_t>& out) override
  {
    std::string reqId = lastReqId_;
    if (const Sub* s = find(symbol); s != nullptr)
    {
      reqId = s->reqId;
    }
    subs_.erase(symbol);  // a rejected request leaves no subscription behind
    std::string b = header("Y");
    FixMdCodec::add(b, 262, reqId);
    FixMdCodec::add(b, 281, reason == MdRejectReason::UnknownSymbol ? "0" : "4");
    FixMdCodec::add(b, 58, reason == MdRejectReason::UnknownSymbol ? "Unknown symbol" : "Unsupported request");
    emit(out, b);
  }

  void heartbeat(std::vector<uint8_t>& out) override
  {
    if (!loggedOn_)
    {
      return;  // nothing is sequenced before the Logon
    }
    std::string b = header("0");
    emit(out, b);
  }

  uint64_t outboundSeq() const noexcept { return outSeq_; }

 private:
  struct Sub
  {
    std::string reqId;
    bool bid{true};
    bool offer{true};
    bool trade{true};
  };

  const Sub* find(SymbolId s) const
  {
    const auto it = subs_.find(s);
    return it == subs_.end() ? nullptr : &it->second;
  }

  static bool wants(const Sub& s, char entry)
  {
    return entry == FixMdCodec::kBid ? s.bid : (entry == FixMdCodec::kOffer ? s.offer : s.trade);
  }

  static char entryType(const MdMessage& m)
  {
    if (m.type == MdType::Trade)
    {
      return FixMdCodec::kTrade;
    }
    return m.side == Side::BUY ? FixMdCodec::kBid : FixMdCodec::kOffer;
  }

  static char updateAction(const MdMessage& m)
  {
    switch (m.type)
    {
      case MdType::AddOrder:
      case MdType::Trade:
        return FixMdCodec::kNew;
      case MdType::Cancel:
        return FixMdCodec::kDelete;
      case MdType::Executed:
        // A fully consumed order leaves the book; a partial fill resizes it.
        return m.qty.raw() == 0 ? FixMdCodec::kDelete : FixMdCodec::kChange;
      case MdType::Replace:
      case MdType::Triggered:
      case MdType::TradingStatus:      // own message (35=f), never an MDEntry
      case MdType::DerivativesUpdate:  // built by derivatives(), not here
        return FixMdCodec::kChange;
    }
    return FixMdCodec::kChange;
  }

  void marketDataRequest(const std::vector<std::pair<int, std::string>>& f,
                         std::vector<MdRequest>& out, std::vector<uint8_t>& reply)
  {
    const std::string reqId = FixMdCodec::first(f, 262);
    if (reqId.empty())
    {
      std::string b = header("3");
      FixMdCodec::add(b, 45, FixMdCodec::first(f, 34));
      FixMdCodec::add(b, 372, "V");
      FixMdCodec::add(b, 58, "MDReqID (262) required");
      emit(reply, b);
      return;
    }
    lastReqId_ = reqId;

    const std::string subType = FixMdCodec::first(f, 263);
    if (subType != "0" && subType != "1" && subType != "2")
    {
      emitReject(reply, reqId, "4", "Unsupported SubscriptionRequestType");
      return;
    }
    // Full displayed depth is what the feed carries; a partial book is not
    // something it can honestly serve, so ask for 0 (or leave 264 out).
    const std::string depth = FixMdCodec::first(f, 264);
    if (FixMdCodec::hasTag(f, 264) && depth != "0")
    {
      emitReject(reply, reqId, "5", "Unsupported MarketDepth: full book only");
      return;
    }

    Sub want;
    bool anyEntryType = false;
    want.bid = want.offer = want.trade = false;
    for (const auto& [tag, val] : f)
    {
      if (tag != 269)
      {
        continue;
      }
      anyEntryType = true;
      if (val == std::string(1, FixMdCodec::kBid))
      {
        want.bid = true;
      }
      else if (val == std::string(1, FixMdCodec::kOffer))
      {
        want.offer = true;
      }
      else if (val == std::string(1, FixMdCodec::kTrade))
      {
        want.trade = true;
      }
      else
      {
        emitReject(reply, reqId, "8", "Unsupported MDEntryType");
        return;
      }
    }
    if (!anyEntryType)
    {
      want.bid = want.offer = want.trade = true;  // 267 omitted: everything
    }
    want.reqId = reqId;

    std::vector<SymbolId> symbols;
    for (const auto& [tag, val] : f)
    {
      if (tag == 55)
      {
        symbols.push_back(static_cast<SymbolId>(std::strtoul(val.c_str(), nullptr, 10)));
      }
    }
    if (symbols.empty())
    {
      emitReject(reply, reqId, "0", "No symbol in the request");
      return;
    }

    for (SymbolId s : symbols)
    {
      if (subType == "2")
      {
        subs_.erase(s);
        out.push_back(MdRequest{MdRequestKind::Unsubscribe, s, 0, false});
        continue;
      }
      subs_[s] = want;
      // FIX market data has no seq-based resend: a re-request is answered with
      // a fresh full refresh, so every subscribe starts from the snapshot.
      out.push_back(MdRequest{MdRequestKind::Subscribe, s, 0, subType == "0"});
    }
  }

  void emitReject(std::vector<uint8_t>& out, const std::string& reqId, const char* reason,
                  const char* text)
  {
    std::string b = header("Y");
    FixMdCodec::add(b, 262, reqId);
    FixMdCodec::add(b, 281, reason);
    FixMdCodec::add(b, 58, text);
    emit(out, b);
  }

  // sendTsNs > 0 puts the PUBLISHER's send time into SendingTime (52) instead
  // of a second clock read here: 52 then means exactly what the binary
  // encoding's sendTs means, and the two encodings of one message agree.
  // Session-level messages (logon, heartbeat, reject) have no feed timestamp
  // and keep the wall-clock read.
  std::string header(const char* msgType, int64_t sendTsNs = 0)
  {
    std::string b;
    FixMdCodec::add(b, 35, msgType);
    FixMdCodec::add(b, 34, std::to_string(outSeq_++));
    FixMdCodec::add(b, 49, cfg_.senderCompId);
    FixMdCodec::add(b, 56, cfg_.targetCompId);
    FixMdCodec::add(b, 52,
                    sendTsNs != 0 ? FixSession::sendingTime(sendTsNs) : FixMdCodec::nowStamp());
    return b;
  }

  // 35=f SecurityStatus: the standard FIX message for a halt, a resume and an
  // auction phase. Not gated on a subscription's MDEntryType filter -- a
  // subscriber that asked for trades only still has to know the instrument
  // stopped trading.
  void securityStatus(const MdMessage& m, std::vector<uint8_t>& out)
  {
    const Sub* s = find(m.symbol);
    std::string b = header("f", m.sendTsNs);
    if (s != nullptr)
    {
      FixMdCodec::add(b, 324, s->reqId);  // SecurityStatusReqID: the request this answers
    }
    FixMdCodec::add(b, 55, std::to_string(m.symbol));
    FixMdCodec::add(b, 326, statusValue(m.status));
    FixMdCodec::addTime(b, 60, m.engineTsNs);
    FixMdCodec::addTime(b, FixMdCodec::kTagHaltUntilTime, m.untilNs);
    FixMdCodec::add(b, 58, statusText(m.status, m.reason));
    emit(out, b);
  }

  // 35=X carrying the derivatives layer: SettlementPrice ('6') = the mark,
  // OpenInterest ('C') = the open interest, plus the funding rate and the next
  // funding time in the user-defined tag range.
  void derivatives(const MdMessage& m, const Sub& s, std::vector<uint8_t>& out)
  {
    std::string b = header("X", m.sendTsNs);
    FixMdCodec::add(b, 262, s.reqId);
    FixMdCodec::addTime(b, 60, m.engineTsNs);
    FixMdCodec::add(b, 268, "2");
    FixMdCodec::add(b, 279, std::string(1, FixMdCodec::kChange));
    FixMdCodec::add(b, 269, std::string(1, FixMdCodec::kSettlement));
    FixMdCodec::add(b, 55, std::to_string(m.symbol));
    FixMdCodec::addPrice(b, 270, m.price);
    FixMdCodec::addTime(b, 273, m.engineTsNs);
    FixMdCodec::add(b, 279, std::string(1, FixMdCodec::kChange));
    FixMdCodec::add(b, 269, std::string(1, FixMdCodec::kOpenInterest));
    FixMdCodec::add(b, 55, std::to_string(m.symbol));
    FixMdCodec::addQty(b, 271, m.qty);
    FixMdCodec::addTime(b, 273, m.engineTsNs);
    // The rate prints through the same exact-decimal path prices use: it is a
    // raw fixed-point value at the shared scale, never a double.
    std::string rate;
    decwire::append(rate, m.fundingRateRaw);
    FixMdCodec::add(b, FixMdCodec::kTagFundingRate, rate);
    FixMdCodec::addTime(b, FixMdCodec::kTagNextFundingTime, m.nextFundingNs);
    emit(out, b);
  }

  static const char* statusValue(lrvx::venue::TradingStatus s)
  {
    switch (s)
    {
      case lrvx::venue::TradingStatus::Trading:
        return FixMdCodec::kStatusTrading;
      case lrvx::venue::TradingStatus::Halted:
      case lrvx::venue::TradingStatus::LuldPause:
        return FixMdCodec::kStatusHalted;
      case lrvx::venue::TradingStatus::AuctionPreOpen:
        return FixMdCodec::kStatusPreOpen;
      case lrvx::venue::TradingStatus::AuctionUncross:
        return FixMdCodec::kStatusUncrossing;
      case lrvx::venue::TradingStatus::Closed:
      // 326 has no value for a delisting. "Not Available For Trading" is the
      // one that says what a subscriber must act on; the reason goes into
      // Text (58), as it does for a volatility pause.
      case lrvx::venue::TradingStatus::Delisted:
        return FixMdCodec::kStatusClosed;
    }
    // A status this encoder does not know is never published as tradeable
    // (see the note on TradingStatus in messages.h).
    return FixMdCodec::kStatusClosed;
  }

  // FIX 4.4's HaltReason (327) has no value for a volatility pause, so the
  // distinction that 326 cannot carry goes into Text (58) rather than into an
  // invented enumeration value.
  static const char* statusText(lrvx::venue::TradingStatus s, TradingStatusReason r)
  {
    if (s == lrvx::venue::TradingStatus::LuldPause)
    {
      return "LULD volatility pause";
    }
    if (r == TradingStatusReason::LuldPauseElapsed)
    {
      return "LULD pause elapsed";
    }
    switch (s)
    {
      case lrvx::venue::TradingStatus::Trading:
        return "Trading";
      case lrvx::venue::TradingStatus::Halted:
        return "Trading halt";
      case lrvx::venue::TradingStatus::AuctionPreOpen:
        return "Pre-open auction";
      case lrvx::venue::TradingStatus::AuctionUncross:
        return "Auction uncross";
      case lrvx::venue::TradingStatus::Closed:
        return "Session closed";
      case lrvx::venue::TradingStatus::Delisted:
        return "Delisted";
      case lrvx::venue::TradingStatus::LuldPause:
        break;
    }
    return "Not available for trading";
  }

  static void emit(std::vector<uint8_t>& out, const std::string& body)
  {
    const std::string msg = FixCodec::frame(body);
    out.insert(out.end(), msg.begin(), msg.end());
  }

  FixMdConfig cfg_;
  std::unordered_map<SymbolId, Sub> subs_;
  std::string lastReqId_;
  uint64_t outSeq_{1};
  uint32_t heartBtInt_{30};
  bool loggedOn_{false};
};

}  // namespace lrvx::venue
