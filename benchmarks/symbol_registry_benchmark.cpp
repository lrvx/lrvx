/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The registry's by-id lookups. The tables behind them are dense for the ids
// a registry normally holds and a hash map above that, so that one large
// explicit id is one entry and not an array sized up to it. The small-id
// cases here are the ones that must not pay for that: compare them across a
// change to the tables. The large-id cases show what the hash map costs.

#include "lrvx/engine/symbol_registry.h"

#include <benchmark/benchmark.h>

#include <array>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

// `count` symbols with the ids 1..count, mapped in pairs as equivalents, and
// one more under `extraId` when it is not 0.
struct Populated
{
  SymbolRegistry registry;
  std::vector<SymbolId> ids;

  Populated(size_t count, SymbolId extraId)
  {
    const ExchangeId venue = registry.registerExchange("BENCH");
    for (size_t i = 0; i < count; ++i)
    {
      ids.push_back(registry.registerSymbol(venue, "SYM" + std::to_string(i)));
    }
    for (size_t i = 0; i + 1 < ids.size(); i += 2)
    {
      const std::array<SymbolId, 2> pair{ids[i], ids[i + 1]};
      registry.mapEquivalentSymbols(pair);
    }
    if (extraId != 0)
    {
      SymbolInfo info;
      info.id = extraId;
      info.exchange = "BENCH";
      info.symbol = "FAR";
      registry.registerSymbol(info);
    }
  }
};

constexpr SymbolId kFarId = 1'000'000;

void BM_ExchangeForSymbol_SmallIds(benchmark::State& state)
{
  Populated p(static_cast<size_t>(state.range(0)), static_cast<SymbolId>(state.range(1)));
  size_t i = 0;
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(p.registry.getExchangeForSymbol(p.ids[i]));
    i = i + 1 == p.ids.size() ? 0 : i + 1;
  }
}
BENCHMARK(BM_ExchangeForSymbol_SmallIds)->Args({64, 0})->Args({1024, 0})->Args({1024, kFarId});

void BM_SymbolName_SmallIds(benchmark::State& state)
{
  Populated p(static_cast<size_t>(state.range(0)), static_cast<SymbolId>(state.range(1)));
  size_t i = 0;
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(p.registry.getSymbolName(p.ids[i]));
    i = i + 1 == p.ids.size() ? 0 : i + 1;
  }
}
BENCHMARK(BM_SymbolName_SmallIds)->Args({64, 0})->Args({1024, 0})->Args({1024, kFarId});

void BM_EquivalentSymbols_SmallIds(benchmark::State& state)
{
  Populated p(static_cast<size_t>(state.range(0)), static_cast<SymbolId>(state.range(1)));
  size_t i = 0;
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(p.registry.getEquivalentSymbols(p.ids[i]));
    i = i + 1 == p.ids.size() ? 0 : i + 1;
  }
}
BENCHMARK(BM_EquivalentSymbols_SmallIds)->Args({64, 0})->Args({1024, 0})->Args({1024, kFarId});

void BM_ExchangeForSymbol_FarId(benchmark::State& state)
{
  Populated p(1024, kFarId);
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(p.registry.getExchangeForSymbol(kFarId));
  }
}
BENCHMARK(BM_ExchangeForSymbol_FarId);

void BM_SymbolName_FarId(benchmark::State& state)
{
  Populated p(1024, kFarId);
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(p.registry.getSymbolName(kFarId));
  }
}
BENCHMARK(BM_SymbolName_FarId);

// What registering one far id costs: the tables used to be sized up to it.
void BM_RegisterOneFarId(benchmark::State& state)
{
  for (auto _ : state)
  {
    Populated p(16, kFarId);
    benchmark::DoNotOptimize(p.registry.size());
  }
}
BENCHMARK(BM_RegisterOneFarId);

}  // namespace

BENCHMARK_MAIN();
