find_package(pybind11 REQUIRED)

# pybind11_add_module links pybind11::lto (-flto=auto on GCC) into the
# module unless CMAKE_INTERPROCEDURAL_OPTIMIZATION is defined, whatever
# the build type. Follow LRVX_LTO: with it off, define the variable so
# the module is built the way the library it links was.
if(NOT LRVX_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
endif()

# Compiled module name is `_lrvx`; the user-facing import path
# `lrvx` is provided by the pure-Python wrapper in lrvx/__init__.py
# that re-exports everything. This split lets us ship .pyi stubs and a
# PEP 561 `py.typed` marker alongside the .so in one importable package.
pybind11_add_module(_lrvx lrvx.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/capi/lrvx_capi.cpp)

target_link_libraries(_lrvx PRIVATE lrvx)
target_include_directories(_lrvx PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/capi
)

if(NOT MSVC)
  # LRVX_ARCH_OPT is computed in the top-level CMakeLists.txt and
  # honours the LRVX_NATIVE option. Wheel builds set LRVX_NATIVE=OFF via
  # python/pyproject.toml so the shipped .so does not require AVX-512
  # (or whatever ISA the build host happened to expose).
  set(_LRVX_PY_OPTS -O3)
  if(LRVX_LTO)
    list(APPEND _LRVX_PY_OPTS -flto)
  endif()
  if(LRVX_ARCH_OPT)
    list(APPEND _LRVX_PY_OPTS ${LRVX_ARCH_OPT})
  endif()
  target_compile_options(_lrvx PRIVATE
    $<$<CONFIG:Release>:${_LRVX_PY_OPTS}>
  )
endif()

# Place the compiled .so inside the package directory in the build tree
# so `PYTHONPATH=build/python python -c "import lrvx"` works during
# local development and CI without an install step.
set_target_properties(_lrvx PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lrvx
)

# Mirror the source-side package layout (pure-Python wrapper, py.typed
# marker, generated stubs) into the build tree alongside the .so. The
# wrapper stub `lrvx/__init__.pyi` lives at the package root; the
# C-extension stubs sit under `lrvx/_lrvx/` because that is where
# type checkers resolve `lrvx._lrvx.X`. Stubs are copied only if
# present so the very first build — which produces the .so that stubgen
# needs as input — is not blocked on them existing.
#
# The .py file list is globbed from `lrvx/*.py` so a new module
# automatically lands in the wheel without touching this file. CMake
# re-runs the configure step when this CMakeLists.txt changes; if you
# add a new .py module, run `cmake -B build -S .` once to pick it up.
file(GLOB LRVX_PY_PURE_PYTHON_MODULES
     CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/lrvx/*.py")
set(LRVX_PY_REQUIRED_FILES
  ${LRVX_PY_PURE_PYTHON_MODULES}
  ${CMAKE_CURRENT_SOURCE_DIR}/lrvx/py.typed
)
set(LRVX_PY_OPTIONAL_ROOT_STUBS
  ${CMAKE_CURRENT_SOURCE_DIR}/lrvx/__init__.pyi
)
set(LRVX_PY_OPTIONAL_EXT_STUBS
  ${CMAKE_CURRENT_SOURCE_DIR}/lrvx/_lrvx/__init__.pyi
  ${CMAKE_CURRENT_SOURCE_DIR}/lrvx/_lrvx/targets.pyi
  ${CMAKE_CURRENT_SOURCE_DIR}/lrvx/_lrvx/_heatmap.pyi
)
add_custom_command(TARGET _lrvx POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/lrvx
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/lrvx/_lrvx
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${LRVX_PY_REQUIRED_FILES}
    ${CMAKE_CURRENT_BINARY_DIR}/lrvx/
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/lrvx/templates
    ${CMAKE_CURRENT_BINARY_DIR}/lrvx/templates
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/lrvx/archives
    ${CMAKE_CURRENT_BINARY_DIR}/lrvx/archives
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/lrvx/dex
    ${CMAKE_CURRENT_BINARY_DIR}/lrvx/dex
)
# The POST_BUILD copies above run only when the extension relinks. The
# sync target below depends on the source files themselves, so editing a
# .py, a template or a stub refreshes build/python on the next build
# without touching the extension.
file(GLOB_RECURSE LRVX_PY_SYNC_INPUTS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/lrvx/*.py"
     "${CMAKE_CURRENT_SOURCE_DIR}/lrvx/*.pyi"
     "${CMAKE_CURRENT_SOURCE_DIR}/lrvx/py.typed"
     "${CMAKE_CURRENT_SOURCE_DIR}/lrvx/templates/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/lrvx/archives/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/lrvx/dex/*")
set(LRVX_PY_SYNC_STAMP ${CMAKE_CURRENT_BINARY_DIR}/lrvx_py_sync.stamp)
add_custom_command(OUTPUT ${LRVX_PY_SYNC_STAMP}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/lrvx/_lrvx
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${LRVX_PY_REQUIRED_FILES}
    ${CMAKE_CURRENT_BINARY_DIR}/lrvx/
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/lrvx/templates
    ${CMAKE_CURRENT_BINARY_DIR}/lrvx/templates
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/lrvx/archives
    ${CMAKE_CURRENT_BINARY_DIR}/lrvx/archives
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/lrvx/dex
    ${CMAKE_CURRENT_BINARY_DIR}/lrvx/dex
  COMMAND ${CMAKE_COMMAND} -E touch ${LRVX_PY_SYNC_STAMP}
  DEPENDS ${LRVX_PY_SYNC_INPUTS}
  COMMENT "Syncing python/lrvx into ${CMAKE_CURRENT_BINARY_DIR}"
  VERBATIM)
foreach(opt_file IN LISTS LRVX_PY_OPTIONAL_ROOT_STUBS)
  if(EXISTS ${opt_file})
    set_property(SOURCE ${LRVX_PY_SYNC_STAMP} APPEND PROPERTY OBJECT_DEPENDS ${opt_file})
  endif()
endforeach()
add_custom_target(lrvx_py_sync ALL DEPENDS ${LRVX_PY_SYNC_STAMP})
add_dependencies(lrvx_py_sync _lrvx)

foreach(opt_file IN LISTS LRVX_PY_OPTIONAL_ROOT_STUBS)
  if(EXISTS ${opt_file})
    add_custom_command(TARGET _lrvx POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${opt_file}
        ${CMAKE_CURRENT_BINARY_DIR}/lrvx/
    )
  endif()
endforeach()
foreach(opt_file IN LISTS LRVX_PY_OPTIONAL_EXT_STUBS)
  if(EXISTS ${opt_file})
    add_custom_command(TARGET _lrvx POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${opt_file}
        ${CMAKE_CURRENT_BINARY_DIR}/lrvx/_lrvx/
    )
  endif()
endforeach()

# A separate ALL target keeps the build-tree stubs in sync with source
# even when the .so itself does not need rebuilding (editing only the
# stubs after `cmake --build` would otherwise leave stale .pyi in the
# build tree, causing CI's `gen_pyi_stubs.py --check` to compare against
# an outdated copy).
set(LRVX_PY_BUILD_STUB_TARGETS "")
foreach(opt_file IN LISTS LRVX_PY_OPTIONAL_ROOT_STUBS)
  if(EXISTS ${opt_file})
    get_filename_component(basename ${opt_file} NAME)
    set(dst ${CMAKE_CURRENT_BINARY_DIR}/lrvx/${basename})
    add_custom_command(OUTPUT ${dst}
      COMMAND ${CMAKE_COMMAND} -E copy ${opt_file} ${dst}
      DEPENDS ${opt_file}
    )
    list(APPEND LRVX_PY_BUILD_STUB_TARGETS ${dst})
  endif()
endforeach()
foreach(opt_file IN LISTS LRVX_PY_OPTIONAL_EXT_STUBS)
  if(EXISTS ${opt_file})
    get_filename_component(basename ${opt_file} NAME)
    set(dst ${CMAKE_CURRENT_BINARY_DIR}/lrvx/_lrvx/${basename})
    add_custom_command(OUTPUT ${dst}
      COMMAND ${CMAKE_COMMAND} -E copy ${opt_file} ${dst}
      DEPENDS ${opt_file}
    )
    list(APPEND LRVX_PY_BUILD_STUB_TARGETS ${dst})
  endif()
endforeach()
if(LRVX_PY_BUILD_STUB_TARGETS)
  add_custom_target(_lrvx_stubs ALL DEPENDS ${LRVX_PY_BUILD_STUB_TARGETS})
endif()

install(TARGETS _lrvx LIBRARY DESTINATION lrvx COMPONENT python_module)
install(FILES ${LRVX_PY_REQUIRED_FILES}
        DESTINATION lrvx COMPONENT python_module)
# Bundled `lrvx new` templates. Shipped as data alongside the package
# so `lrvx new <project>` works after `pip install lrvx`. Walked at
# runtime by lrvx.cli via importlib.resources.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lrvx/templates/
        DESTINATION lrvx/templates
        COMPONENT python_module)
foreach(opt_file IN LISTS LRVX_PY_OPTIONAL_ROOT_STUBS)
  if(EXISTS ${opt_file})
    install(FILES ${opt_file} DESTINATION lrvx COMPONENT python_module)
  endif()
endforeach()
foreach(opt_file IN LISTS LRVX_PY_OPTIONAL_EXT_STUBS)
  if(EXISTS ${opt_file})
    install(FILES ${opt_file} DESTINATION lrvx/_lrvx COMPONENT python_module)
  endif()
endforeach()
