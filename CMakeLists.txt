cmake_minimum_required(VERSION 3.22)
project(lrvx VERSION 0.11.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# An unset CMAKE_BUILD_TYPE is not a neutral default here: without NDEBUG,
# LRVX_SCALE_CHECKS turns on (include/lrvx/util/base/scale_check.h) and adds a
# member to Decimal, so sizeof(Price) goes 8 -> 16 and every struct holding a
# price or quantity changes layout. Consumers that compile their own
# translation units against a prebuilt archive (the Node addon does) then read
# fields at the wrong offsets, which corrupts memory instead of failing to
# link. Every build recipe in docs/ omitted the build type, so this state was
# the documented default. Pin it.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  message(STATUS "CMAKE_BUILD_TYPE was unset; defaulting to Release")
endif()

option(LRVX_ENABLE_DEV_SETUP "Install pre-commit hook" OFF)

# ──────────────────────────────────────────────────────────────────────
# LRVX_VENUE_LITE -- the execution-venue profile.
#
# A downstream consumer that runs the execution venue compiles against 37 of
# this tree's headers (venue/lite_surface.txt): the sequencer and its
# storage, the matching engine, the network perimeter, the FIX initiator, and
# the utilities those need. Everything else here -- the backtest module, the
# exchange connectors and their DEX curves, Python/Node/C-API/Codon/QuickJS,
# the demo, the benchmarks -- is weight that consumer carries and never
# calls, and each of them drags a dependency: OpenSSL, CURL, zlib,
# ixwebsocket, simdjson, LZ4, pybind11.
#
# So this is not a list of targets to skip at build time. It is a list of
# subdirectories that are never added and find_package calls that never run:
# a lite configure asks the system for nothing the venue does not use. The
# options below are FORCEd rather than defaulted because a stale cache from
# a full configure would otherwise leave half of them on.
#
# What stays: the venue module (engine + perimeter), the parts of the core
# library the venue links, and the FIX initiator. What the profile installs
# is the closure of venue/lite_surface.txt and those targets -- see the
# install section at the end of this file and docs/venue/build-profiles.md.
#
# Declared before every option it overrides, so the overrides are in place
# on a first configure.
# ──────────────────────────────────────────────────────────────────────
option(LRVX_VENUE_LITE "Build only the execution venue: engine, perimeter, FIX initiator, utilities" OFF)

if(LRVX_VENUE_LITE)
  macro(_lrvx_lite_force name value)
    set(${name} ${value} CACHE BOOL "Forced by LRVX_VENUE_LITE" FORCE)
  endmacro()

  _lrvx_lite_force(LRVX_BUILD_VENUE ON)

  # The reason this profile exists. Before the clearing primitives moved out
  # of lrvx/backtest/ (FeeSchedule, Account, LeveragedPosition -> lrvx/clearing/),
  # the venue could not be built without the backtest module, so a consumer
  # who never ran a backtest compiled all of it.
  _lrvx_lite_force(LRVX_ENABLE_BACKTEST OFF)

  foreach(_off
      LRVX_BUILD_BENCHMARKS LRVX_BUILD_DEMO LRVX_BUILD_TOOLS
      LRVX_BUILD_PYTHON LRVX_BUILD_NODE LRVX_BUILD_CAPI
      LRVX_BUILD_CODON LRVX_BUILD_QUICKJS LRVX_BUILD_CONNECTORS
      LRVX_ENABLE_ONNX LRVX_ENABLE_AF_XDP LRVX_ENABLE_TRACY
      LRVX_ENABLE_LZ4)
    _lrvx_lite_force(${_off} OFF)
  endforeach()

  message(STATUS "LRVX_VENUE_LITE: venue engine, perimeter, FIX initiator and utilities only")

  # venue/lite_surface.txt is read once, here, rather than as a second list:
  # its transitive closure (scripts/lite_closure.py --paths) is both the set
  # of headers this configure installs (below, in the install section) and
  # the filter that decides which core .cpp files SRC_FILES gets (right
  # after this block) -- a src/<x>/<y>.cpp is compiled only if
  # include/lrvx/<x>/<y>.h is on the closure. The same call is the direction
  # gate: a closure that has grown into lrvx/backtest/ or lrvx/aggregator/
  # fails the configure here, before anything is built.
  execute_process(
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/scripts/lite_closure.py" --paths
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE LRVX_LITE_CLOSURE
    ERROR_VARIABLE LRVX_LITE_CLOSURE_ERR
    RESULT_VARIABLE LRVX_LITE_CLOSURE_RC)
  if(NOT LRVX_LITE_CLOSURE_RC EQUAL 0)
    message(FATAL_ERROR
      "scripts/lite_closure.py rejected the venue-lite surface:\n"
      "${LRVX_LITE_CLOSURE}${LRVX_LITE_CLOSURE_ERR}")
  endif()
  string(STRIP "${LRVX_LITE_CLOSURE}" LRVX_LITE_CLOSURE)
  string(REPLACE "\n" ";" LRVX_LITE_CLOSURE "${LRVX_LITE_CLOSURE}")
  list(LENGTH LRVX_LITE_CLOSURE _lrvx_lite_closure_n)
  message(STATUS "LRVX_VENUE_LITE: closure of venue/lite_surface.txt is ${_lrvx_lite_closure_n} headers")
endif()


if(LRVX_ENABLE_DEV_SETUP)
  message(STATUS "Developer setup enabled")

  set(PRECOMMIT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/scripts/pre-commit")
  set(PRECOMMIT_DST "${CMAKE_CURRENT_SOURCE_DIR}/.git/hooks/pre-commit")

  if(EXISTS "${PRECOMMIT_SRC}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${PRECOMMIT_SRC}" "${PRECOMMIT_DST}")
    execute_process(COMMAND chmod +x "${PRECOMMIT_DST}")
    message(STATUS "Installed pre-commit hook to .git/hooks")
  else()
    message(WARNING "pre-commit script not found at ${PRECOMMIT_SRC}")
  endif()
endif()

include_directories(include)

# The core library's sources. The lite profile compiles the subset the venue
# links and nothing else: no replay (10k lines, and the only thing in the
# core that needs LZ4), no report, no risk, no run. Not every .cpp under
# these four directories, either -- most of what is in them (engine.cpp,
# symbol_registry.cpp, algos.cpp, order_journey_tracer.cpp,
# order_tracker.cpp, atomic_logger.cpp) exists for the full engine and the
# venue links none of it. A src/<x>/<y>.cpp is kept only when
# include/lrvx/<x>/<y>.h -- its own header -- is on the venue/lite_surface.txt
# closure computed above; that is what decides the install surface too, so
# both answers come from one list. Verified empirically, not read off the
# header graph: build with the filter, link, and see what is missing.
if(LRVX_VENUE_LITE)
  file(GLOB_RECURSE _lrvx_lite_candidate_src
      src/clearing/*.cpp
      src/engine/*.cpp
      src/execution/*.cpp
      src/log/*.cpp
  )
  set(SRC_FILES "")
  foreach(_cpp ${_lrvx_lite_candidate_src})
    file(RELATIVE_PATH _cpp_rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_cpp}")
    string(REGEX REPLACE "^src/" "include/lrvx/" _cpp_hdr "${_cpp_rel}")
    string(REGEX REPLACE "\\.cpp$" ".h" _cpp_hdr "${_cpp_hdr}")
    if(_cpp_hdr IN_LIST LRVX_LITE_CLOSURE)
      list(APPEND SRC_FILES "${_cpp}")
    endif()
  endforeach()
  list(LENGTH SRC_FILES _lrvx_lite_src_n)
  message(STATUS "LRVX_VENUE_LITE: ${_lrvx_lite_src_n} core .cpp files match the closure: ${SRC_FILES}")
else()
  file(GLOB_RECURSE SRC_FILES
      src/aggregator/*.cpp
      src/clearing/*.cpp
      src/engine/*.cpp
      src/execution/*.cpp
      src/log/*.cpp
      src/replay/*.cpp
      src/report/*.cpp
      src/risk/*.cpp
      src/run/*.cpp
  )
endif()

# The venue module reuses backtest-side primitives (FeeSchedule,
# RateLimitPolicy, LiquidationEngine, Account), so it implies the backtest
# module. Declared here, before the option below, so the implication applies on
# a first configure; the add_subdirectory(venue) itself lives further down,
# after enable_testing() so the module's tests register.
# GoogleTest is normally found already built. That works until the compiler
# building lrvx and the compiler that built GoogleTest disagree about which
# types exist: clang-cl has __int128, cl does not, and GoogleTest's header
# declares a printer for it under __SIZEOF_INT128__. A library compiled by cl
# never contains that printer, so a clang-cl build of code that puts a
# 128-bit value in an assertion fails to link on a symbol neither project got
# wrong. Building GoogleTest from source with the same compiler removes the
# disagreement rather than working around it.
option(LRVX_FETCH_GTEST "Build GoogleTest from source instead of finding a prebuilt one" OFF)
if(LRVX_BUILD_TESTS AND LRVX_FETCH_GTEST)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  message(STATUS "LRVX_FETCH_GTEST: GoogleTest built here, by this compiler")
endif()

option(LRVX_BUILD_VENUE "Build the venue module (matching engine, clearing, venue risk)" ON)

# The venue ledger keeps money in native 128-bit integers so a notional can
# never overflow and a rounding drift is impossible.
#
# cl has no such type and no announced plan for one, so the module is
# unavailable there until `venue::Amount` is ported onto the portable wide
# integers in lrvx/util/int.
#
# clang-cl is a different compiler wearing cl's command line. It has
# __int128, and its runtime helpers (__divti3 and friends) come from
# clang_rt.builtins. The test is therefore the compiler's identity, not
# CMake's MSVC variable -- that variable is true for anything speaking cl's
# command line, clang-cl included, and gating on it turned the module off on
# a compiler that can build it.
if(LRVX_BUILD_VENUE AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  message(STATUS "LRVX_BUILD_VENUE: cl has no native 128-bit integer type; disabling.")
  set(LRVX_BUILD_VENUE OFF CACHE BOOL "Build the venue module" FORCE)
endif()

option(LRVX_ENABLE_BACKTEST "Enable backtest module" OFF)
# What is left of the implication after the clearing primitives moved into
# the core: the venue LIBRARY needs nothing from lrvx/backtest/ any more, but
# one venue test (test_venue_derivatives) drives the backtest liquidation
# engine through lrvx-venue/liquidation_monitor.h, a header-only helper that
# is compiled by nothing else. So the implication stays for the full profile,
# where it costs nothing and keeps every existing build byte-identical, and
# the lite profile is exempt: it builds with LRVX_ENABLE_BACKTEST=OFF and
# drops that one test, which is the criterion this profile was written to.
if(LRVX_BUILD_VENUE AND NOT LRVX_ENABLE_BACKTEST AND NOT LRVX_VENUE_LITE)
  message(STATUS "LRVX_BUILD_VENUE=ON implies LRVX_ENABLE_BACKTEST=ON; enabling.")
  set(LRVX_ENABLE_BACKTEST ON CACHE BOOL "Enable backtest module" FORCE)
endif()
if(LRVX_ENABLE_BACKTEST)
  file(GLOB BACKTEST_SRC_FILES src/backtest/*.cpp)
  list(APPEND SRC_FILES ${BACKTEST_SRC_FILES})
  message(STATUS "Backtest module enabled")
endif()

set(LRVX ${PROJECT_NAME})

add_library(${LRVX} STATIC ${SRC_FILES})
add_library(lrvx::${LRVX} ALIAS ${LRVX})

# Global EventBus sizing knobs. The headers guard these with #ifndef, but they
# only take effect if forwarded as compile definitions -- passing them as -D
# cache vars alone did nothing. Documented in docs/how-to/optimize-performance.md.
set(LRVX_DEFAULT_EVENTBUS_CAPACITY "" CACHE STRING "Default EventBus ring capacity (power of 2)")
set(LRVX_DEFAULT_EVENTBUS_MAX_CONSUMERS "" CACHE STRING "Default EventBus max consumers")
if(LRVX_DEFAULT_EVENTBUS_CAPACITY)
  target_compile_definitions(${LRVX} PUBLIC LRVX_DEFAULT_EVENTBUS_CAPACITY=${LRVX_DEFAULT_EVENTBUS_CAPACITY})
endif()
if(LRVX_DEFAULT_EVENTBUS_MAX_CONSUMERS)
  target_compile_definitions(${LRVX} PUBLIC LRVX_DEFAULT_EVENTBUS_MAX_CONSUMERS=${LRVX_DEFAULT_EVENTBUS_MAX_CONSUMERS})
endif()

# 128-bit arithmetic needs runtime helpers -- __divti3, __floattidf, __fixdfti
# -- and where they come from is a property of the toolchain, not of the
# language. GCC and Clang on Unix link libgcc or compiler-rt for you. clang-cl
# targeting the MSVC ABI does not: the Microsoft link line has no such default,
# and the omission surfaces as undefined symbols at the very end of a build.
#
# This belongs to the core and not to one module: money arithmetic uses the
# 128-bit path everywhere, and while this lived under venue/ the guards in
# common.h steered clang-cl around that path into a narrower fallback that
# overflows on ordinary prices. The helpers are linked here so the path is
# available to everything that computes with money.
#
# The library is found at configure time, so a configure that cannot find it
# says so here instead of letting the link fail eight minutes later.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
  set(LRVX_CLANG_BUILTINS "")
  # Ask the compiler first; it knows where its own runtime lives.
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" --rtlib=compiler-rt -print-libgcc-file-name
    OUTPUT_VARIABLE _rt OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE _rt_rc)
  if(_rt_rc EQUAL 0 AND EXISTS "${_rt}")
    set(LRVX_CLANG_BUILTINS "${_rt}")
  else()
    # Otherwise look where clang keeps it, next to the driver.
    get_filename_component(_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_root "${_bin}" DIRECTORY)
    file(GLOB_RECURSE _found "${_root}/lib/clang/*/lib/windows/clang_rt.builtins-x86_64.lib")
    if(_found)
      list(GET _found 0 LRVX_CLANG_BUILTINS)
    endif()
  endif()
  if(NOT LRVX_CLANG_BUILTINS)
    message(FATAL_ERROR
      "clang-cl needs clang_rt.builtins for 128-bit arithmetic (__divti3 and "
      "friends) and it was not found near ${CMAKE_CXX_COMPILER}. Install the "
      "LLVM runtime.")
  endif()
  message(STATUS "128-bit helpers from ${LRVX_CLANG_BUILTINS}")
  target_link_libraries(${LRVX} PUBLIC "${LRVX_CLANG_BUILTINS}")
endif()

# Platform-specific settings
if(WIN32)
  # Prevent Windows.h from defining min/max macros that break std::min/max
  target_compile_definitions(${LRVX} PUBLIC NOMINMAX)
endif()

# LRVX_NATIVE controls whether Release builds target the build machine's
# instruction set (`-march=native`) or a portable baseline. `native` is
# fastest but ties the resulting binary to the build host — if the build
# CPU has AVX-512 and the runtime CPU does not, the process dies with
# SIGILL on the first vector instruction. Default ON preserves the
# historical local-dev behaviour; wheel/binary distribution paths
# (python/pyproject.toml) flip it OFF.
option(LRVX_NATIVE
  "Compile Release with -march=native. Disable for distributable artifacts."
  ON)

if(NOT MSVC)
  if(LRVX_NATIVE)
    set(LRVX_ARCH_OPT -march=native)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    # x86-64-v3 = AVX2 + BMI2 + FMA, baseline for Haswell/Excavator
    # (~2015) and up. Covers the vast majority of distribution targets
    # without requiring AVX-512.
    set(LRVX_ARCH_OPT -march=x86-64-v3)
  else()
    # arm64/aarch64 etc.: compiler default is portable enough; macOS
    # arm64 wheels are arch-tagged anyway so native vs. baseline is moot.
    set(LRVX_ARCH_OPT "")
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  # Genuine cl.exe: whole-program optimization via /GL + /LTCG.
  target_compile_options(${LRVX} PUBLIC
    $<$<CONFIG:Release>:/O2 /GL>
  )
  target_link_options(${LRVX} PUBLIC
    $<$<CONFIG:Release>:/LTCG>
  )
elseif(MSVC)
  # clang-cl has the MSVC frontend but does not accept /GL at compile time
  # (it emits "argument unused"). Keep /O2; LTO would need -flto, not /GL.
  target_compile_options(${LRVX} PUBLIC
    $<$<CONFIG:Release>:/O2>
  )
else()
  set(LRVX_RELEASE_OPTS -O3 -flto -funroll-loops)
  if(LRVX_ARCH_OPT)
    list(APPEND LRVX_RELEASE_OPTS ${LRVX_ARCH_OPT})
  endif()
  target_compile_options(${LRVX} PUBLIC
    $<$<CONFIG:Release>:${LRVX_RELEASE_OPTS}>
  )
  # LTO requires -flto flag at link time as well
  target_link_options(${LRVX} PUBLIC
    $<$<CONFIG:Release>:-flto>
  )
endif()

# WARNING: CPU affinity can decrease performance on busy/shared systems.
# It prevents OS scheduler optimization and should only be used on isolated,
# dedicated hardware where you control the entire system workload.
option(LRVX_ENABLE_CPU_AFFINITY "Enable CPU affinity and NUMA functionality" OFF)
if(LRVX_ENABLE_CPU_AFFINITY)
  message(STATUS "CPU affinity features enabled")
  target_compile_definitions(${LRVX} PUBLIC LRVX_CPU_AFFINITY_ENABLED=1)
  # Find and conditionally link NUMA library for CPU affinity functionality
  find_library(NUMA_LIBRARY numa)
  if(NUMA_LIBRARY)
    target_link_libraries(${LRVX} PUBLIC ${NUMA_LIBRARY})
    target_compile_definitions(${LRVX} PUBLIC LRVX_NUMA_LIBRARY_LINKED=1)
    message(STATUS "Found NUMA library: ${NUMA_LIBRARY}")
  else()
    target_compile_definitions(${LRVX} PUBLIC LRVX_NUMA_LIBRARY_LINKED=0)
    message(WARNING "NUMA library not found")
  endif()
else()
  message(STATUS "CPU affinity features disabled")
  target_compile_definitions(${LRVX} PUBLIC LRVX_CPU_AFFINITY_ENABLED=0)
  target_compile_definitions(${LRVX} PUBLIC LRVX_NUMA_LIBRARY_LINKED=0)
endif()

# ONNX Runtime inference nodes for the indicator graph (lrvx/ml/). Off by
# default: onnxruntime is a heavy optional dependency of the ML last mile.
option(LRVX_ENABLE_ONNX "Enable ONNX Runtime inference nodes" OFF)
if(LRVX_ENABLE_ONNX)
  find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime)
  find_path(ONNXRUNTIME_INCLUDE_DIR NAMES onnxruntime_cxx_api.h PATH_SUFFIXES onnxruntime onnxruntime/core/session)
  if(ONNXRUNTIME_LIBRARY AND ONNXRUNTIME_INCLUDE_DIR)
    target_include_directories(${LRVX} PUBLIC ${ONNXRUNTIME_INCLUDE_DIR})
    target_link_libraries(${LRVX} PUBLIC ${ONNXRUNTIME_LIBRARY})
    target_compile_definitions(${LRVX} PUBLIC LRVX_ONNX_ENABLED=1)
    message(STATUS "ONNX Runtime found: ${ONNXRUNTIME_LIBRARY}")
  else()
    message(FATAL_ERROR "LRVX_ENABLE_ONNX=ON but onnxruntime was not found")
  endif()
else()
  target_compile_definitions(${LRVX} PUBLIC LRVX_ONNX_ENABLED=0)
endif()

# AF_XDP receive-path backend (lrvx/net/af_xdp_receive_path.h). Linux-only,
# needs libxdp/libbpf; off by default.
option(LRVX_ENABLE_AF_XDP "Enable AF_XDP receive-path backend" OFF)
if(LRVX_ENABLE_AF_XDP)
  find_library(XDP_LIBRARY NAMES xdp)
  find_library(BPF_LIBRARY NAMES bpf)
  if(XDP_LIBRARY AND BPF_LIBRARY)
    target_link_libraries(${LRVX} PUBLIC ${XDP_LIBRARY} ${BPF_LIBRARY})
    target_compile_definitions(${LRVX} PUBLIC LRVX_AF_XDP_ENABLED=1)
    message(STATUS "AF_XDP backend enabled: ${XDP_LIBRARY}")
  else()
    message(FATAL_ERROR "LRVX_ENABLE_AF_XDP=ON but libxdp/libbpf were not found")
  endif()
else()
  target_compile_definitions(${LRVX} PUBLIC LRVX_AF_XDP_ENABLED=0)
endif()

option(LRVX_ENABLE_LZ4 "Enable LZ4 compression for binary logs" ON)
# The imported target lrvx was linked against when lz4 came from a config
# package; lrvxConfig.cmake.in re-finds that package for consumers, since the
# export file names the target and nothing else defines it. Empty for every
# other way of getting lz4, and with LZ4 off.
set(LRVX_LZ4_TARGET "")
if(LRVX_ENABLE_LZ4)
  # Try find_package first. Which target the config defines depends on who
  # packaged it: vcpkg exports lz4::lz4; lz4's own CMake build, which Homebrew
  # installs the package files from, exports LZ4::lz4 over LZ4::lz4_shared and
  # LZ4::lz4_static and nothing in lower case. lz4_FOUND only says a config
  # was read, so the target is picked by existence, and a config that defines
  # none of these falls through to the lookups below instead of failing the
  # generate step on a name that was never there.
  find_package(lz4 CONFIG QUIET)
  foreach(_lrvx_lz4_candidate lz4::lz4 LZ4::lz4 LZ4::lz4_shared LZ4::lz4_static)
    if(TARGET ${_lrvx_lz4_candidate})
      set(LRVX_LZ4_TARGET ${_lrvx_lz4_candidate})
      break()
    endif()
  endforeach()
  if(LRVX_LZ4_TARGET)
    target_link_libraries(${LRVX} PUBLIC ${LRVX_LZ4_TARGET})
    message(STATUS "LZ4 found via find_package: ${LRVX_LZ4_TARGET}")
  else()
    # Try find_library (a system lz4 that ships no package files)
    find_library(LZ4_LIBRARY NAMES lz4)
    find_path(LZ4_INCLUDE_DIR NAMES lz4.h)
    if(LZ4_LIBRARY AND LZ4_INCLUDE_DIR)
      target_include_directories(${LRVX} PUBLIC ${LZ4_INCLUDE_DIR})
      target_link_libraries(${LRVX} PUBLIC ${LZ4_LIBRARY})
      message(STATUS "LZ4 found via find_library: ${LZ4_LIBRARY}")
    else()
      # Try pkg-config (Linux with liblz4-devel installed)
      find_package(PkgConfig QUIET)
      if(PkgConfig_FOUND)
        pkg_check_modules(LZ4 QUIET liblz4)
      endif()
      if(LZ4_FOUND)
        target_include_directories(${LRVX} PUBLIC ${LZ4_INCLUDE_DIRS})
        target_link_libraries(${LRVX} PUBLIC ${LZ4_LINK_LIBRARIES})
        message(STATUS "LZ4 found via pkg-config")
      else()
        # Last resort: vendor lz4 source and compile it directly into lrvx.
        # We avoid lz4's own cmake build because lz4_static lands in lz4's own
        # export set, conflicting with install(EXPORT lrvxTargets) below
        # (a target can belong to only one export set in CMake). Compiling
        # lz4.c into ${LRVX} sidesteps the issue and keeps lz4 symbols
        # available to compression.h consumers without a separate target.
        include(FetchContent)
        FetchContent_Declare(
          lz4_src
          GIT_REPOSITORY https://github.com/lz4/lz4.git
          GIT_TAG v1.10.0
          GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(lz4_src)
        if(NOT lz4_src_POPULATED)
          FetchContent_Populate(lz4_src)
        endif()
        target_sources(${LRVX} PRIVATE ${lz4_src_SOURCE_DIR}/lib/lz4.c)
        target_include_directories(${LRVX} PUBLIC
          $<BUILD_INTERFACE:${lz4_src_SOURCE_DIR}/lib>
        )
        # Install vendored lz4 header so installed lrvx consumers can include it.
        install(FILES ${lz4_src_SOURCE_DIR}/lib/lz4.h DESTINATION include)
        message(STATUS "LZ4 vendored from sources (compiled into lrvx)")
      endif()
    endif()
  endif()
  target_compile_definitions(${LRVX} PUBLIC LRVX_LZ4_ENABLED=1)
  message(STATUS "LZ4 compression enabled")
else()
  target_compile_definitions(${LRVX} PUBLIC LRVX_LZ4_ENABLED=0)
  message(STATUS "LZ4 compression disabled")
endif()

option(LRVX_ENABLE_TRACY "Enable Tracy profiler" OFF)
if (LRVX_ENABLE_TRACY)
  include(FetchContent)
  FetchContent_Declare(
    tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG v0.10
  )

  FetchContent_MakeAvailable(tracy)

  target_link_libraries(${LRVX} PUBLIC Tracy::TracyClient)
  target_compile_definitions(${LRVX} PUBLIC LRVX_ENABLE_TRACY=1)
endif()

# ──────────────────────────────────────────────────────────────────────
# Build artefacts (LRVX_BUILD_*)
#
# Naming convention: LRVX_BUILD_* gates optional build outputs (binding
# wheels, demo binary, tools, tests, benchmarks, connectors); LRVX_ENABLE_*
# gates capabilities of the core library (backtest module, LZ4, Tracy,
# CPU affinity).
# ──────────────────────────────────────────────────────────────────────

option(LRVX_BUILD_BENCHMARKS "Build benchmarks" OFF)
if (LRVX_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()


option(LRVX_BUILD_TESTS "Build tests" OFF)
if (LRVX_BUILD_TESTS)
  enable_testing()
  # tests/ is the core suite: it drives the backtest module, the replay
  # pipeline and the indicator graph, none of which the lite profile builds.
  # The venue's own suite registers from venue/CMakeLists.txt further down,
  # and that is what the lite profile runs.
  if(NOT LRVX_VENUE_LITE)
    add_subdirectory(tests)
  endif()
endif()

# Venue module (option declared above, next to the backtest implication).
if (LRVX_BUILD_VENUE)
  add_subdirectory(venue)
endif()

option(LRVX_BUILD_DEMO "Build demo application" OFF)
if (LRVX_BUILD_DEMO)
  add_subdirectory(demo)
endif()

# TLS for the FIX initiator. Off by default, and that is the point: the core
# links no OpenSSL, so a strategy that speaks plain FIX -- or no FIX at all --
# does not acquire the dependency. Turning it on finds OpenSSL and defines the
# macro the channel header refuses to compile without.
#
# The alternative placement was connectors/, which already links OpenSSL and
# also requires ZLIB and CURL and fetches ixwebsocket and simdjson. One
# optional dependency behind one flag is cheaper for whoever is writing the
# strategy, which is the only cost that matters here.
option(LRVX_FIX_TLS "Build the FIX initiator's TLS channel (needs OpenSSL)" OFF)
if(LRVX_FIX_TLS)
  find_package(OpenSSL REQUIRED)
  target_link_libraries(${LRVX} PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  target_compile_definitions(${LRVX} PUBLIC LRVX_FIX_TLS=1)
  message(STATUS "LRVX_FIX_TLS: the FIX initiator can speak TLS")
endif()

option(LRVX_BUILD_TOOLS "Build command-line tools" OFF)
if (LRVX_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

option(LRVX_BUILD_PYTHON "Build Python bindings" OFF)
if (LRVX_BUILD_PYTHON)
  add_subdirectory(python)
endif()

# LRVX_BUILD_NODE is an explicit flag for parity with the other
# binding artefacts. The Node addon is built out-of-tree by
# `npm run build` — this flag exists for documentation completeness
# and so the CI matrix can reference it uniformly. CMake itself does
# not invoke npm.
option(LRVX_BUILD_NODE "Build Node.js bindings (out-of-tree via npm)" OFF)

option(LRVX_BUILD_CAPI "Build C API shared library" OFF)
if (LRVX_BUILD_CAPI)
  # Static lib must be PIC to link into the shared CAPI lib
  set_target_properties(${LRVX} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  # The C API exposes the full lrvx surface — including the
  # backtest module — so the shared library has to link against
  # backtest_*.cpp. Auto-enable BACKTEST when it isn't already on.
  if(NOT LRVX_ENABLE_BACKTEST)
    message(STATUS "LRVX_BUILD_CAPI=ON implies LRVX_ENABLE_BACKTEST=ON; enabling.")
    set(LRVX_ENABLE_BACKTEST ON CACHE BOOL "Enable backtest module" FORCE)
    file(GLOB BACKTEST_SRC_FILES src/backtest/*.cpp)
    target_sources(${LRVX} PRIVATE ${BACKTEST_SRC_FILES})
  endif()
  add_subdirectory(src/capi)
endif()

option(LRVX_BUILD_CODON "Build Codon strategy support" OFF)
if (LRVX_BUILD_CODON)
  if (NOT LRVX_BUILD_CAPI)
    message(FATAL_ERROR "LRVX_BUILD_CODON requires LRVX_BUILD_CAPI=ON")
  endif()
  add_subdirectory(codon)
endif()

option(LRVX_BUILD_QUICKJS "Build QuickJS JavaScript strategy support" OFF)
if (LRVX_BUILD_QUICKJS)
  if (NOT LRVX_BUILD_CAPI)
    message(FATAL_ERROR "LRVX_BUILD_QUICKJS requires LRVX_BUILD_CAPI=ON")
  endif()
  add_subdirectory(src/quickjs)
endif()

# Native exchange connectors (Bybit, Bitget, Hyperliquid, Polymarket).
# OFF by default so a backtest-only or research build doesn't pay the
# dependency cost (OpenSSL/CURL/zlib + ixwebsocket/simdjson via
# FetchContent). See connectors/CMakeLists.txt for the per-connector
# wiring.
option(LRVX_BUILD_CONNECTORS "Build native exchange connectors module" OFF)

# Subset of connectors to build when LRVX_BUILD_CONNECTORS=ON. Empty
# string (the default) means "all venues currently in connectors/src/".
# Validation lives in connectors/CMakeLists.txt; a configure-time
# error fires when an unknown name is requested.
set(LRVX_CONNECTORS "" CACHE STRING
    "Semicolon-separated list of venues to build under \
connectors/src/<name>/; empty = all available")

if (LRVX_BUILD_CONNECTORS)
  add_subdirectory(connectors)
endif()

target_include_directories(lrvx PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

install(TARGETS lrvx
  EXPORT lrvxTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

# lrvx/ml/onnx_inference.h includes <onnxruntime_cxx_api.h> unconditionally,
# and LRVX_ENABLE_ONNX is off by default. Shipping it in a build without the
# dependency hands a consumer a header that cannot compile, with nothing to
# say why -- so it is not shipped in that build. Same rule as the venue's TLS
# gateway; scripts/check_installed_headers_declared.py is what asks.
#
# The lite profile installs the transitive closure of venue/lite_surface.txt
# and nothing else -- LRVX_LITE_CLOSURE, computed once near the top of this
# file (next to LRVX_VENUE_LITE) rather than read a second time here, so the
# installed surface, the direction gate and the core .cpp filter above all
# come from the same configure-time call.
if(LRVX_VENUE_LITE)
  message(STATUS "LRVX_VENUE_LITE: installing ${_lrvx_lite_closure_n} headers, the closure of venue/lite_surface.txt")
  foreach(_h ${LRVX_LITE_CLOSURE})
    # include/lrvx/util/crc32.h and venue/include/lrvx-venue/journal.h both
    # install under include/, at the path after their own include root.
    string(REGEX REPLACE "^(venue/)?include/" "" _rel "${_h}")
    get_filename_component(_dir "${_rel}" DIRECTORY)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${_h}" DESTINATION "include/${_dir}")
  endforeach()
elseif(LRVX_ENABLE_ONNX)
  install(DIRECTORY include/ DESTINATION include)
else()
  install(DIRECTORY include/ DESTINATION include PATTERN "onnx_inference.h" EXCLUDE)
endif()

install(EXPORT lrvxTargets
  FILE lrvxTargets.cmake
  NAMESPACE lrvx::
  DESTINATION lib/cmake/lrvx
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/lrvxConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/lrvxConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/lrvxConfig.cmake"
  INSTALL_DESTINATION lib/cmake/lrvx
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/lrvxConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/lrvxConfigVersion.cmake"
  DESTINATION lib/cmake/lrvx
)
