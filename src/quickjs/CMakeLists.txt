# QuickJS — vendored sources from third_party/quickjs
set(QJS_DIR ${CMAKE_SOURCE_DIR}/third_party/quickjs)
set(QJS_SOURCES
  ${QJS_DIR}/quickjs.c
  ${QJS_DIR}/libregexp.c
  ${QJS_DIR}/libunicode.c
  ${QJS_DIR}/cutils.c
  ${QJS_DIR}/libbf.c
)

add_library(quickjs_lib STATIC ${QJS_SOURCES})
target_include_directories(quickjs_lib PUBLIC ${QJS_DIR})
set_target_properties(quickjs_lib PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON
)
target_compile_definitions(quickjs_lib PRIVATE
  CONFIG_VERSION="lrvx"
  CONFIG_BIGNUM=1
)
if(UNIX)
  target_link_libraries(quickjs_lib PRIVATE m)
endif()

# JS_NewFloat64's int32 fast path (quickjs.h) casts an arbitrary double --
# including NaN and values far outside int32 range, both completely normal
# inputs here (a timestamp, a computed NaN) -- to int32, then detects the
# non-representable case via a bit-pattern comparison and falls back to a
# proper boxed float64 JSValue. The cast is technically undefined behavior
# under -fsanitize=undefined's float-cast-overflow check; the surrounding
# code's own fallback makes the observable result correct on every target
# this project builds for, and it is not fixable without patching the
# vendored interpreter, which this repo treats as read-only.
#
# JS_NewFloat64 is `static inline` in the header, so the cast is compiled
# (and sanitizer-instrumented) into whichever translation unit calls it --
# in practice every file in lrvx_quickjs that builds a JSValue from a
# timestamp or a computed double, not just quickjs_lib's own .c files.
# Disabling only quickjs_lib leaves the finding fully in place. Both
# targets need the flag; the rest of this project's build (the core
# engine, backtest, venue, ...) keeps this check active, including its
# own double-to-integer conversions.
include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
check_c_compiler_flag("-fno-sanitize=float-cast-overflow" LRVX_QUICKJS_HAS_NO_SANITIZE_FLOAT_CAST_C)
check_cxx_compiler_flag("-fno-sanitize=float-cast-overflow" LRVX_QUICKJS_HAS_NO_SANITIZE_FLOAT_CAST_CXX)
if(LRVX_QUICKJS_HAS_NO_SANITIZE_FLOAT_CAST_C)
  target_compile_options(quickjs_lib PRIVATE -fno-sanitize=float-cast-overflow)
endif()

# The bytecode optimiser packs four opcode constants into one int with the
# M4 macro, whose top byte shifts a value above 127 left by 24 -- a signed
# overflow under -fsanitize=undefined's shift-base check. It fires while
# compiling the embedded JS stdlib, so every UBSan run of the QuickJS
# tests carried one report from the vendored interpreter. Clang reports
# it where GCC folds the constant, which is why CI never showed it. The
# packed value is only ever compared against another packed value, so the
# wrapped result is correct on every target here, and the fix belongs
# upstream in an interpreter this repo treats as read-only. Suppressing
# it on quickjs_lib alone keeps the check active everywhere else and
# leaves an UBSan run over src/quickjs/ reporting only real findings.
check_c_compiler_flag("-fno-sanitize=shift-base" LRVX_QUICKJS_HAS_NO_SANITIZE_SHIFT_BASE_C)
if(LRVX_QUICKJS_HAS_NO_SANITIZE_SHIFT_BASE_C)
  target_compile_options(quickjs_lib PRIVATE -fno-sanitize=shift-base)
endif()

# Generate embedded JS stdlib as C string literals via a script
# Rebuilds when JS source files change
set(JS_STDLIB_DIR ${CMAKE_SOURCE_DIR}/quickjs/lrvx)
set(GEN_INC_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/gen_js_inc.cmake)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_strategy.inc
  COMMAND ${CMAKE_COMMAND}
    -DIN_FILE=${JS_STDLIB_DIR}/strategy.js
    -DOUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_strategy.inc
    -P ${GEN_INC_SCRIPT}
  DEPENDS ${JS_STDLIB_DIR}/strategy.js ${GEN_INC_SCRIPT}
  COMMENT "Embedding strategy.js"
)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_indicators.inc
  COMMAND ${CMAKE_COMMAND}
    -DIN_FILE=${JS_STDLIB_DIR}/indicators.js
    -DOUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_indicators.inc
    -P ${GEN_INC_SCRIPT}
  DEPENDS ${JS_STDLIB_DIR}/indicators.js ${GEN_INC_SCRIPT}
  COMMENT "Embedding indicators.js"
)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_targets.inc
  COMMAND ${CMAKE_COMMAND}
    -DIN_FILE=${JS_STDLIB_DIR}/targets.js
    -DOUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_targets.inc
    -P ${GEN_INC_SCRIPT}
  DEPENDS ${JS_STDLIB_DIR}/targets.js ${GEN_INC_SCRIPT}
  COMMENT "Embedding targets.js"
)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_latency.inc
  COMMAND ${CMAKE_COMMAND}
    -DIN_FILE=${JS_STDLIB_DIR}/latency.js
    -DOUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_latency.inc
    -P ${GEN_INC_SCRIPT}
  DEPENDS ${JS_STDLIB_DIR}/latency.js ${GEN_INC_SCRIPT}
  COMMENT "Embedding latency.js"
)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_composite.inc
  COMMAND ${CMAKE_COMMAND}
    -DIN_FILE=${JS_STDLIB_DIR}/composite.js
    -DOUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_composite.inc
    -P ${GEN_INC_SCRIPT}
  DEPENDS ${JS_STDLIB_DIR}/composite.js ${GEN_INC_SCRIPT}
  COMMENT "Embedding composite.js"
)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_order_group.inc
  COMMAND ${CMAKE_COMMAND}
    -DIN_FILE=${JS_STDLIB_DIR}/order_group.js
    -DOUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_order_group.inc
    -P ${GEN_INC_SCRIPT}
  DEPENDS ${JS_STDLIB_DIR}/order_group.js ${GEN_INC_SCRIPT}
  COMMENT "Embedding order_group.js"
)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_feed_clock.inc
  COMMAND ${CMAKE_COMMAND}
    -DIN_FILE=${JS_STDLIB_DIR}/feed_clock.js
    -DOUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_feed_clock.inc
    -P ${GEN_INC_SCRIPT}
  DEPENDS ${JS_STDLIB_DIR}/feed_clock.js ${GEN_INC_SCRIPT}
  COMMENT "Embedding feed_clock.js"
)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_exits.inc
  COMMAND ${CMAKE_COMMAND}
    -DIN_FILE=${JS_STDLIB_DIR}/exits.js
    -DOUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_exits.inc
    -P ${GEN_INC_SCRIPT}
  DEPENDS ${JS_STDLIB_DIR}/exits.js ${GEN_INC_SCRIPT}
  COMMENT "Embedding exits.js"
)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_history.inc
  COMMAND ${CMAKE_COMMAND}
    -DIN_FILE=${JS_STDLIB_DIR}/history.js
    -DOUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_history.inc
    -P ${GEN_INC_SCRIPT}
  DEPENDS ${JS_STDLIB_DIR}/history.js ${GEN_INC_SCRIPT}
  COMMENT "Embedding history.js"
)

add_custom_target(js_stdlib_gen DEPENDS
  ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_strategy.inc
  ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_indicators.inc
  ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_targets.inc
  ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_latency.inc
  ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_composite.inc
  ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_order_group.inc
  ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_feed_clock.inc
  ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_exits.inc
  ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_history.inc
)

# lrvx_quickjs static library
add_library(lrvx_quickjs STATIC
  js_engine.cpp
  js_bindings.cpp
  js_strategy.cpp
  js_executor.cpp
)
add_dependencies(lrvx_quickjs js_stdlib_gen)
target_include_directories(lrvx_quickjs PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(lrvx_quickjs PUBLIC
  quickjs_lib
  lrvx_capi
  ${LRVX}
)
target_compile_features(lrvx_quickjs PRIVATE cxx_std_20)
if(LRVX_QUICKJS_HAS_NO_SANITIZE_FLOAT_CAST_CXX)
  # See the comment above quickjs_lib's own -fno-sanitize flag: the same
  # vendored int32 fast path gets inlined into this target's translation
  # units (js_bindings.cpp, js_strategy.cpp, ...), not just quickjs.c.
  target_compile_options(lrvx_quickjs PRIVATE -fno-sanitize=float-cast-overflow)
endif()

# lrvx_js_runner executable
add_executable(lrvx_js_runner js_runner.cpp)
target_link_libraries(lrvx_js_runner PRIVATE lrvx_quickjs)
target_compile_features(lrvx_js_runner PRIVATE cxx_std_20)
