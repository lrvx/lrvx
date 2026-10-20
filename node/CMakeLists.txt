cmake_minimum_required(VERSION 3.15)
project(lrvx_node)

set(CMAKE_CXX_STANDARD 23)

# node-addon-api
execute_process(
  COMMAND node -p "require('node-addon-api').include_dir"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE NAPI_INCLUDE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE)

# Node.js headers (provided by cmake-js)
include_directories(${CMAKE_JS_INC} ${NAPI_INCLUDE_DIR})

# lrvx headers + capi
include_directories(${CMAKE_SOURCE_DIR}/../include)
include_directories(${CMAKE_SOURCE_DIR}/../src/capi)

# Find the built lrvx static library (Release/ subdir for MSVC multi-config generators)
find_library(LRVX_LIB lrvx HINTS
  ${CMAKE_SOURCE_DIR}/../build
  ${CMAKE_SOURCE_DIR}/../build/Release)

# The addon compiles its own translation units and links a prebuilt archive, so
# the two halves must agree on NDEBUG. libc++ changes container layout and
# hardening between debug and release, and a mismatch does not fail to link: it
# corrupts silently. Observed symptoms of a Release addon against a Debug
# archive were SymbolRegistry::registerSymbol returning 0 for every symbol,
# malloc aborting with free_list_checksum_botch, and segfaults at exit.
#
# cmake-js builds Release by default. Read the core build's type out of its
# CMakeCache and refuse to proceed on a mismatch, naming the fix.
#
# Multi-config generators (Visual Studio, Ninja Multi-Config) do not use
# CMAKE_BUILD_TYPE at all -- the configuration is chosen at build time with
# --config, so the cache entry is empty even when the caller passed
# -DCMAKE_BUILD_TYPE=Release. They are detected by CMAKE_CONFIGURATION_TYPES,
# and the built configuration is recoverable from where the archive landed:
# build/Release/lrvx.lib rather than build/lrvx.lib. Demanding
# CMAKE_BUILD_TYPE from those generators broke the Windows leg of
# node-publish.yml and blocked a release.
foreach(_lrvx_core_dir "${CMAKE_SOURCE_DIR}/../build" "${CMAKE_SOURCE_DIR}/../build/Release")
  if(EXISTS "${_lrvx_core_dir}/CMakeCache.txt")
    file(STRINGS "${_lrvx_core_dir}/CMakeCache.txt" _lrvx_core_build_type
         REGEX "^CMAKE_BUILD_TYPE:STRING=")
    string(REPLACE "CMAKE_BUILD_TYPE:STRING=" "" _lrvx_core_build_type "${_lrvx_core_build_type}")
    file(STRINGS "${_lrvx_core_dir}/CMakeCache.txt" _lrvx_core_config_types
         REGEX "^CMAKE_CONFIGURATION_TYPES:STRING=")
    break()
  endif()
endforeach()

if(NOT _lrvx_core_build_type AND _lrvx_core_config_types)
  # Multi-config: recover the configuration from the archive's directory.
  get_filename_component(_lrvx_lib_dir "${LRVX_LIB}" DIRECTORY)
  get_filename_component(_lrvx_lib_leaf "${_lrvx_lib_dir}" NAME)
  if(_lrvx_lib_leaf MATCHES "^(Debug|Release|RelWithDebInfo|MinSizeRel)$")
    set(_lrvx_core_build_type "${_lrvx_lib_leaf}")
    message(STATUS
      "lrvx core built by a multi-config generator; taking configuration "
      "'${_lrvx_core_build_type}' from ${LRVX_LIB}")
  else()
    message(FATAL_ERROR
      "The lrvx core at ${LRVX_LIB} was built by a multi-config generator, "
      "but its configuration cannot be determined: the archive is not under a "
      "Debug/ Release/ RelWithDebInfo/ or MinSizeRel/ directory. Build the "
      "core with `cmake --build build --config Release` so the layout names "
      "the configuration.")
  endif()
endif()

if(NOT _lrvx_core_build_type)
  message(FATAL_ERROR
    "Could not read CMAKE_BUILD_TYPE from the lrvx core build next to "
    "${LRVX_LIB}, and it was not built by a multi-config generator either. An "
    "unset build type means no NDEBUG, which turns on LRVX_SCALE_CHECKS and "
    "changes sizeof(Price) from 8 to 16 — linking that archive into this addon "
    "corrupts memory silently. Reconfigure the core with an explicit "
    "-DCMAKE_BUILD_TYPE=Release.")
endif()

if(_lrvx_core_build_type)
  set(_lrvx_node_build_type "${CMAKE_BUILD_TYPE}")
  if(NOT _lrvx_node_build_type)
    set(_lrvx_node_build_type "Release")
  endif()
  string(TOUPPER "${_lrvx_core_build_type}" _core_upper)
  string(TOUPPER "${_lrvx_node_build_type}" _node_upper)
  # Debug is the only type without NDEBUG; Release / RelWithDebInfo /
  # MinSizeRel all define it, so they are interchangeable here.
  set(_core_ndebug ON)
  set(_node_ndebug ON)
  if(_core_upper STREQUAL "DEBUG")
    set(_core_ndebug OFF)
  endif()
  if(_node_upper STREQUAL "DEBUG")
    set(_node_ndebug OFF)
  endif()
  if(NOT _core_ndebug STREQUAL _node_ndebug)
    message(FATAL_ERROR
      "lrvx_node build type (${_lrvx_node_build_type}) disagrees with the lrvx "
      "archive it links (${_lrvx_core_build_type}). Mixing NDEBUG and non-NDEBUG "
      "halves corrupts memory instead of failing to link. Rebuild the core with "
      "-DCMAKE_BUILD_TYPE=${_lrvx_node_build_type}, or build the addon with "
      "npm run build -- --debug to match a Debug core.")
  endif()
endif()

# Compile lrvx_capi.cpp directly to avoid DLL/import-lib complexity on Windows
add_library(lrvx_node SHARED
  src/lrvx_node.cpp
  ${CMAKE_SOURCE_DIR}/../src/capi/lrvx_capi.cpp
  ${CMAKE_JS_SRC})
set_target_properties(lrvx_node PROPERTIES PREFIX "" SUFFIX ".node")
# Match the runtime library used by lrvx.lib (/MD) — cmake-js defaults to /MT
if(MSVC)
  set_property(TARGET lrvx_node PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()
target_link_libraries(lrvx_node ${CMAKE_JS_LIB} ${LRVX_LIB})
# NODE_ADDON_API_CPP_EXCEPTIONS_ALL widens node-addon-api's callback wrapper
# (node_modules/node-addon-api/napi-inl.h) from catching only Napi::Error to
# also catching std::exception and (...), converting them into JS errors.
# Without it a C++ exception thrown by lrvx core — lrvx::Strategy's constructor
# throws std::invalid_argument for a SymbolId missing from the registry —
# escapes into V8's C frame and calls std::terminate, killing the process
# instead of raising something JS can catch. That path is reachable from four
# addStrategy/setStrategy entry points, and only 14 of 577 NAPI methods carry a
# hand-written try/catch. tryLrvx stays as the enrichment layer that preserves
# LrvxError's code and helpUrl; this is the safety net underneath it.
target_compile_definitions(lrvx_node PRIVATE NAPI_VERSION=8 NODE_ADDON_API_DISABLE_DEPRECATED
                                             NODE_ADDON_API_CPP_EXCEPTIONS_ALL)

# lrvx headers are feature-gated on macros the core build sets PUBLIC on the
# `lrvx` target. find_library() gives us the archive but none of those
# definitions, so lrvx_capi.cpp compiled here can disagree with the archive it
# links: LRVX_LZ4_ENABLED changes whether <lz4.h> is included and whether
# compression reports as supported, which is enough to read a tape written by
# the other half and land on corrupted state. Set them explicitly.
#
# The affinity pair is guarded because LRVX_CPU_AFFINITY_ENABLED also decides
# whether EventBus declares two data members, i.e. it changes sizeof.
# LRVX_ENABLE_CPU_AFFINITY defaults OFF in the core build; if you turn it on
# there, pass -DLRVX_NODE_CPU_AFFINITY=ON here so both halves agree.
option(LRVX_NODE_CPU_AFFINITY "Match a core build configured with LRVX_ENABLE_CPU_AFFINITY=ON" OFF)
if(LRVX_NODE_CPU_AFFINITY)
  target_compile_definitions(lrvx_node PRIVATE LRVX_CPU_AFFINITY_ENABLED=1)
else()
  target_compile_definitions(lrvx_node PRIVATE LRVX_CPU_AFFINITY_ENABLED=0)
endif()
target_compile_definitions(lrvx_node PRIVATE LRVX_NUMA_LIBRARY_LINKED=0)

# Opt-in sanitizer build of the addon's own translation units.
#
# cmake-js assembles CMAKE_CXX_FLAGS itself, so -DCMAKE_CXX_FLAGS on the
# command line is silently dropped and the addon compiles uninstrumented while
# the archive next to it is instrumented -- a configuration that loads, runs,
# and reports nothing, which reads exactly like a clean run. Attaching the
# flags to the target instead is what actually takes effect.
#
#   cmake --build build-tsan            # lrvx core with the same sanitizer
#   npx cmake-js compile --out build-tsan \
#     --CDLRVX_NODE_SANITIZE=thread \
#     --CDLRVX_LIB=<repo>/build-tsan/liblrvx.a
#
# The archive has to carry the same sanitizer as the addon; mixing an
# instrumented half with an uninstrumented one gives partial shadow state and
# under-reports.
set(LRVX_NODE_SANITIZE "" CACHE STRING
    "Sanitizer to build the addon with: thread, address, or undefined")
if(LRVX_NODE_SANITIZE)
  message(STATUS "lrvx_node: building with -fsanitize=${LRVX_NODE_SANITIZE}")
  target_compile_options(lrvx_node PRIVATE
                         -fsanitize=${LRVX_NODE_SANITIZE} -g -fno-omit-frame-pointer)
  target_link_options(lrvx_node PRIVATE -fsanitize=${LRVX_NODE_SANITIZE})
endif()

# Mirror the LZ4 detection from the core CMakeLists. find_library(LRVX_LIB)
# locates a static archive but does not propagate the PUBLIC dependencies of
# the original `lrvx` target, so the binding has to re-discover lz4 itself.
# Try the three discovery methods used in the core build (vcpkg, brew/Linux
# system, pkg-config) and link whichever one matches.
set(LRVX_NODE_LZ4_FOUND OFF)
# The config's target name follows the packager (lz4::lz4 from vcpkg; LZ4::lz4
# over the _shared/_static pair from lz4's own CMake build, which is what
# Homebrew installs), so it is picked by existence, as the core build does.
find_package(lz4 CONFIG QUIET)
set(LRVX_NODE_LZ4_TARGET "")
foreach(_lrvx_lz4_candidate lz4::lz4 LZ4::lz4 LZ4::lz4_shared LZ4::lz4_static)
  if(TARGET ${_lrvx_lz4_candidate})
    set(LRVX_NODE_LZ4_TARGET ${_lrvx_lz4_candidate})
    break()
  endif()
endforeach()
if(LRVX_NODE_LZ4_TARGET)
  target_link_libraries(lrvx_node ${LRVX_NODE_LZ4_TARGET})
  set(LRVX_NODE_LZ4_FOUND ON)
  message(STATUS "lrvx_node: LZ4 via find_package: ${LRVX_NODE_LZ4_TARGET}")
else()
  find_library(LZ4_LIBRARY NAMES lz4)
  find_path(LZ4_INCLUDE_DIR NAMES lz4.h)
  if(LZ4_LIBRARY AND LZ4_INCLUDE_DIR)
    target_include_directories(lrvx_node PRIVATE ${LZ4_INCLUDE_DIR})
    target_link_libraries(lrvx_node ${LZ4_LIBRARY})
    set(LRVX_NODE_LZ4_FOUND ON)
    message(STATUS "lrvx_node: LZ4 via find_library: ${LZ4_LIBRARY}")
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(LZ4 QUIET liblz4)
    endif()
    if(LZ4_FOUND)
      target_include_directories(lrvx_node PRIVATE ${LZ4_INCLUDE_DIRS})
      target_link_libraries(lrvx_node ${LZ4_LINK_LIBRARIES})
      set(LRVX_NODE_LZ4_FOUND ON)
      message(STATUS "lrvx_node: LZ4 via pkg-config")
    else()
      message(STATUS "lrvx_node: LZ4 not found — compressed segment reads will fail at runtime")
    endif()
  endif()
endif()

# Must match the value the linked archive was compiled with.
if(LRVX_NODE_LZ4_FOUND)
  target_compile_definitions(lrvx_node PRIVATE LRVX_LZ4_ENABLED=1)
else()
  target_compile_definitions(lrvx_node PRIVATE LRVX_LZ4_ENABLED=0)
endif()
