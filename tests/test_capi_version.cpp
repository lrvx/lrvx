/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// lrvx_version() reports the release the library was built from, the
// `project(lrvx VERSION ...)` of the tree, so a binding can expose it
// (Python lrvx.__version__ and lrvx.version(), Node lrvx.version(), Codon
// lrvx.abi.version(), QuickJS lrvx.version()) without a copy of the number
// of its own. It is a separate thing from lrvx_capi_abi_version(), which
// moves only when the C boundary changes shape.

#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>

#include <regex>
#include <string>

TEST(CapiVersion, ReportsTheProjectVersion)
{
  const char* v = lrvx_version();
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(std::string(v), std::string(LRVX_EXPECTED_VERSION));
}

TEST(CapiVersion, IsAStableSemverString)
{
  const std::string v = lrvx_version();
  EXPECT_TRUE(std::regex_match(v, std::regex(R"(\d+\.\d+\.\d+([.-][A-Za-z0-9.]+)?)"))) << v;
  // The same pointer every call: a binding may hold on to it.
  EXPECT_EQ(lrvx_version(), lrvx_version());
}
