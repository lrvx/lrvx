#include "lrvx/capi/lrvx_capi.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/writers/binary_log_writer.h"

#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>
#include <vector>

// ============================================================
// Order book
// ============================================================

TEST(CapiBookTest, CreateAndQuery)
{
  LrvxBookHandle book = lrvx_book_create(0.01);
  ASSERT_NE(book, nullptr);

  double p = 0;
  EXPECT_EQ(lrvx_book_best_bid(book, &p), 0);
  EXPECT_EQ(lrvx_book_best_ask(book, &p), 0);

  double bids_p[] = {100.0, 99.99};
  double bids_q[] = {1.5, 2.0};
  double asks_p[] = {100.01, 100.02};
  double asks_q[] = {0.5, 1.0};
  lrvx_book_apply_snapshot(book, bids_p, bids_q, 2, asks_p, asks_q, 2);

  EXPECT_EQ(lrvx_book_best_bid(book, &p), 1);
  EXPECT_NEAR(p, 100.0, 0.001);

  EXPECT_EQ(lrvx_book_best_ask(book, &p), 1);
  EXPECT_NEAR(p, 100.01, 0.001);

  double mid = 0;
  EXPECT_EQ(lrvx_book_mid(book, &mid), 1);
  EXPECT_NEAR(mid, 100.005, 0.001);

  double spread = 0;
  EXPECT_EQ(lrvx_book_spread(book, &spread), 1);
  EXPECT_NEAR(spread, 0.01, 0.001);

  EXPECT_EQ(lrvx_book_is_crossed(book), 0);

  double prices[10], qtys[10];
  uint32_t n = lrvx_book_get_bids(book, prices, qtys, 10);
  EXPECT_GE(n, 1u);
  EXPECT_NEAR(prices[0], 100.0, 0.001);

  lrvx_book_clear(book);
  EXPECT_EQ(lrvx_book_best_bid(book, &p), 0);

  lrvx_book_destroy(book);
}

// ============================================================
// Executor
// ============================================================

TEST(CapiExecutorTest, SubmitAndFill)
{
  LrvxSimulatedExecutorHandle exec = lrvx_simulated_executor_create();
  ASSERT_NE(exec, nullptr);

  lrvx_simulated_executor_submit_order(exec, 1, 0, 100.0, 1.0, 0, 1);
  lrvx_simulated_executor_on_bar(exec, 1, 100.0);

  EXPECT_GE(lrvx_simulated_executor_fill_count(exec), 0u);

  lrvx_simulated_executor_destroy(exec);
}

// ============================================================
// Position tracker
// ============================================================

TEST(CapiPositionTest, TrackFills)
{
  LrvxPositionTrackerHandle tracker = lrvx_position_tracker_create(0);
  ASSERT_NE(tracker, nullptr);

  lrvx_position_tracker_on_fill(tracker, 1, 0, 100.0, 1.0);
  EXPECT_NEAR(lrvx_position_tracker_position(tracker, 1), 1.0, 0.001);
  EXPECT_NEAR(lrvx_position_tracker_avg_entry(tracker, 1), 100.0, 0.01);

  lrvx_position_tracker_on_fill(tracker, 1, 1, 110.0, 1.0);
  EXPECT_NEAR(lrvx_position_tracker_position(tracker, 1), 0.0, 0.001);
  EXPECT_GT(lrvx_position_tracker_realized_pnl(tracker, 1), 0.0);

  lrvx_position_tracker_destroy(tracker);
}

// ============================================================
// Volume profile
// ============================================================

TEST(CapiProfileTest, VolumeProfile)
{
  LrvxVolumeProfileHandle vp = lrvx_volume_profile_create(0.01);
  ASSERT_NE(vp, nullptr);

  lrvx_volume_profile_add_trade(vp, 100.0, 10.0, 1);
  lrvx_volume_profile_add_trade(vp, 100.0, 5.0, 0);
  lrvx_volume_profile_add_trade(vp, 101.0, 3.0, 1);

  EXPECT_GT(lrvx_volume_profile_total_volume(vp), 0.0);
  EXPECT_GT(lrvx_volume_profile_num_levels(vp), 0u);

  lrvx_volume_profile_clear(vp);
  EXPECT_EQ(lrvx_volume_profile_num_levels(vp), 0u);

  lrvx_volume_profile_destroy(vp);
}

// ============================================================
// Stats
// ============================================================

TEST(CapiStatsTest, Correlation)
{
  double x[] = {1, 2, 3, 4, 5};
  double y[] = {1, 2, 3, 4, 5};
  EXPECT_NEAR(lrvx_stat_correlation(x, y, 5), 1.0, 0.001);

  double z[] = {5, 4, 3, 2, 1};
  EXPECT_NEAR(lrvx_stat_correlation(x, z, 5), -1.0, 0.001);
}

TEST(CapiStatsTest, ProfitFactorAndWinRate)
{
  double pnl[] = {100, -50, 200, -30};
  EXPECT_GT(lrvx_stat_profit_factor(pnl, 4), 1.0);
  EXPECT_NEAR(lrvx_stat_win_rate(pnl, 4), 0.5, 0.001);
}

TEST(CapiStatsTest, BootstrapCI)
{
  double data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  double lo, med, hi;
  lrvx_stat_bootstrap_ci(data, 10, 0.95, 1000, &lo, &med, &hi);
  EXPECT_LT(lo, med);
  EXPECT_LT(med, hi);
  EXPECT_NEAR(med, 5.5, 1.0);
}

TEST(CapiStatsTest, PermutationTest)
{
  // Very different groups — should be significant
  double g1[] = {1, 2, 3, 4, 5};
  double g2[] = {100, 200, 300, 400, 500};
  double pval = lrvx_stat_permutation_test(g1, 5, g2, 5, 5000);
  EXPECT_LT(pval, 0.05);

  // Same distribution — should NOT be significant
  double g3[] = {1, 2, 3, 4, 5};
  double g4[] = {1, 2, 3, 4, 5};
  double pval2 = lrvx_stat_permutation_test(g3, 5, g4, 5, 5000);
  EXPECT_GT(pval2, 0.5);
}

// ============================================================
// L3 Book
// ============================================================

TEST(CapiL3BookTest, AddRemoveQuery)
{
  LrvxL3BookHandle book = lrvx_l3_book_create();
  ASSERT_NE(book, nullptr);

  lrvx_l3_book_add_order(book, 1, 100.0, 1.0, 0);
  lrvx_l3_book_add_order(book, 2, 101.0, 0.5, 1);

  double p = 0;
  EXPECT_EQ(lrvx_l3_book_best_bid(book, &p), 1);
  EXPECT_NEAR(p, 100.0, 0.01);

  EXPECT_EQ(lrvx_l3_book_best_ask(book, &p), 1);
  EXPECT_NEAR(p, 101.0, 0.01);

  lrvx_l3_book_remove_order(book, 1);
  EXPECT_EQ(lrvx_l3_book_best_bid(book, &p), 0);

  lrvx_l3_book_destroy(book);
}

// ============================================================
// Composite book
// ============================================================

TEST(CapiCompositeBookTest, ApplySnapshotThenQuery)
{
  LrvxCompositeBookHandle book = lrvx_composite_book_create();
  ASSERT_NE(book, nullptr);

  double p = 0, q = 0;
  EXPECT_EQ(lrvx_composite_book_best_bid(book, 1, &p, &q), 0);

  double bid_p[] = {100.0};
  double bid_q[] = {1.5};
  double ask_p[] = {100.02};
  double ask_q[] = {0.5};
  lrvx_composite_book_apply_snapshot(book, /*exchange=*/1, /*symbol=*/1, bid_p, bid_q, 1, ask_p,
                                     ask_q, 1, /*recv_ns=*/1'000);

  EXPECT_EQ(lrvx_composite_book_best_bid(book, 1, &p, &q), 1);
  EXPECT_NEAR(p, 100.0, 0.001);
  EXPECT_NEAR(q, 1.5, 0.001);

  EXPECT_EQ(lrvx_composite_book_best_ask(book, 1, &p, &q), 1);
  EXPECT_NEAR(p, 100.02, 0.001);

  lrvx_composite_book_destroy(book);
}

TEST(CapiCompositeBookTest, BidOnlyDeltaDoesNotWipeAskSide)
{
  // Same contract as the C++ CompositeBookMatrix this wraps
  // (CompositeBookMatrixTest.BidOnlyDeltaDoesNotWipeAskSide): a delta that
  // carries only the bid side must leave a previously-set ask side intact,
  // not zero it out.
  LrvxCompositeBookHandle book = lrvx_composite_book_create();
  ASSERT_NE(book, nullptr);

  double bid_p[] = {100.0};
  double bid_q[] = {1.0};
  double ask_p[] = {100.05};
  double ask_q[] = {2.0};
  lrvx_composite_book_apply_snapshot(book, 1, 7, bid_p, bid_q, 1, ask_p, ask_q, 1, 0);

  double new_bid_p[] = {100.01};
  double new_bid_q[] = {3.0};
  lrvx_composite_book_apply_delta(book, 1, 7, new_bid_p, new_bid_q, 1, nullptr, nullptr, 0, 0);

  double p = 0, q = 0;
  EXPECT_EQ(lrvx_composite_book_best_bid(book, 7, &p, &q), 1);
  EXPECT_NEAR(p, 100.01, 0.001);

  EXPECT_EQ(lrvx_composite_book_best_ask(book, 7, &p, &q), 1);
  EXPECT_NEAR(p, 100.05, 0.001);  // unchanged by the bid-only delta

  lrvx_composite_book_destroy(book);
}

TEST(CapiCompositeBookTest, ArbitrageAcrossExchanges)
{
  LrvxCompositeBookHandle book = lrvx_composite_book_create();
  ASSERT_NE(book, nullptr);

  double bid_p1[] = {101.0};
  double bid_q1[] = {1.0};
  double ask_p1[] = {101.5};
  double ask_q1[] = {1.0};
  lrvx_composite_book_apply_snapshot(book, /*exchange=*/1, /*symbol=*/3, bid_p1, bid_q1, 1, ask_p1,
                                     ask_q1, 1, 0);

  // A second exchange bids above the first exchange's ask -- an arbitrage.
  double bid_p2[] = {102.0};
  double bid_q2[] = {1.0};
  double ask_p2[] = {103.0};
  double ask_q2[] = {1.0};
  lrvx_composite_book_apply_snapshot(book, /*exchange=*/2, /*symbol=*/3, bid_p2, bid_q2, 1, ask_p2,
                                     ask_q2, 1, 0);

  EXPECT_EQ(lrvx_composite_book_has_arb(book, 3), 1);

  lrvx_composite_book_destroy(book);
}

// ============================================================
// Order tracker
// ============================================================

TEST(CapiOrderTrackerTest, Lifecycle)
{
  LrvxOrderTrackerHandle tracker = lrvx_order_tracker_create();
  ASSERT_NE(tracker, nullptr);

  EXPECT_EQ(lrvx_order_tracker_active_count(tracker), 0u);

  lrvx_order_tracker_on_submitted(tracker, 1, 1, 0, 100.0, 1.0);
  EXPECT_EQ(lrvx_order_tracker_active_count(tracker), 1u);
  EXPECT_EQ(lrvx_order_tracker_is_active(tracker, 1), 1);

  lrvx_order_tracker_on_filled(tracker, 1, 1.0);
  EXPECT_EQ(lrvx_order_tracker_is_active(tracker, 1), 0);
  EXPECT_EQ(lrvx_order_tracker_active_count(tracker), 0u);

  lrvx_order_tracker_destroy(tracker);
}

// ============================================================
// Replay book reads (parity with Python; same C path used by Node/Codon/QuickJS)
// ============================================================

TEST(CapiReplayTest, ReadBboAndBookUpdates)
{
  using namespace lrvx::replay;

  auto dir = std::filesystem::temp_directory_path() / "lrvx_test_capi_replay";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  constexpr int kEvents = 10;
  constexpr uint16_t kBidCount = 3;
  constexpr uint16_t kAskCount = 4;

  // Write 10 book updates via the C++ writer.
  {
    WriterConfig wcfg{.output_dir = dir};
    BinaryLogWriter writer(wcfg);

    for (int i = 0; i < kEvents; ++i)
    {
      BookRecordHeader header{};
      header.exchange_ts_ns = 1000000000LL + i * 1000000LL;
      header.recv_ts_ns = header.exchange_ts_ns + 100;
      header.seq = 1000 + i;
      header.symbol_id = 7;
      header.bid_count = kBidCount;
      header.ask_count = kAskCount;
      header.type = (i == 0) ? 0 : 1;  // 0=snapshot, 1=delta

      std::vector<BookLevel> bids(kBidCount);
      std::vector<BookLevel> asks(kAskCount);
      for (uint16_t j = 0; j < kBidCount; ++j)
      {
        bids[j] = {(50000LL - j) * 1000000LL, (j + 1LL) * 1000000LL};
      }
      for (uint16_t j = 0; j < kAskCount; ++j)
      {
        asks[j] = {(50001LL + j) * 1000000LL, (j + 1LL) * 1000000LL};
      }

      EXPECT_TRUE(writer.writeBook(header, bids, asks));
    }
    writer.close();
  }

  // BBO read.
  LrvxDataReaderHandle r = lrvx_data_reader_create(dir.string().c_str());
  ASSERT_NE(r, nullptr);

  uint64_t bbo_count = lrvx_data_reader_read_bbo(r, nullptr, 0);
  EXPECT_EQ(bbo_count, static_cast<uint64_t>(kEvents));

  std::vector<LrvxBBO> bbos(bbo_count);
  uint64_t got = lrvx_data_reader_read_bbo(r, bbos.data(), bbo_count);
  EXPECT_EQ(got, bbo_count);
  for (uint64_t i = 0; i < got; ++i)
  {
    EXPECT_EQ(bbos[i].symbol_id, 7u);
    EXPECT_EQ(bbos[i].seq, static_cast<int64_t>(1000 + i));
    EXPECT_EQ(bbos[i].bid_price_raw, 50000LL * 1000000LL);
    EXPECT_EQ(bbos[i].ask_price_raw, 50001LL * 1000000LL);
  }

  // Full book updates (headers + flat levels).
  uint64_t total_levels = 0;
  uint64_t event_count = lrvx_data_reader_count_book_updates(r, &total_levels);
  EXPECT_EQ(event_count, static_cast<uint64_t>(kEvents));
  EXPECT_EQ(total_levels, static_cast<uint64_t>(kEvents) * (kBidCount + kAskCount));

  std::vector<LrvxBookUpdateHeader> headers(event_count);
  std::vector<LrvxLevel> levels(total_levels);
  uint64_t events_got = lrvx_data_reader_read_book_updates(
      r, headers.data(), event_count, levels.data(), total_levels);
  EXPECT_EQ(events_got, event_count);

  for (uint64_t i = 0; i < events_got; ++i)
  {
    const auto& h = headers[i];
    EXPECT_EQ(h.symbol_id, 7u);
    EXPECT_EQ(h.bid_count, kBidCount);
    EXPECT_EQ(h.ask_count, kAskCount);
    EXPECT_EQ(h.level_offset, i * (kBidCount + kAskCount));

    for (uint16_t k = 0; k < kBidCount; ++k)
    {
      EXPECT_EQ(levels[h.level_offset + k].side, 0);
    }
    for (uint16_t k = 0; k < kAskCount; ++k)
    {
      EXPECT_EQ(levels[h.level_offset + kBidCount + k].side, 1);
    }
  }

  lrvx_data_reader_destroy(r);
  std::filesystem::remove_all(dir);
}

namespace
{
std::vector<uint8_t> readEventTypes(const std::filesystem::path& dir,
                                    std::vector<LrvxBookUpdateHeader>* headers_out = nullptr,
                                    std::vector<LrvxLevel>* levels_out = nullptr)
{
  LrvxDataReaderHandle r = lrvx_data_reader_create(dir.string().c_str());
  EXPECT_NE(r, nullptr);
  uint64_t total_levels = 0;
  const uint64_t n = lrvx_data_reader_count_book_updates(r, &total_levels);
  std::vector<LrvxBookUpdateHeader> headers(n);
  std::vector<LrvxLevel> levels(total_levels);
  lrvx_data_reader_read_book_updates(r, headers.data(), n, levels.data(), total_levels);
  lrvx_data_reader_destroy(r);
  std::vector<uint8_t> types;
  for (const auto& h : headers)
  {
    types.push_back(h.event_type);
  }
  if (headers_out)
  {
    *headers_out = headers;
  }
  if (levels_out)
  {
    *levels_out = levels;
  }
  return types;
}
}  // namespace

// The single writer takes the recorder's byte (1 snapshot, 0 delta, 2 top);
// the reader returns the record byte (0, 1, 2) and the batched writer takes
// that back, so a read and a write_books keep every row's type.
TEST(CapiReplayTest, WriteBooksKeepsTheRecordByte)
{
  const auto base = std::filesystem::temp_directory_path() / "lrvx_test_capi_write_books";
  const auto src = base / "src";
  const auto dst = base / "dst";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(src);
  std::filesystem::create_directories(dst);

  {
    LrvxDataWriterHandle w = lrvx_data_writer_create(src.string().c_str(), 4, 0);
    ASSERT_NE(w, nullptr);
    const uint8_t kinds[] = {1, 2, 2, 0};
    for (int i = 0; i < 4; ++i)
    {
      const LrvxBookLevel bid{(100 - i) * 100000000LL, 100000000LL};
      const LrvxBookLevel ask{(101 + i) * 100000000LL, 200000000LL};
      const int64_t ts = 1000000000LL + i;
      EXPECT_EQ(lrvx_data_writer_write_book(w, ts, ts, i, 7, kinds[i], &bid, 1, &ask, 1), 1);
    }
    lrvx_data_writer_close(w);
    lrvx_data_writer_destroy(w);
  }

  std::vector<LrvxBookUpdateHeader> headers;
  std::vector<LrvxLevel> levels;
  EXPECT_EQ(readEventTypes(src, &headers, &levels), (std::vector<uint8_t>{0, 2, 2, 1}));

  // A frame type is not a record byte: the row is not written.
  auto extra = headers.back();
  extra.event_type = 3;
  headers.push_back(extra);

  {
    LrvxDataWriterHandle w = lrvx_data_writer_create(dst.string().c_str(), 4, 0);
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(lrvx_data_writer_write_books(w, headers.data(), headers.size(), levels.data(),
                                           levels.size()),
              4u);
    lrvx_data_writer_close(w);
    lrvx_data_writer_destroy(w);
  }

  std::vector<LrvxBookUpdateHeader> copied;
  std::vector<LrvxLevel> copied_levels;
  EXPECT_EQ(readEventTypes(dst, &copied, &copied_levels), (std::vector<uint8_t>{0, 2, 2, 1}));
  ASSERT_EQ(copied_levels.size(), 8u);
  EXPECT_EQ(copied_levels[2].price_raw, 99 * 100000000LL);
  EXPECT_EQ(copied_levels[3].qty_raw, 200000000LL);

  std::filesystem::remove_all(base);
}

// ============================================================
// IndicatorGraph (batch)
// ============================================================

namespace
{
// Test fixture: a node fn that returns the close array * 2.
static const double* doubleClose(void* user_data, LrvxIndicatorGraphHandle g, uint32_t sym,
                                 size_t* out_len)
{
  auto* state = static_cast<std::vector<double>*>(user_data);
  size_t n = 0;
  const double* c = lrvx_indicator_graph_close(g, sym, &n);
  state->resize(n);
  for (size_t i = 0; i < n; ++i)
  {
    (*state)[i] = c[i] * 2.0;
  }
  *out_len = n;
  return state->data();
}

// dependent: returns close + parent
static const double* sumWithDouble(void* user_data, LrvxIndicatorGraphHandle g, uint32_t sym,
                                   size_t* out_len)
{
  auto* state = static_cast<std::vector<double>*>(user_data);
  size_t n = 0;
  const double* c = lrvx_indicator_graph_close(g, sym, &n);
  size_t pn = 0;
  const double* p = lrvx_indicator_graph_get(g, sym, "double_close", &pn);
  if (!p || pn != n)
  {
    *out_len = 0;
    return nullptr;
  }
  state->resize(n);
  for (size_t i = 0; i < n; ++i)
  {
    (*state)[i] = c[i] + p[i];
  }
  *out_len = n;
  return state->data();
}
}  // namespace

TEST(CapiGraphTest, BasicComputeAndDeps)
{
  auto g = lrvx_indicator_graph_create();
  ASSERT_NE(g, nullptr);

  std::vector<double> close = {1.0, 2.0, 3.0, 4.0, 5.0};
  lrvx_indicator_graph_set_bars(g, 0, close.data(), nullptr, nullptr, nullptr, close.size());

  std::vector<double> stateA, stateB;
  lrvx_indicator_graph_add_node(g, "double_close", nullptr, 0, doubleClose, &stateA);
  const char* deps[] = {"double_close"};
  lrvx_indicator_graph_add_node(g, "sum", deps, 1, sumWithDouble, &stateB);

  size_t len = 0;
  const double* out = lrvx_indicator_graph_require(g, 0, "sum", &len);
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(len, 5u);
  for (size_t i = 0; i < 5; ++i)
  {
    EXPECT_NEAR(out[i], close[i] * 3.0, 1e-12);
  }

  // get on cached node returns the same pointer.
  size_t cl = 0;
  const double* cached = lrvx_indicator_graph_get(g, 0, "double_close", &cl);
  ASSERT_NE(cached, nullptr);
  EXPECT_EQ(cl, 5u);

  // Unknown node -> nullptr.
  size_t l2 = 999;
  const double* missing = lrvx_indicator_graph_require(g, 0, "nope", &l2);
  EXPECT_EQ(missing, nullptr);
  EXPECT_EQ(l2, 0u);

  lrvx_indicator_graph_destroy(g);
}

// ============================================================
// StreamingIndicatorGraph (C API)
// ============================================================

TEST(CapiStreamingGraphTest, StepAndCurrent)
{
  auto sg = lrvx_streaming_graph_create();
  ASSERT_NE(sg, nullptr);

  std::vector<double> stateA, stateB;
  lrvx_streaming_graph_add_node(sg, "double_close", nullptr, 0, doubleClose, &stateA);
  const char* deps[] = {"double_close"};
  lrvx_streaming_graph_add_node(sg, "sum", deps, 1, sumWithDouble, &stateB);

  // Before any steps current returns NaN.
  EXPECT_TRUE(std::isnan(lrvx_streaming_graph_current(sg, 0, "double_close")));
  EXPECT_EQ(lrvx_streaming_graph_bar_count(sg, 0), 0u);

  std::vector<double> closes = {1.0, 2.0, 3.0, 4.0, 5.0};
  for (size_t i = 0; i < closes.size(); ++i)
  {
    double c = closes[i];
    lrvx_streaming_graph_step(sg, 0, c, c, c, c, 0.0);
    EXPECT_EQ(lrvx_streaming_graph_bar_count(sg, 0), i + 1);

    // double_close node: returns close * 2 for the last bar
    EXPECT_NEAR(lrvx_streaming_graph_current(sg, 0, "double_close"), c * 2.0, 1e-12);
    // sum node: returns close + double_close = close * 3
    EXPECT_NEAR(lrvx_streaming_graph_current(sg, 0, "sum"), c * 3.0, 1e-12);
  }

  lrvx_streaming_graph_destroy(sg);
}

TEST(CapiStreamingGraphTest, ParityWithBatch)
{
  // Verify streaming current values after N steps == batch result[N-1].
  std::vector<double> stateA, stateB;

  auto sg = lrvx_streaming_graph_create();
  lrvx_streaming_graph_add_node(sg, "double_close", nullptr, 0, doubleClose, &stateA);
  const char* deps[] = {"double_close"};
  lrvx_streaming_graph_add_node(sg, "sum", deps, 1, sumWithDouble, &stateB);

  std::vector<double> closes = {10.0, 20.0, 30.0, 40.0, 50.0};
  for (double c : closes)
  {
    lrvx_streaming_graph_step(sg, 0, c, c, c, c, 0.0);
  }

  // Batch run on the same data.
  std::vector<double> batchStateA, batchStateB;
  auto bg = lrvx_indicator_graph_create();
  lrvx_indicator_graph_add_node(bg, "double_close", nullptr, 0, doubleClose, &batchStateA);
  lrvx_indicator_graph_add_node(bg, "sum", deps, 1, sumWithDouble, &batchStateB);
  lrvx_indicator_graph_set_bars(bg, 0, closes.data(), nullptr, nullptr, nullptr, closes.size());

  size_t len = 0;
  const double* batchSum = lrvx_indicator_graph_require(bg, 0, "sum", &len);
  ASSERT_EQ(len, closes.size());

  // Streaming current == batch last element.
  EXPECT_NEAR(lrvx_streaming_graph_current(sg, 0, "sum"), batchSum[len - 1], 1e-12);

  lrvx_indicator_graph_destroy(bg);
  lrvx_streaming_graph_destroy(sg);
}

TEST(CapiStreamingGraphTest, Reset)
{
  std::vector<double> stateA;
  auto sg = lrvx_streaming_graph_create();
  lrvx_streaming_graph_add_node(sg, "double_close", nullptr, 0, doubleClose, &stateA);

  lrvx_streaming_graph_step(sg, 0, 5.0, 5.0, 5.0, 5.0, 0.0);
  EXPECT_NEAR(lrvx_streaming_graph_current(sg, 0, "double_close"), 10.0, 1e-12);
  EXPECT_EQ(lrvx_streaming_graph_bar_count(sg, 0), 1u);

  lrvx_streaming_graph_reset(sg, 0);
  EXPECT_EQ(lrvx_streaming_graph_bar_count(sg, 0), 0u);
  EXPECT_TRUE(std::isnan(lrvx_streaming_graph_current(sg, 0, "double_close")));

  // Can resume stepping after reset.
  lrvx_streaming_graph_step(sg, 0, 7.0, 7.0, 7.0, 7.0, 0.0);
  EXPECT_NEAR(lrvx_streaming_graph_current(sg, 0, "double_close"), 14.0, 1e-12);

  lrvx_streaming_graph_destroy(sg);
}

// ============================================================
// ABI version
// ============================================================

// The ABI number is what a consumer loading the shared library compares
// against before it trusts a struct layout, and the only test that touched it
// compared the macro with the function the same header compiled -- true
// whatever the number says. Anchoring it to a shape the header itself declares
// makes the number falsifiable: LrvxBar gained close_reason at 3, so a header
// that has the field and calls itself 2 is describing a struct it does not
// have. LrvxBookSnapshot gained has_bid and has_ask at 4, by the same rule.
template <typename T>
concept LrvxBarCarriesCloseReason = requires(T bar) { bar.close_reason; };

template <typename T>
concept LrvxBookSnapshotCarriesPresence = requires(T snap) {
  snap.has_bid;
  snap.has_ask;
};

TEST(CapiAbiVersion, TheNumberMovedWithTheStructShape)
{
  EXPECT_EQ(lrvx_capi_abi_version(), LRVX_CAPI_ABI_VERSION);
  EXPECT_GT(lrvx_capi_abi_version(), 0u);

  if constexpr (LrvxBarCarriesCloseReason<LrvxBar>)
  {
    EXPECT_GE(lrvx_capi_abi_version(), 3u)
        << "LrvxBar carries close_reason, which is the change ABI 3 names; a "
           "library reporting "
        << lrvx_capi_abi_version()
        << " tells a consumer to read the struct it had before the field existed";
  }
  else
  {
    EXPECT_LT(lrvx_capi_abi_version(), 3u)
        << "the header claims ABI 3 or later but LrvxBar has no close_reason";
  }

  if constexpr (LrvxBookSnapshotCarriesPresence<LrvxBookSnapshot>)
  {
    EXPECT_GE(lrvx_capi_abi_version(), 4u)
        << "LrvxBookSnapshot carries has_bid and has_ask, which is the change ABI 4 "
           "names; a library reporting "
        << lrvx_capi_abi_version()
        << " tells a consumer to read the struct it had before the flags existed";
  }
  else
  {
    EXPECT_LT(lrvx_capi_abi_version(), 4u)
        << "the header claims ABI 4 or later but LrvxBookSnapshot has no presence flags";
  }
}
