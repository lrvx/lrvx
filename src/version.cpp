/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/version.h"

#ifndef LRVX_PROJECT_VERSION
#error "LRVX_PROJECT_VERSION is set by the root CMakeLists.txt from project(lrvx VERSION ...)"
#endif

namespace lrvx
{

const char* projectVersion() noexcept
{
  static constexpr char kVersion[] = LRVX_PROJECT_VERSION;
  return kVersion;
}

}  // namespace lrvx
