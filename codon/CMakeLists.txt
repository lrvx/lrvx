find_program(CODON_COMPILER codon HINTS $ENV{CODON_HOME}/bin $ENV{HOME}/.codon/bin)

if(NOT CODON_COMPILER)
  message(WARNING "Codon compiler not found. Skipping Codon build. "
                  "Set CODON_HOME or add codon to PATH.")
  return()
endif()

message(STATUS "Found Codon compiler: ${CODON_COMPILER}")

# Compile a Codon source file to a native executable
# Every Codon executable compiles the binding in; a change under
# codon/lrvx/ must rebuild them all, or a tree (or a CI cache) tests the
# old binding.
file(GLOB_RECURSE LRVX_CODON_BINDING_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/lrvx/*.codon")

function(add_codon_executable name source)
  set(OUTPUT_EXE ${CMAKE_CURRENT_BINARY_DIR}/${name})
  add_custom_command(
    OUTPUT ${OUTPUT_EXE}
    COMMAND ${CMAKE_COMMAND} -E env
      "LIBRARY_PATH=$<TARGET_FILE_DIR:lrvx_capi>"
      "CODON_PATH=${CMAKE_CURRENT_SOURCE_DIR}"
      ${CODON_COMPILER} build
        -o ${OUTPUT_EXE}
        -l lrvx_capi
        ${source}
    DEPENDS ${source} ${LRVX_CODON_BINDING_SOURCES} lrvx_capi
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Compiling Codon: ${name}"
  )
  add_custom_target(build_${name} ALL DEPENDS ${OUTPUT_EXE})
endfunction()

# Engine threads allocate Codon objects in callbacks; this one forces the
# collector to run on those threads and checks the objects survive.
add_codon_executable(codon_threaded_gc_stress
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/threaded_gc_stress.codon)

# Forces realization of the binding surface; see the file header.
add_codon_executable(codon_binding_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/binding_smoke.codon)

add_codon_executable(codon_sma_crossover
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/sma_crossover.codon)

add_codon_executable(codon_pairs_strategy
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/pairs_strategy.codon)

add_codon_executable(codon_backtest_from_csv
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/backtest_from_csv.codon)

add_codon_executable(codon_full_backtest
  ${CMAKE_SOURCE_DIR}/docs/examples/codon_full_backtest.codon)

add_codon_executable(codon_targets_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/targets_smoke.codon)

add_codon_executable(codon_adf_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/adf_smoke.codon)

add_codon_executable(codon_autocorrelation_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/autocorrelation_smoke.codon)

add_codon_executable(codon_graph_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/graph_smoke.codon)

add_codon_executable(codon_streaming_graph_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/streaming_graph_smoke.codon)

add_codon_executable(codon_latency_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/latency_smoke.codon)

add_codon_executable(codon_tape_diff_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/tape_diff_smoke.codon)

add_codon_executable(codon_portfolio_risk_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/portfolio_risk_smoke.codon)

add_codon_executable(codon_execution_algos_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/execution_algos_smoke.codon)

add_codon_executable(codon_delta_book_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/delta_book_smoke.codon)

add_codon_executable(codon_order_book_window_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/order_book_window_smoke.codon)

add_codon_executable(codon_composite_book_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/composite_book_smoke.codon)

add_codon_executable(codon_run_trace_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/run_trace_smoke.codon)

add_codon_executable(codon_multi_tf_helpers_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/multi_tf_helpers_smoke.codon)

add_codon_executable(codon_composite_dsl_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/composite_dsl_smoke.codon)

add_codon_executable(codon_order_group_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/order_group_smoke.codon)

add_codon_executable(codon_bar_close_ordering_parity_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/bar_close_ordering_parity_smoke.codon)

add_codon_executable(codon_feed_clock_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/feed_clock_smoke.codon)

add_codon_executable(codon_trace_attach_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/trace_attach_smoke.codon)

add_codon_executable(codon_byte_identical_fixture
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/byte_identical_fixture.codon)

add_codon_executable(codon_strategy_e2e_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/strategy_e2e_smoke.codon)

add_codon_executable(codon_merged_tape_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/merged_tape_smoke.codon)

add_codon_executable(codon_reader_segments_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/reader_segments_smoke.codon)

add_codon_executable(codon_backtest_run_tapes_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/backtest_run_tapes_smoke.codon)

add_codon_executable(codon_backtest_run_tape_bars_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/backtest_run_tape_bars_smoke.codon)

add_codon_executable(codon_backtest_run_tape_bars_on_ticks_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/backtest_run_tape_bars_on_ticks_smoke.codon)

add_codon_executable(codon_order_type_codes_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/order_type_codes_smoke.codon)

add_codon_executable(codon_indicator_bounds_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/indicator_bounds_smoke.codon)

add_codon_executable(codon_indicator_streaming_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/indicator_streaming_smoke.codon)

add_codon_executable(codon_indicator_params_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/indicator_params_smoke.codon)

add_codon_executable(codon_signal_fields_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/signal_fields_smoke.codon)

add_codon_executable(codon_aggregate_bars_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/aggregate_bars_smoke.codon)

add_codon_executable(codon_strategy_dispatch_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/strategy_dispatch_smoke.codon)

# The documented backtest-and-live walkthrough. Built here because it was
# broken for as long as nothing compiled it: it passed List[u32] where
# Strategy.__init__ wants List[int], and its on_trade handed a Price to an
# indicator that takes a float -- a body no compiler had ever looked at,
# because a never-dispatched override is never realised.
add_codon_executable(codon_backtest_vs_live
  ${CMAKE_SOURCE_DIR}/docs/examples/codon_backtest_vs_live.codon)
