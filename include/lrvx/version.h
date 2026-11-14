/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

namespace lrvx
{

// The release this library was built from: `project(lrvx VERSION ...)` in the
// root CMakeLists.txt, which scripts/set-version.sh keeps in step with
// pyproject.toml and node/package.json. A static string; the pointer is the
// same on every call.
const char* projectVersion() noexcept;

}  // namespace lrvx
