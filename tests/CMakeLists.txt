if(NOT TARGET GTest::gtest)
  find_package(GTest REQUIRED)
endif()
find_package(Threads)

function(add_lrvx_test name)
  add_executable(${name} ${name}.cpp)
  target_compile_definitions(${name} PRIVATE LRVX_UNIT_TEST)
  target_include_directories(${name}
    PRIVATE
        include()
  )
  target_link_libraries(${name}
    PRIVATE ${LRVX}
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
    GTest::gmock_main
  )
  if(TARGET Threads::Threads)
    target_link_libraries(${name} PRIVATE Threads::Threads)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# The same test source built a second time under extra compile definitions.
# The fixed-point suite uses it to build itself with LRVX_FORCE_PORTABLE_INT128,
# so the software arithmetic every MSVC build runs is compiled and run on
# macOS and Linux too, instead of waiting for a Windows CI job to find out.
function(add_lrvx_test_variant name source)
  add_executable(${name} ${source}.cpp)
  target_compile_definitions(${name} PRIVATE LRVX_UNIT_TEST ${ARGN})
  target_link_libraries(${name}
    PRIVATE ${LRVX}
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
    GTest::gmock_main
  )
  if(TARGET Threads::Threads)
    target_link_libraries(${name} PRIVATE Threads::Threads)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_lrvx_test(test_book_update_bus)
add_lrvx_test(test_socket_portability)
add_lrvx_test(test_socket_poll_and_family)
# Reads the reference page it is checking, so it needs to know where the
# sources are rather than where the test binary happens to run.
target_compile_definitions(test_socket_poll_and_family PRIVATE
  LRVX_DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../docs")
add_lrvx_test(test_thread_body)
add_lrvx_test(test_shadow_mode)
add_lrvx_test(test_latency_contour)
add_lrvx_test(test_rt_spin_guard)
add_lrvx_test(test_event_bus_health)
add_lrvx_test(test_event_bus_step)
add_lrvx_test(test_event_bus_park)
add_lrvx_test(test_event_bus_wake_set)
add_lrvx_test(test_memory_profile)
add_lrvx_test(test_large_arena)
add_lrvx_test(test_live_rate_limit_budgeter)
add_lrvx_test(test_bar_aggregator)
add_lrvx_test(test_time_bar_alignment)
add_lrvx_test(test_bar_aggregator_semantics)
# The two guarded cases in that file assert against the interface the fix
# introduces: RenkoBarPolicy::kMaxGapBricks with BarCloseReason::Gap on the
# bar that absorbs a capped gap, and BarAggregator::lateTradeCount().
target_compile_definitions(test_bar_aggregator_semantics
                           PRIVATE LRVX_RENKO_GAP_BOUND_API LRVX_LATE_TRADE_COUNTER_API)
add_lrvx_test(test_multi_feed_clock)
add_lrvx_test(test_connection_factory)
add_lrvx_test(test_connector_manager)
add_lrvx_test(test_decimal)
add_lrvx_test(test_decimal_locale)
# Built twice: once the way this toolchain would, once with the native 128-bit
# intermediate compiled out, so Decimal's MSVC arithmetic is run here too.
add_lrvx_test(test_decimal_portable)
add_lrvx_test_variant(test_decimal_portable_forced test_decimal_portable
                      LRVX_FORCE_PORTABLE_INT128=1)
add_lrvx_test(test_math_fastdiv)
add_lrvx_test(test_scale)
add_lrvx_test(test_u256)
add_lrvx_test(test_hot_path_allocation)
add_lrvx_test(test_pool_concurrent_release)
add_lrvx_test(test_i256)
add_lrvx_test(test_lp_signal)
add_lrvx_test(test_quoter)
add_lrvx_test(test_exits)
add_lrvx_test(test_venue_behavior)
add_lrvx_test(test_engine_smoke)
add_lrvx_test(test_engine_timebase)
add_lrvx_test(test_event_bus_batch)
add_lrvx_test(test_event_bus_lifecycle)
add_lrvx_test(test_event_pool)
add_lrvx_test(test_pool_object_lifetime)
add_lrvx_test(test_l3_order_book)
add_lrvx_test(test_multi_execution_listener)
add_lrvx_test(test_fill_price_dispatch)
add_lrvx_test(test_nlevel_order_book)
add_lrvx_test(test_nlevel_order_book_window)
add_lrvx_test(test_nlevel_order_book_model)
add_lrvx_test(test_nlevel_tick_rounding)
add_lrvx_test(test_nlevel_order_book_negative_prices)
add_lrvx_test(test_order_execution_bus)
add_lrvx_test(test_pool_batch)
add_lrvx_test(test_spsc_queue_segments)
add_lrvx_test(test_order_lifecycle)
add_lrvx_test(test_onchain_lifecycle)
add_lrvx_test(test_push_pull_subscribers)
add_lrvx_test(test_ref_countable)
add_lrvx_test(test_spsc_advanced)
add_lrvx_test(test_spsc)
add_lrvx_test(test_symbol_registry)
add_lrvx_test(test_atomic_logger)
add_lrvx_test(test_log_defaults_and_cost)
add_lrvx_test(test_strategy_position_refresh)
add_lrvx_test(test_order_tracker)
add_lrvx_test(test_order_tracker_replaced)
add_lrvx_test(test_order_tracker_capacity)
add_lrvx_test(test_order_tracker_exchange_lookup)
add_lrvx_test(test_batch_submit_hooks)
add_lrvx_test(test_rate_limiter)
add_lrvx_test(test_rate_limiter_zero_refill)
add_lrvx_test(test_order_router_concurrency)
add_lrvx_test(test_order_router_typed)

add_lrvx_test(test_sync_order_execution_bus)
add_lrvx_test(test_sync_book_update_bus)
add_lrvx_test(test_sync_bar_aggregator)

# EventBus core tests
add_lrvx_test(test_event_bus)

# Replay tests
add_lrvx_test(test_binary_log)
add_lrvx_test(test_replay_tape_integrity)
add_lrvx_test(test_replay_writer_rotation)
add_lrvx_test(test_replay_sorted_flag)
add_lrvx_test(test_replay_side_channel_frames)
add_lrvx_test(test_replay_merge_stability)
add_lrvx_test(test_replay_validator_overlap)
add_lrvx_test(test_delta_book)
add_lrvx_test(test_replay_connector)
add_lrvx_test(test_aggregator_framework)
add_lrvx_test(test_book_snapshot_aggregator)
add_lrvx_test(test_recording_metadata)
# Run-trace tests (.lrvxrun)
add_lrvx_test(test_run_trace)

# Multi-TF alignment helpers
add_lrvx_test(test_multi_tf_alignment)
add_lrvx_test(test_bar_matrix_sizing)

# Run-trace adapter handlers (.lrvxrun auto-capture)
add_lrvx_test(test_trace_handlers)

# Bar-close ordering on tied timestamps
add_lrvx_test(test_bar_close_ordering)

add_lrvx_test(test_multi_symbol_strategy)

add_lrvx_test(test_order_group_replace)

add_lrvx_test(test_position_aggregator)
add_lrvx_test(test_position_valuator)
add_lrvx_test(test_position_tracker_fixed_point)
add_lrvx_test(test_clearing_fixed_point)
add_lrvx_test(test_strategy_unrealized_pnl)
add_lrvx_test(test_portfolio_risk_gates)

# CEX coordination tests
add_lrvx_test(test_cex_coordination)

if(LRVX_ENABLE_ONNX)
  add_lrvx_test(test_onnx_inference)
  target_compile_definitions(test_onnx_inference PRIVATE
    LRVX_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
endif()

# test_receive_path uses POSIX sockets; not built on Windows. The FIX initiator
# tests sit here for the same reason: the sequence sidecar writes through
# open/fsync/rename and the transport is a POSIX socket.
if(NOT WIN32)
  add_lrvx_test(test_receive_path)
  add_lrvx_test(test_fix_initiator)
  add_lrvx_test(test_fix_client_parser_fuzz)
endif()

if(LRVX_ENABLE_BACKTEST)
  add_lrvx_test(test_reference_backtest)
  # Reads docs/examples/data/btcusdt_1m.csv, the data file the reference page
  # fixes, so it needs the source tree rather than the test binary's directory.
  target_compile_definitions(test_reference_backtest PRIVATE
    LRVX_DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../docs")
  add_lrvx_test(test_backtest)
  add_lrvx_test(test_fixed_point_muldiv)
  add_lrvx_test(test_strategy_pump)
  add_lrvx_test(test_ntoken_curve)
  add_lrvx_test(test_constant_product_curve)
  add_lrvx_test(test_raydium_cp_curve)
  add_lrvx_test(test_stableswap_curve)
  add_lrvx_test(test_saber_stableswap)
  add_lrvx_test(test_cryptoswap_curve)
  add_lrvx_test(test_repegging_cryptoswap_pool)
  add_lrvx_test(test_weighted_curve)
  add_lrvx_test(test_concentrated_liquidity_curve)
  add_lrvx_test(test_orca_whirlpool_curve)
  add_lrvx_test(test_adaptive_fee_whirlpool_curve)
  add_lrvx_test(test_raydium_clmm_curve)
  add_lrvx_test(test_meteora_dlmm_curve)
  add_lrvx_test(test_token2022_transfer_fee)
  add_lrvx_test(test_amm_dex_connector)
  add_lrvx_test(test_pool_quote_view)
  add_lrvx_test(test_amm_pool_replay_source)
  add_lrvx_test(test_pool_state_tape)
  add_lrvx_test(test_pool_state_binary_log)
  add_lrvx_test(test_evm_v3_ingest)
  add_lrvx_test(test_reorg_safe_pool_feed)
  add_lrvx_test(test_solana_cp_ingest)
  add_lrvx_test(test_interactive_runner)
  add_lrvx_test(test_advanced_orders)
  add_lrvx_test(test_mmap_bar_storage)
  add_lrvx_test(test_mmap_bar_writer)
  add_lrvx_test(test_optimization_stats)
  add_lrvx_test(test_robustness_stats)
  add_lrvx_test(test_grid_search)
  add_lrvx_test(test_backtest_slippage)
  add_lrvx_test(test_backtest_queue)
  add_lrvx_test(test_resting_limit_fills)
  add_lrvx_test(test_executor_fill_reentrancy)
  add_lrvx_test(test_order_timestamps)
  add_lrvx_test(test_maker_taker)
  add_lrvx_test(test_cancel_latency)
  add_lrvx_test(test_submit_ack_latency)
  add_lrvx_test(test_latency_profiles)
  add_lrvx_test(test_latency_distribution)
  add_lrvx_test(test_order_journey_tracer)
  add_lrvx_test(test_market_position)
  add_lrvx_test(test_replace_in_flight)
  add_lrvx_test(test_backtest_metrics)
  add_lrvx_test(test_backtest_fee_attribution)
  add_lrvx_test(test_backtest_economics)
  add_lrvx_test(test_backtest_ctx_position)
  add_lrvx_test(test_backtest_run_tape)
  # Needs the backtest runner's tape path alongside the replay writer/reader,
  # and reads docs/spec/lrvx.md to check the recorded side-convention
  # decision.
  add_lrvx_test(test_replay_trade_side)
  target_compile_definitions(test_replay_trade_side PRIVATE
    LRVX_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
  add_lrvx_test(test_backtest_runner_hooks)
  add_lrvx_test(test_backtest_runner_venue_stack)
  add_lrvx_test(test_backtest_fill_realism)
  add_lrvx_test(test_backtest_fill_model)
  add_lrvx_test(test_maker_taker_fees)
  add_lrvx_test(test_live_queue_position)
  add_lrvx_test(test_extended_tif)
  add_lrvx_test(test_hidden_order_attribution)
  add_lrvx_test(test_funding_schedule)
  add_lrvx_test(test_rate_limit_policy)
  add_lrvx_test(test_fee_schedule)
  add_lrvx_test(test_stp_modes)
  add_lrvx_test(test_pro_rata_matching)
  add_lrvx_test(test_venue_availability)
  add_lrvx_test(test_bracket_orders)
  add_lrvx_test(test_liquidation_engine)
  add_lrvx_test(test_venue_stack_composition)
  add_lrvx_test(test_iceberg_orders)
  add_lrvx_test(test_account)
  add_lrvx_test(test_venue_stack)
  add_lrvx_test(test_venue_stack_realism_integration)
  add_lrvx_test(test_cross_cascade)
  add_lrvx_test(test_venue_stack_reproducibility)
  add_lrvx_test(test_venue_stack_calibration)
  add_lrvx_test(test_option_settlement)
  add_lrvx_test(test_option_exercise)
  add_lrvx_test(test_option_fill_model)
  add_lrvx_test(test_system_clock)
  add_lrvx_test(test_time_domains)
endif()

add_lrvx_test(test_indicators)
add_lrvx_test(test_streaming_parity)
add_lrvx_test(test_targets)
add_lrvx_test(test_adf)
add_lrvx_test(test_autocorrelation)
add_lrvx_test(test_portfolio_greeks)
add_lrvx_test(test_delta_hedger)
add_lrvx_test(test_hedge_cost)
add_lrvx_test(test_hedge_templates)
add_lrvx_test(test_black_scholes)
add_lrvx_test(test_greeks)
add_lrvx_test(test_american_pricing)
add_lrvx_test(test_svi_surface)
add_lrvx_test(test_carry)
add_lrvx_test(test_trading_calendar)
add_lrvx_test(test_vol_cone)
add_lrvx_test(test_option_quote_record)
add_lrvx_test(test_option_quote_io)

if(LRVX_BUILD_CAPI)
  add_lrvx_test(test_capi_event_layout)
  target_link_libraries(test_capi_event_layout PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_registry)
  target_link_libraries(test_capi_registry PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_dex_amount)
  target_link_libraries(test_capi_dex_amount PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_amm_curve)
  target_link_libraries(test_capi_amm_curve PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_pool_tape)
  target_link_libraries(test_capi_pool_tape PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_infra)
  target_link_libraries(test_capi_infra PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_risk_manager)
  target_link_libraries(test_capi_risk_manager PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_kill_switch)
  target_link_libraries(test_capi_kill_switch PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_observers)
  target_link_libraries(test_capi_observers PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_tracker_lifetime)
  target_link_libraries(test_capi_tracker_lifetime PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_market_data_recorder)
  target_link_libraries(test_capi_market_data_recorder PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_replay_source)
  target_link_libraries(test_capi_replay_source PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_execution_listener)
  target_link_libraries(test_capi_execution_listener PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_executor)
  target_link_libraries(test_capi_executor PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_contract)
  target_link_libraries(test_capi_contract PRIVATE lrvx_capi)
  # Reads include/lrvx/capi/lrvx_capi.h back as text to check the ownership
  # notes the calling contract promises.
  target_compile_definitions(test_capi_contract PRIVATE
    LRVX_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
  add_lrvx_test(test_capi_abi_check)
  target_link_libraries(test_capi_abi_check PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_venue_stack)
  target_link_libraries(test_capi_venue_stack PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_diagnostics)
  target_link_libraries(test_capi_diagnostics PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_signal_codes)
  target_link_libraries(test_capi_signal_codes PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_bar_aggregation)
  target_link_libraries(test_capi_bar_aggregation PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_book_snapshot)
  target_link_libraries(test_capi_book_snapshot PRIVATE lrvx_capi)
  add_lrvx_test(test_capi_negative_book)
  target_link_libraries(test_capi_negative_book PRIVATE lrvx_capi)
  # The strategy-side case needs a strategy that already holds a book; the
  # fixture that builds one is force-included so the test source stays on the
  # C API and gtest alone.
  target_precompile_headers(test_capi_negative_book
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support/capi_strategy_under_test.h)
  # test_capi_logger needs to share a single ILogger global with lrvx_capi.
  # If we use add_lrvx_test, the test exe links the static `lrvx` library
  # alongside lrvx_capi.dylib (which itself statically embeds `lrvx`) —
  # two copies of g_logger, so lrvx_set_log_callback in the dylib doesn't
  # affect LRVX_LOG_* calls compiled into the test exe. Build manually to
  # link only lrvx_capi (which exports every symbol the test reaches).
  add_executable(test_capi_logger test_capi_logger.cpp)
  target_compile_definitions(test_capi_logger PRIVATE LRVX_UNIT_TEST)
  target_link_libraries(test_capi_logger
    PRIVATE lrvx_capi
    GTest::gtest
    GTest::gtest_main
  )
  if(TARGET Threads::Threads)
    target_link_libraries(test_capi_logger PRIVATE Threads::Threads)
  endif()
  add_test(NAME test_capi_logger COMMAND test_capi_logger)
  if(LRVX_ENABLE_BACKTEST)
    add_lrvx_test(test_capi_backtest)
    target_link_libraries(test_capi_backtest PRIVATE lrvx_capi)
    add_lrvx_test(test_capi_backtest_timestamps)
    target_link_libraries(test_capi_backtest_timestamps PRIVATE lrvx_capi)
    add_lrvx_test(test_capi_executor_bar_ohlc)
    target_link_libraries(test_capi_executor_bar_ohlc PRIVATE lrvx_capi)
  endif()
  add_lrvx_test(test_order_type_names)
endif()

if(LRVX_BUILD_QUICKJS)
  add_lrvx_test(test_quickjs)
  target_link_libraries(test_quickjs PRIVATE lrvx_quickjs)
  # One test runs quickjs/examples/backtest_sma.js and checks the numbers it
  # computes. The example loads its CSV by a path relative to the repo root,
  # so the test has to know where that is.
  target_compile_definitions(test_quickjs PRIVATE
    LRVX_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

if(LRVX_ENABLE_CPU_AFFINITY)
    add_lrvx_test(test_cpu_affinity)
endif()
