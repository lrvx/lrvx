/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline feed-health test: a socket that stays open while the data stops is
 * the failure mode a disconnect callback cannot catch, and it is the one that
 * keeps a dead book quoted. IExchangeConnector declares StaleDataCallback and
 * emitStaleData for it; nothing calls either, and the core's own staleness
 * check (composite_book_matrix) skips any exchange that never stamped a last
 * update, which Bitget and Hyperliquid never do.
 *
 * Time is injected rather than slept on, so the test is deterministic.
 *
 * needs, per connector (Bybit, Bitget, Hyperliquid, Polymarket):
 *   int <Config>::staleDataTimeoutMs{...};      // 0 disables the check
 *   void pollFeedHealth(MonoNanos now);         // public
 * pollFeedHealth emits emitStaleData(symbol, lastUpdateMs) for every
 * subscribed symbol whose last received update is older than the window, and
 * emits nothing for a symbol whose data is inside it. The connector must
 * record a per-symbol last-update stamp to answer that.
 *
 * needs, additionally:
 *   public void BitgetExchangeConnector::handleMessage(std::string_view);
 *   public void HyperliquidExchangeConnector::handleMessage(std::string_view);
 * -- both are private today, so a frame cannot be fed without a live socket.
 */

#include "lrvx-connectors/bitget/bitget_exchange_connector.h"
#include "lrvx-connectors/bybit/bybit_exchange_connector.h"
#include "lrvx-connectors/hyperliquid/hyperliquid_exchange_connector.h"
#include "lrvx-connectors/polymarket/polymarket_exchange_connector.h"

#include <lrvx/book/bus/book_update_bus.h>
#include <lrvx/book/bus/trade_bus.h>
#include <lrvx/common.h>
#include <lrvx/log/atomic_logger.h>
#include <lrvx/util/base/time.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

constexpr int kWindowMs = 1000;
constexpr uint64_t kNsPerMsU = 1'000'000ULL;

std::shared_ptr<AtomicLogger> makeLogger(const char* basename)
{
  auto dir = std::filesystem::temp_directory_path() / "lrvx_conn_stale_test_logs";
  std::filesystem::create_directories(dir);
  AtomicLoggerOptions opts;
  opts.directory = dir.string();
  opts.basename = basename;
  return std::make_shared<AtomicLogger>(opts);
}

MonoNanos plusMs(MonoNanos base, uint64_t ms)
{
  return MonoNanos::fromRaw(base.raw() + ms * kNsPerMsU);
}

MonoNanos minusMs(MonoNanos base, uint64_t ms)
{
  return MonoNanos::fromRaw(base.raw() - ms * kNsPerMsU);
}

// The window arithmetic lives in IExchangeConnector, shared by all four
// connectors, and its edges are nanoseconds wide: through a connector the
// stamp is taken by the connector itself from the clock, so a test can only
// bracket it. This exposes the two shared entry points directly, which is the
// only way to hand the check a stamp and a poll instant that are an exact
// number of nanoseconds apart. The connector-level cases above keep covering
// that the connectors reach these two through their own paths.
class ProbeConnector : public IExchangeConnector
{
 public:
  std::string exchangeId() const override { return "probe"; }

  using IExchangeConnector::checkStaleFeeds;
  using IExchangeConnector::markFeedActivity;
};

uint64_t toMs(MonoNanos t) { return t.raw() / kNsPerMsU; }

// Loopback port 1: nothing listens there, so start() runs its whole body --
// including the stamping of every subscribed symbol -- while every connection
// attempt is refused instantly. The staleness window is driven by the injected
// clock, not by this socket.
constexpr const char* kUnreachable = "ws://127.0.0.1:1";

class StaleRecorder
{
 public:
  void install(IExchangeConnector& c)
  {
    c.setErrorCallbacks(
        [](std::string_view)
        {
        },
        [](uint64_t, uint64_t)
        {
        },
        [this](SymbolId sym, uint64_t lastUpdateMs)
        {
          std::lock_guard<std::mutex> lk(_m);
          _hits.push_back({sym, lastUpdateMs});
        });
  }

  struct Hit
  {
    SymbolId symbol;
    uint64_t lastUpdateMs;
  };

  std::vector<Hit> hits()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _hits;
  }

  size_t count()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _hits.size();
  }

 private:
  std::mutex _m;
  std::vector<Hit> _hits;
};

struct Buses
{
  BookUpdateBus book;
  TradeBus trade;

  Buses()
  {
    book.start();
    trade.start();
  }

  ~Buses()
  {
    book.flush();
    book.stop();
    trade.stop();
  }
};

std::string bybitBookFrame(int64_t u)
{
  std::string s = R"({"topic":"orderbook.50.BTCUSDT","type":"snapshot","ts":1700000000000,)"
                  R"("cts":1700000000000,"data":{"s":"BTCUSDT","b":[["100","1"]],)"
                  R"("a":[["101","2"]],"u":)";
  s += std::to_string(u);
  s += R"(,"seq":1}})";
  return s;
}

std::string bitgetBookFrame()
{
  return R"({"action":"snapshot","arg":{"instType":"USDT-FUTURES","channel":"books15",)"
         R"("instId":"BTCUSDT"},"data":[{"asks":[["101","2"]],"bids":[["100","1"]],)"
         R"("ts":"1700000000000"}],"ts":1700000000000})";
}

std::string hyperliquidBookFrame()
{
  return R"({"channel":"l2Book","data":{"coin":"BTC","time":1700000000000,)"
         R"("levels":[[{"px":"100","sz":"1","n":1}],[{"px":"101","sz":"2","n":1}]]}})";
}

std::string polymarketBookFrame()
{
  return R"([{"event_type":"book","asset_id":"TOKEN",)"
         R"("bids":[{"price":"0.40","size":"100"}],)"
         R"("asks":[{"price":"0.60","size":"80"}]}])";
}

std::string polymarketPriceChangeFrame()
{
  return R"({"event_type":"price_change","market":"0xabc","price_changes":[)"
         R"({"asset_id":"TOKEN","price":"0.41","size":"50","side":"BUY","hash":"h"}],)"
         R"("timestamp":1})";
}

// A books frame whose parse is slow enough to be measured: the book itself is
// two levels, and the weight is a field the connector never reads, so nothing
// but the cost of parsing the document changes. The arrival stamp and the
// post-parse instant are milliseconds apart on this frame instead of
// microseconds, which is what makes the stamp site observable at all.
std::string bitgetBookFrameWithHeavyTail()
{
  std::string s = R"({"action":"snapshot","arg":{"instType":"USDT-FUTURES","channel":"books15",)"
                  R"("instId":"BTCUSDT"},"data":[{"asks":[["101","2"]],"bids":[["100","1"]],)"
                  R"("ts":"1700000000000"}],"ts":1700000000000,"unread":[)";
  s.reserve(40u * 1024u * 1024u);
  constexpr int kEntries = 4'000'000;
  for (int i = 0; i < kEntries; ++i)
  {
    if (i)
    {
      s += ',';
    }
    s += std::to_string(100000 + (i % 100000));
  }
  s += "]}";
  return s;
}

}  // namespace

TEST(ConnectorHealthStale, BybitReportsAQuietFeed)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  cfg.staleDataTimeoutMs = kWindowMs;

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_stale.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.handleMessage(bybitBookFrame(100));
  const MonoNanos t0 = nowMonoNanos();

  connector.pollFeedHealth(plusMs(t0, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 0u) << "a feed inside its window is not stale";

  connector.pollFeedHealth(plusMs(t0, kWindowMs * 2));
  const auto hits = rec.hits();
  ASSERT_EQ(hits.size(), 1u) << "a feed past its staleness window must reach emitStaleData";
  EXPECT_EQ(hits[0].symbol, connector.resolveSymbolId("BTCUSDT"));

  // Fresh data clears it: the next poll inside the window is silent again.
  connector.handleMessage(bybitBookFrame(101));
  const MonoNanos t1 = nowMonoNanos();
  connector.pollFeedHealth(plusMs(t1, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 1u) << "a recovered feed must not keep reporting stale";
}

TEST(ConnectorHealthStale, BitgetReportsAQuietFeed)
{
  Buses buses;
  SymbolRegistry registry;

  BitgetConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth15}};
  cfg.staleDataTimeoutMs = kWindowMs;

  BitgetExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                    makeLogger("bitget_stale.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.handleMessage(bitgetBookFrame());
  const MonoNanos t0 = nowMonoNanos();

  connector.pollFeedHealth(plusMs(t0, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 0u);

  connector.pollFeedHealth(plusMs(t0, kWindowMs * 2));
  const auto hits = rec.hits();
  ASSERT_EQ(hits.size(), 1u) << "a feed past its staleness window must reach emitStaleData";
  EXPECT_EQ(hits[0].symbol, connector.resolveSymbolId("BTCUSDT"));

  connector.handleMessage(bitgetBookFrame());
  const MonoNanos t1 = nowMonoNanos();
  connector.pollFeedHealth(plusMs(t1, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 1u);
}

TEST(ConnectorHealthStale, HyperliquidReportsAQuietFeed)
{
  Buses buses;
  SymbolRegistry registry;

  HyperliquidConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.wsEndpoint = "wss://unused.invalid";
  cfg.symbols = {"BTC"};
  cfg.staleDataTimeoutMs = kWindowMs;

  HyperliquidExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                         makeLogger("hyperliquid_stale.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.handleMessage(hyperliquidBookFrame());
  const MonoNanos t0 = nowMonoNanos();

  connector.pollFeedHealth(plusMs(t0, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 0u);

  connector.pollFeedHealth(plusMs(t0, kWindowMs * 2));
  const auto hits = rec.hits();
  ASSERT_EQ(hits.size(), 1u) << "a feed past its staleness window must reach emitStaleData";
  EXPECT_EQ(hits[0].symbol, connector.resolveSymbolId("BTC"));

  connector.handleMessage(hyperliquidBookFrame());
  const MonoNanos t1 = nowMonoNanos();
  connector.pollFeedHealth(plusMs(t1, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 1u);
}

TEST(ConnectorHealthStale, PolymarketReportsAQuietFeed)
{
  Buses buses;
  SymbolRegistry registry;

  PolymarketConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.wsEndpoint = "wss://unused.invalid";
  cfg.tokenIds = {"TOKEN"};
  cfg.staleDataTimeoutMs = kWindowMs;

  PolymarketExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                        makeLogger("polymarket_stale.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.handleMessage(polymarketBookFrame());
  const MonoNanos t0 = nowMonoNanos();

  connector.pollFeedHealth(plusMs(t0, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 0u);

  connector.pollFeedHealth(plusMs(t0, kWindowMs * 2));
  const auto hits = rec.hits();
  ASSERT_EQ(hits.size(), 1u) << "a feed past its staleness window must reach emitStaleData";
  EXPECT_EQ(hits[0].symbol, connector.resolveSymbolId("TOKEN"));

  connector.handleMessage(polymarketBookFrame());
  const MonoNanos t1 = nowMonoNanos();
  connector.pollFeedHealth(plusMs(t1, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 1u);
}

// A zero window disables the check. The connector is started first, so every
// subscribed symbol carries a stamp and the map the check walks is populated:
// with a window of 0 taken as "off" this is silent no matter how far in the
// future the poll lands, and with 0 taken as a window every feed in it is
// instantly and permanently stale.
TEST(ConnectorHealthStale, ZeroWindowDisablesTheCheck)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = kUnreachable;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  cfg.staleDataTimeoutMs = 0;

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_stale_off.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.start();
  const MonoNanos started = nowMonoNanos();

  connector.pollFeedHealth(plusMs(started, 60'000));
  EXPECT_EQ(rec.count(), 0u) << "a window of 0 means the check is off, not a window of 0 ms";

  connector.stop();
}

// One event per staleness episode, not one per poll: a supervisor polling at
// 1 Hz wants to hear that a feed died once, not once a second for as long as
// it stays dead.
TEST(ConnectorHealthStale, AQuietFeedIsReportedOncePerEpisode)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = kUnreachable;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  cfg.staleDataTimeoutMs = kWindowMs;

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_stale_once.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.start();
  const MonoNanos started = nowMonoNanos();

  connector.pollFeedHealth(plusMs(started, kWindowMs * 2));
  ASSERT_EQ(rec.count(), 1u) << "the first poll past the window must report";

  connector.pollFeedHealth(plusMs(started, kWindowMs * 3));
  EXPECT_EQ(rec.count(), 1u) << "a feed that is still dead must not be reported again";

  connector.stop();
}

// Fresh data re-arms the report: a feed that died, was reported, came back and
// died again is a second episode and must produce a second event.
TEST(ConnectorHealthStale, FreshDataReArmsTheReport)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = kUnreachable;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  cfg.staleDataTimeoutMs = kWindowMs;

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_stale_rearm.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.start();
  const MonoNanos started = nowMonoNanos();

  connector.pollFeedHealth(plusMs(started, kWindowMs * 2));
  ASSERT_EQ(rec.count(), 1u) << "the first episode must be reported";

  connector.handleMessage(bybitBookFrame(100));
  const MonoNanos recovered = nowMonoNanos();

  connector.pollFeedHealth(plusMs(recovered, kWindowMs / 2));
  EXPECT_EQ(rec.count(), 1u) << "a feed inside its window is not stale";

  connector.pollFeedHealth(plusMs(recovered, kWindowMs * 2));
  EXPECT_EQ(rec.count(), 2u) << "a second staleness episode must be reported again";

  connector.stop();
}

// The window boundary. The stamp is taken inside start(), so it is bracketed:
// a poll one millisecond before the earliest moment it could expire is silent,
// and a poll a full window after the latest moment it could have been taken
// reports. A window quietly stretched (or shrunk) fails one side or the other.
TEST(ConnectorHealthStale, TheConfiguredWindowIsTheBoundary)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = kUnreachable;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  cfg.staleDataTimeoutMs = kWindowMs;

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_stale_boundary.log"));
  StaleRecorder rec;
  rec.install(connector);

  const MonoNanos before = nowMonoNanos();
  connector.start();
  const MonoNanos after = nowMonoNanos();

  connector.pollFeedHealth(minusMs(plusMs(before, kWindowMs), 1));
  EXPECT_EQ(rec.count(), 0u) << "a feed one millisecond short of its window is not stale yet";

  connector.pollFeedHealth(plusMs(after, kWindowMs));
  EXPECT_EQ(rec.count(), 1u) << "a feed a full window past its last update must be reported";

  connector.stop();
}

// The callback's second argument is documented as milliseconds. It is read as
// an age by whoever receives it, so the unit is the whole content of the
// number: it must bracket the monotonic instant the frame was handed in.
TEST(ConnectorHealthStale, TheReportCarriesTheLastUpdateInMilliseconds)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = kUnreachable;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  cfg.staleDataTimeoutMs = kWindowMs;

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_stale_units.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.start();

  const MonoNanos before = nowMonoNanos();
  connector.handleMessage(bybitBookFrame(100));
  const MonoNanos after = nowMonoNanos();

  connector.pollFeedHealth(plusMs(after, kWindowMs * 2));

  const auto hits = rec.hits();
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_GE(hits[0].lastUpdateMs, toMs(before))
      << "the reported stamp is older than the frame that set it";
  EXPECT_LE(hits[0].lastUpdateMs, toMs(after)) << "the reported stamp is not in milliseconds";

  connector.stop();
}

// A venue that loses several feeds at once must report all of them: one poll,
// two dead symbols, two events.
TEST(ConnectorHealthStale, EverySymbolThatWentQuietIsReported)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = kUnreachable;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50},
                 {"ETHUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  cfg.staleDataTimeoutMs = kWindowMs;

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_stale_multi.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.start();
  const MonoNanos started = nowMonoNanos();

  connector.pollFeedHealth(plusMs(started, kWindowMs * 2));

  auto hits = rec.hits();
  ASSERT_EQ(hits.size(), 2u) << "both dead feeds must be reported, not just the first one found";

  std::vector<SymbolId> reported{hits[0].symbol, hits[1].symbol};
  std::sort(reported.begin(), reported.end());
  std::vector<SymbolId> expected{connector.resolveSymbolId("BTCUSDT"),
                                 connector.resolveSymbolId("ETHUSDT")};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(reported, expected);

  connector.stop();
}

// start() stamps every subscribed symbol, so a feed that never delivers a
// single frame still ages out. Without that baseline it has no entry to age
// from and stays silently absent forever, which is the louder of the two
// failures this check exists for.
TEST(ConnectorHealthStale, BybitStartSeedsEverySubscribedSymbol)
{
  Buses buses;
  SymbolRegistry registry;

  BybitConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = kUnreachable;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  cfg.staleDataTimeoutMs = kWindowMs;

  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_stale_seed.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.start();
  const MonoNanos started = nowMonoNanos();

  // Not one frame has been handed in: the only stamp this symbol can have is
  // the one start() wrote.
  connector.pollFeedHealth(plusMs(started, kWindowMs * 2));

  const auto hits = rec.hits();
  ASSERT_EQ(hits.size(), 1u) << "a subscribed feed that never delivered anything must age out";
  EXPECT_EQ(hits[0].symbol, connector.resolveSymbolId("BTCUSDT"));

  connector.stop();
}

// Bitget stamps the frame when it arrives, not when it is done being parsed.
// The two are indistinguishable on a small frame, so the fixture here carries
// a payload whose parse is measurable: the recorded stamp must sit at the
// arrival end of that interval. A stamp taken after the parse makes every feed
// look fresher than it is by the cost of the parse -- which is exactly the
// cost that grows when a venue floods.
TEST(ConnectorHealthStale, BitgetStampsTheArrivalTimeNotThePostParseTime)
{
  Buses buses;
  SymbolRegistry registry;

  BitgetConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.publicEndpoint = kUnreachable;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth15}};
  cfg.staleDataTimeoutMs = kWindowMs;

  BitgetExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                    makeLogger("bitget_stale_stamp.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.start();

  const std::string frame = bitgetBookFrameWithHeavyTail();
  const MonoNanos before = nowMonoNanos();
  connector.handleMessage(frame);
  const MonoNanos after = nowMonoNanos();

  const uint64_t spanMs = toMs(after) - toMs(before);
  if (spanMs < 8u)
  {
    // A host that parses the heavy frame in under 8 ms cannot tell the two
    // stamp sites apart by time alone; that is a limit of the probe, not a
    // defect in the connector, so the case steps aside rather than failing.
    GTEST_SKIP() << "the fixture parses too fast to tell the two stamp sites apart (span " << spanMs
                 << " ms)";
  }

  connector.pollFeedHealth(plusMs(after, kWindowMs * 2));

  const auto hits = rec.hits();
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_GE(hits[0].lastUpdateMs, toMs(before)) << "the frame did not stamp the feed at all";
  EXPECT_LE(hits[0].lastUpdateMs, toMs(before) + spanMs / 4)
      << "the stamp was taken after the parse, not on arrival";

  connector.stop();
}

// A Polymarket token whose book frame arrived once and which then only ever
// receives price_change deltas must keep counting as alive: the delta path
// stamps activity too, or the token is reported stale forever while its data
// is arriving normally.
TEST(ConnectorHealthStale, PolymarketPriceChangesStampActivity)
{
  Buses buses;
  SymbolRegistry registry;

  PolymarketConfig cfg;
  cfg.instrumentInfoUrl.clear();  // offline: start() reads no instrument info
  cfg.wsEndpoint = kUnreachable;
  cfg.tokenIds = {"TOKEN"};
  cfg.staleDataTimeoutMs = kWindowMs;

  PolymarketExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                        makeLogger("polymarket_stale_delta.log"));
  StaleRecorder rec;
  rec.install(connector);

  connector.start();
  const MonoNanos started = nowMonoNanos();

  connector.pollFeedHealth(plusMs(started, kWindowMs * 2));
  ASSERT_EQ(rec.count(), 1u) << "the token has to go stale first for the recovery to mean anything";

  connector.handleMessage(polymarketPriceChangeFrame());
  const MonoNanos recovered = nowMonoNanos();

  connector.pollFeedHealth(plusMs(recovered, kWindowMs * 2));
  EXPECT_EQ(rec.count(), 2u)
      << "a token that only receives price_change deltas must be stamped by them";

  connector.stop();
}

// The window is the silence a desk is willing to tolerate, so a feed that has
// been quiet for exactly that long has reached it and is reported; the last
// nanosecond inside it is still silent. An off-by-one here is not visible from
// a test that only measures halves and doubles of the window, and it moves the
// report by a whole poll interval in one direction or the other.
TEST(ConnectorHealthStale, TheWindowBoundaryIsExactToTheNanosecond)
{
  constexpr uint64_t kStamp = 1'000'000'000'000ULL;
  constexpr uint64_t kWindowNs = static_cast<uint64_t>(kWindowMs) * kNsPerMsU;
  const SymbolId sym = 7;

  {
    ProbeConnector probe;
    StaleRecorder rec;
    rec.install(probe);

    probe.markFeedActivity(sym, MonoNanos::fromRaw(kStamp));
    probe.checkStaleFeeds(MonoNanos::fromRaw(kStamp + kWindowNs - 1), kWindowMs);
    EXPECT_EQ(rec.count(), 0u) << "one nanosecond short of the window the feed is not stale yet";
  }

  {
    ProbeConnector probe;
    StaleRecorder rec;
    rec.install(probe);

    probe.markFeedActivity(sym, MonoNanos::fromRaw(kStamp));
    probe.checkStaleFeeds(MonoNanos::fromRaw(kStamp + kWindowNs), kWindowMs);
    EXPECT_EQ(rec.count(), 1u) << "a feed quiet for exactly the window has reached it";
  }
}

// A poll instant older than the stamp it is compared against is not an
// impossible state: the stamp is taken on the market-data thread and the poll
// instant on the supervisor's, and the two read the clock independently, so a
// frame handled after the poll read its own `now` leaves exactly this. The age
// is unsigned, so subtracting the larger value does not produce a negative
// age -- it produces an enormous one, which reports every live feed on the
// venue as dead.
TEST(ConnectorHealthStale, APollOlderThanTheStampReportsNothing)
{
  constexpr uint64_t kStamp = 1'000'000'000'000ULL;
  const SymbolId sym = 7;

  ProbeConnector probe;
  StaleRecorder rec;
  rec.install(probe);

  probe.markFeedActivity(sym, MonoNanos::fromRaw(kStamp));

  probe.checkStaleFeeds(MonoNanos::fromRaw(kStamp - 1), kWindowMs);
  EXPECT_EQ(rec.count(), 0u) << "a poll one nanosecond before the stamp is not a dead feed";

  probe.checkStaleFeeds(MonoNanos::fromRaw(kStamp - 5 * kNsPerMsU), kWindowMs);
  EXPECT_EQ(rec.count(), 0u) << "the age is unsigned: a poll behind the stamp must not wrap";
}

// Fresh data re-arms the report because data arrived, not because the clock
// advanced. Two frames can carry the same stamp -- every connector stamps a
// whole batch of symbols with one reading, and a clock whose resolution is
// coarser than the gap between two frames repeats a value -- so making the
// re-arm conditional on a strictly later stamp silently drops the second
// episode of any feed that recovers inside one tick.
TEST(ConnectorHealthStale, DataReArmsTheReportEvenWithoutANewerStamp)
{
  constexpr uint64_t kStamp = 1'000'000'000'000ULL;
  constexpr uint64_t kWindowNs = static_cast<uint64_t>(kWindowMs) * kNsPerMsU;
  const SymbolId sym = 7;

  ProbeConnector probe;
  StaleRecorder rec;
  rec.install(probe);

  probe.markFeedActivity(sym, MonoNanos::fromRaw(kStamp));
  probe.checkStaleFeeds(MonoNanos::fromRaw(kStamp + 2 * kWindowNs), kWindowMs);
  ASSERT_EQ(rec.count(), 1u) << "the first episode must be reported";

  // Same stamp as the frame before it: the feed is alive again, and the check
  // has to be armed for the next time it stops.
  probe.markFeedActivity(sym, MonoNanos::fromRaw(kStamp));
  probe.checkStaleFeeds(MonoNanos::fromRaw(kStamp + 4 * kWindowNs), kWindowMs);
  EXPECT_EQ(rec.count(), 2u) << "data re-arms the report whatever stamp it carries";
}
