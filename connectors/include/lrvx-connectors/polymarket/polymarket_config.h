/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx-connectors/execution/instrument_spec.h"

#include <string>
#include <vector>

namespace lrvx
{

struct PolymarketConfig
{
  std::string wsEndpoint{"wss://ws-subscriptions-clob.polymarket.com/ws/market"};

  /// Token IDs to subscribe to for market data
  std::vector<std::string> tokenIds;

  int reconnectDelayMs{1000};
  int pingIntervalSec{5};

  /// Window after which a token that stopped ticking is reported through
  /// emitStaleData. 0 disables the check: a quiet prediction market is
  /// normal, so there is no window the connector can pick for you.
  int staleDataTimeoutMs{0};

  /// CLOB REST base each token's tick size is read from at start(): GET
  /// /tick-size?token_id=... (minimum_tick_size). The token is then
  /// registered with that tick, marked known. Empty skips the load. A token
  /// whose tick could not be read is logged as an error and registered with
  /// its tick unknown. The venue can change a market's tick while it trades
  /// (the tick_size_change message); this connector does not follow that.
  std::string instrumentInfoUrl{"https://clob.polymarket.com"};
  /// The request the load goes through; empty uses defaultInstrumentFetch().
  InstrumentFetch instrumentFetch;

  bool isValid() const { return !wsEndpoint.empty(); }
  // The connector is WS-only and the order executor takes the signing key,
  // the funder wallet and the REST endpoint through its own constructor, so
  // privateKey/funderWallet/restEndpoint are gone: a config field nothing
  // reads is a lie in the public API, and for the two credentials it also
  // invited an operator to write a plaintext key into a file no code path
  // ever consumed. (hasCredentials() went the same way, for the same
  // reason.)
};

}  // namespace lrvx
