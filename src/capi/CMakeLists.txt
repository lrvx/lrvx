add_library(lrvx_capi SHARED lrvx_capi.cpp)

target_link_libraries(lrvx_capi PRIVATE lrvx)
target_include_directories(lrvx_capi PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

# Without VERSION/SOVERSION the shared library carries compatibility version
# 0.0.0, so neither the macOS nor the ELF loader can refuse a mismatched
# build -- the one check that happens before any of our code runs.
set_target_properties(lrvx_capi PROPERTIES
  C_VISIBILITY_PRESET default
  CXX_VISIBILITY_PRESET default
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
)

if(NOT MSVC)
  # LRVX_ARCH_OPT comes from the top-level CMakeLists.txt and honours
  # the LRVX_NATIVE option; the shared CAPI lib is a distribution target
  # (Codon/QuickJS/Node load it at runtime), so set LRVX_NATIVE=OFF when
  # shipping it to heterogeneous CPUs.
  set(_LRVX_CAPI_OPTS -O3)
  if(LRVX_LTO)
    list(APPEND _LRVX_CAPI_OPTS -flto)
  endif()
  if(LRVX_ARCH_OPT)
    list(APPEND _LRVX_CAPI_OPTS ${LRVX_ARCH_OPT})
  endif()
  target_compile_options(lrvx_capi PRIVATE
    $<$<CONFIG:Release>:${_LRVX_CAPI_OPTS}>
  )
endif()

install(TARGETS lrvx_capi
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
install(FILES ${CMAKE_SOURCE_DIR}/include/lrvx/capi/lrvx_capi.h
  DESTINATION include/lrvx/capi
)
