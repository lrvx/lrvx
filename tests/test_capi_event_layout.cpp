/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The Codon binding has no struct declarations to work from -- the codegen
// emits every out-struct as an opaque cobj -- so codon/lrvx/dispatch.codon
// reads LrvxTradeData, LrvxBarData, LrvxOrderEventData and LrvxSymbolContext
// by integer byte offsets typed in by hand, and codon/lrvx/strategy.codon
// does the same for LrvxBar. The offsets are right today. Nothing in the
// build says so: add a field, widen one, reorder two, and every strategy
// written in Codon starts reading a different field than it asks for, with
// no diagnostic anywhere.
//
// The generated table in lrvx_capi_layout.h is what the Codon side is built
// from. This test is the other end of it: every entry is compared against
// offsetof/sizeof on the real struct, so a layout change that does not reach
// the table fails here rather than in a user's strategy.

#include "lrvx/capi/lrvx_capi.h"

#if __has_include("lrvx/capi/lrvx_capi_layout.h")
#include "lrvx/capi/lrvx_capi_layout.h"
#define LRVX_HAS_LAYOUT_TABLE 1
#else
#define LRVX_HAS_LAYOUT_TABLE 0
#endif

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace
{

struct Field
{
  const char* structName;
  const char* fieldName;
  std::size_t offset;
  std::size_t size;
};

#define LRVX_FIELD(S, F) \
  Field { #S, #F, offsetof(S, F), sizeof(S::F) }

// Every field of every struct the Codon binding reads. Padding is left out:
// a table may carry it, nothing may depend on it.
const std::vector<Field>& expectedFields()
{
  static const std::vector<Field> kFields = {
      LRVX_FIELD(LrvxTradeData, symbol),
      LRVX_FIELD(LrvxTradeData, price_raw),
      LRVX_FIELD(LrvxTradeData, quantity_raw),
      LRVX_FIELD(LrvxTradeData, is_buy),
      LRVX_FIELD(LrvxTradeData, exchange_ts_ns),

      LRVX_FIELD(LrvxBookSnapshot, bid_price_raw),
      LRVX_FIELD(LrvxBookSnapshot, bid_qty_raw),
      LRVX_FIELD(LrvxBookSnapshot, ask_price_raw),
      LRVX_FIELD(LrvxBookSnapshot, ask_qty_raw),
      LRVX_FIELD(LrvxBookSnapshot, mid_raw),
      LRVX_FIELD(LrvxBookSnapshot, spread_raw),
      LRVX_FIELD(LrvxBookSnapshot, has_bid),
      LRVX_FIELD(LrvxBookSnapshot, has_ask),

      LRVX_FIELD(LrvxSymbolContext, symbol_id),
      LRVX_FIELD(LrvxSymbolContext, has_avg_entry_price),
      LRVX_FIELD(LrvxSymbolContext, position_raw),
      LRVX_FIELD(LrvxSymbolContext, avg_entry_price_raw),
      LRVX_FIELD(LrvxSymbolContext, last_trade_price_raw),
      LRVX_FIELD(LrvxSymbolContext, last_update_ns),
      LRVX_FIELD(LrvxSymbolContext, book),
      LRVX_FIELD(LrvxSymbolContext, last_touch_ns),

      LRVX_FIELD(LrvxBarData, symbol),
      LRVX_FIELD(LrvxBarData, bar_type),
      LRVX_FIELD(LrvxBarData, close_reason),
      LRVX_FIELD(LrvxBarData, bar_type_param),
      LRVX_FIELD(LrvxBarData, open_raw),
      LRVX_FIELD(LrvxBarData, high_raw),
      LRVX_FIELD(LrvxBarData, low_raw),
      LRVX_FIELD(LrvxBarData, close_raw),
      LRVX_FIELD(LrvxBarData, volume_raw),
      LRVX_FIELD(LrvxBarData, buy_volume_raw),
      LRVX_FIELD(LrvxBarData, trade_count_raw),
      LRVX_FIELD(LrvxBarData, start_time_ns),
      LRVX_FIELD(LrvxBarData, end_time_ns),

      LRVX_FIELD(LrvxOrderEventData, order_id),
      LRVX_FIELD(LrvxOrderEventData, symbol_id),
      LRVX_FIELD(LrvxOrderEventData, side),
      LRVX_FIELD(LrvxOrderEventData, order_type),
      LRVX_FIELD(LrvxOrderEventData, status),
      LRVX_FIELD(LrvxOrderEventData, fill_qty_raw),
      LRVX_FIELD(LrvxOrderEventData, fill_price_raw),
      LRVX_FIELD(LrvxOrderEventData, exchange_ts_ns),
      LRVX_FIELD(LrvxOrderEventData, reject_reason),
      LRVX_FIELD(LrvxOrderEventData, queue_ahead_raw),
      LRVX_FIELD(LrvxOrderEventData, queue_total_raw),
      LRVX_FIELD(LrvxOrderEventData, submitted_at_ns),
      LRVX_FIELD(LrvxOrderEventData, accepted_at_ns),
      LRVX_FIELD(LrvxOrderEventData, first_fill_at_ns),
      LRVX_FIELD(LrvxOrderEventData, last_fill_at_ns),
      LRVX_FIELD(LrvxOrderEventData, canceled_at_ns),
      LRVX_FIELD(LrvxOrderEventData, rejected_at_ns),
      LRVX_FIELD(LrvxOrderEventData, triggered_at_ns),
      LRVX_FIELD(LrvxOrderEventData, expired_at_ns),
      LRVX_FIELD(LrvxOrderEventData, is_maker),
      LRVX_FIELD(LrvxOrderEventData, market_position),
      LRVX_FIELD(LrvxOrderEventData, distance_to_best_ticks),

      LRVX_FIELD(LrvxBar, start_time_ns),
      LRVX_FIELD(LrvxBar, end_time_ns),
      LRVX_FIELD(LrvxBar, open_raw),
      LRVX_FIELD(LrvxBar, high_raw),
      LRVX_FIELD(LrvxBar, low_raw),
      LRVX_FIELD(LrvxBar, close_raw),
      LRVX_FIELD(LrvxBar, volume_raw),
      LRVX_FIELD(LrvxBar, buy_volume_raw),
      LRVX_FIELD(LrvxBar, trade_count),
      LRVX_FIELD(LrvxBar, close_reason),
  };
  return kFields;
}

#undef LRVX_FIELD

struct Struct
{
  const char* name;
  std::size_t size;
  std::size_t alignment;
};

#define LRVX_STRUCT(S) \
  Struct { #S, sizeof(S), alignof(S) }

const std::vector<Struct>& expectedStructs()
{
  static const std::vector<Struct> kStructs = {
      LRVX_STRUCT(LrvxTradeData),
      LRVX_STRUCT(LrvxBookSnapshot),
      LRVX_STRUCT(LrvxSymbolContext),
      LRVX_STRUCT(LrvxBarData),
      LRVX_STRUCT(LrvxOrderEventData),
      LRVX_STRUCT(LrvxBar),
  };
  return kStructs;
}

#undef LRVX_STRUCT

#if LRVX_HAS_LAYOUT_TABLE
std::size_t fieldCount()
{
  return sizeof(kLrvxEventLayout) / sizeof(kLrvxEventLayout[0]);
}

std::size_t structCount()
{
  return sizeof(kLrvxEventStructLayout) / sizeof(kLrvxEventStructLayout[0]);
}

const LrvxFieldLayout* findField(const char* structName, const char* fieldName)
{
  for (std::size_t i = 0; i < fieldCount(); ++i)
  {
    if (std::strcmp(kLrvxEventLayout[i].struct_name, structName) == 0 &&
        std::strcmp(kLrvxEventLayout[i].field_name, fieldName) == 0)
    {
      return &kLrvxEventLayout[i];
    }
  }
  return nullptr;
}

const LrvxStructLayout* findStruct(const char* name)
{
  for (std::size_t i = 0; i < structCount(); ++i)
  {
    if (std::strcmp(kLrvxEventStructLayout[i].struct_name, name) == 0)
    {
      return &kLrvxEventStructLayout[i];
    }
  }
  return nullptr;
}
#endif

constexpr const char* kMissingTable =
    "include/lrvx/capi/lrvx_capi_layout.h is missing: the Codon binding's "
    "struct offsets are generated from it and this test is what checks them "
    "against offsetof";

}  // namespace

TEST(CapiEventLayout, TableCarriesEveryFieldCodonCanRead)
{
#if !LRVX_HAS_LAYOUT_TABLE
  FAIL() << kMissingTable;
#else
  for (const auto& f : expectedFields())
  {
    EXPECT_NE(findField(f.structName, f.fieldName), nullptr)
        << f.structName << "." << f.fieldName << " has no layout entry";
  }
#endif
}

TEST(CapiEventLayout, EveryOffsetAndWidthMatchesTheStruct)
{
#if !LRVX_HAS_LAYOUT_TABLE
  FAIL() << kMissingTable;
#else
  for (const auto& f : expectedFields())
  {
    const LrvxFieldLayout* entry = findField(f.structName, f.fieldName);
    if (entry == nullptr)
    {
      continue;  // reported by TableCarriesEveryFieldCodonCanRead
    }
    EXPECT_EQ(static_cast<std::size_t>(entry->offset), f.offset)
        << f.structName << "." << f.fieldName << " moved";
    EXPECT_EQ(static_cast<std::size_t>(entry->size), f.size)
        << f.structName << "." << f.fieldName << " changed width";
  }
#endif
}

TEST(CapiEventLayout, StructSizesAndAlignmentsMatch)
{
#if !LRVX_HAS_LAYOUT_TABLE
  FAIL() << kMissingTable;
#else
  for (const auto& s : expectedStructs())
  {
    const LrvxStructLayout* entry = findStruct(s.name);
    ASSERT_NE(entry, nullptr) << s.name << " has no layout entry";
    EXPECT_EQ(static_cast<std::size_t>(entry->size), s.size) << s.name;
    EXPECT_EQ(static_cast<std::size_t>(entry->alignment), s.alignment) << s.name;
  }
#endif
}

TEST(CapiEventLayout, TableDescribesNoFieldTheStructDoesNotHave)
{
  // A generated table that grew an entry nobody can account for is as bad as
  // a missing one: the Codon side would import a constant pointing at
  // nothing. Padding members are the only unlisted names allowed.
#if !LRVX_HAS_LAYOUT_TABLE
  FAIL() << kMissingTable;
#else
  for (std::size_t i = 0; i < fieldCount(); ++i)
  {
    const LrvxFieldLayout& entry = kLrvxEventLayout[i];
    if (entry.field_name[0] == '_')
    {
      continue;
    }
    bool known = false;
    for (const auto& f : expectedFields())
    {
      if (std::strcmp(f.structName, entry.struct_name) == 0 &&
          std::strcmp(f.fieldName, entry.field_name) == 0)
      {
        known = true;
        break;
      }
    }
    EXPECT_TRUE(known) << "layout entry " << entry.struct_name << "."
                       << entry.field_name << " matches no field of that struct";
  }
#endif
}

TEST(CapiEventLayout, RejectReasonIsReachable)
{
  // The field the C ABI fills in on every rejection, and the one Codon had no
  // constant for at all.
#if !LRVX_HAS_LAYOUT_TABLE
  FAIL() << kMissingTable;
#else
  const LrvxFieldLayout* entry = findField("LrvxOrderEventData", "reject_reason");
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(static_cast<std::size_t>(entry->offset),
            offsetof(LrvxOrderEventData, reject_reason));
#endif
}
