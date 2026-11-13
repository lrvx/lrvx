/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// What the validators say when they cannot read their input: the issue names
// the file or the directory.

#include "lrvx/replay/ops/validator.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace lrvx::replay;

namespace
{

std::filesystem::path scratchDir(const std::string& tag)
{
  auto dir = std::filesystem::temp_directory_path() /
             ("lrvx_validator_messages_" + tag + "_" + std::to_string(std::random_device{}()));
  std::filesystem::create_directories(dir);
  return dir;
}

const ValidationIssue* find(const std::vector<ValidationIssue>& issues, IssueType type)
{
  for (const auto& issue : issues)
  {
    if (issue.type == type)
    {
      return &issue;
    }
  }
  return nullptr;
}

}  // namespace

TEST(ValidatorMessages, AFileThatCannotBeOpenedIsNamed)
{
  const auto dir = scratchDir("locked");
  const auto path = dir / "locked.lrvx";
  std::ofstream(path, std::ios::binary) << std::string(256, 'x');
  std::filesystem::permissions(path, std::filesystem::perms::none);
  if (std::FILE* f = std::fopen(path.string().c_str(), "rb"))
  {
    std::fclose(f);
    std::filesystem::remove_all(dir);
    GTEST_SKIP() << "this user can read a file with no permission bits";
  }

  const auto result = SegmentValidator().validate(path);
  std::filesystem::permissions(path, std::filesystem::perms::owner_all);
  std::filesystem::remove_all(dir);

  EXPECT_FALSE(result.valid);
  const ValidationIssue* issue = find(result.issues, IssueType::FileReadError);
  ASSERT_NE(issue, nullptr);
  EXPECT_EQ(issue->message, "Cannot open file for reading: " + path.string());
}

TEST(ValidatorMessages, AMissingDatasetDirectoryIsAnIssueThatNamesIt)
{
  const auto dir = scratchDir("missing");
  const auto missing = dir / "no_such_directory";

  const auto result = DatasetValidator().validate(missing);
  EXPECT_FALSE(result.valid);
  EXPECT_EQ(result.total_segments, 0u);
  EXPECT_GE(result.total_errors, 1u);
  const ValidationIssue* issue = find(result.issues, IssueType::FileNotFound);
  ASSERT_NE(issue, nullptr);
  EXPECT_EQ(issue->severity, IssueSeverity::Critical);
  EXPECT_EQ(issue->message, "Directory not found: " + missing.string());
  std::filesystem::remove_all(dir);
}

// A path that is a file used to throw std::filesystem::filesystem_error out
// of validate(), which is documented to return a result.
TEST(ValidatorMessages, ADatasetPathThatIsNotADirectoryIsAnIssueNotAnException)
{
  const auto dir = scratchDir("file");
  const auto file = dir / "a_file";
  std::ofstream(file) << "not a directory";

  DatasetValidationResult result;
  ASSERT_NO_THROW(result = DatasetValidator().validate(file));
  EXPECT_FALSE(result.valid);
  const ValidationIssue* issue = find(result.issues, IssueType::FileReadError);
  ASSERT_NE(issue, nullptr);
  EXPECT_NE(issue->message.find("Cannot read directory: " + file.string()), std::string::npos)
      << issue->message;
  std::filesystem::remove_all(dir);
}

// repair() said "File not found" and nothing else; validate() names the file
// in the same words.
TEST(ValidatorMessages, RepairNamesTheFileItCannotFind)
{
  const auto dir = scratchDir("repair");
  const auto missing = dir / "no_such_segment.lrvx";

  const RepairResult direct = SegmentRepairer().repair(missing);
  EXPECT_FALSE(direct.success);
  ASSERT_EQ(direct.errors.size(), 1u);
  EXPECT_EQ(direct.errors.front(), "File not found: " + missing.string());

  SegmentValidationResult validation;
  const RepairResult given = SegmentRepairer().repair(missing, validation);
  ASSERT_EQ(given.errors.size(), 1u);
  EXPECT_EQ(given.errors.front(), "File not found: " + missing.string());
  std::filesystem::remove_all(dir);
}

// The repair's other errors named nothing: "Cannot read header" for one
// segment of a directory left the caller to guess which.
TEST(ValidatorMessages, RepairNamesTheFileInEveryError)
{
  const auto dir = std::filesystem::temp_directory_path() / "lrvx_repair_names_the_file";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  // A count to fix makes the repair open the file and read its header.
  SegmentValidationResult validation;
  validation.reported_event_count = 1;
  validation.actual_event_count = 2;

  const auto expectOneErrorNaming =
      [](const RepairResult& r, const std::string& what, const std::filesystem::path& file)
  {
    ASSERT_EQ(r.errors.size(), 1u);
    EXPECT_EQ(r.errors.front().rfind(what, 0), 0u) << r.errors.front();
    EXPECT_NE(r.errors.front().find(file.string()), std::string::npos) << r.errors.front();
  };

  RepairConfig inPlace;
  inPlace.backup_before_repair = false;

  // Too short to hold a header.
  const auto empty = dir / "empty.lrvx";
  std::ofstream(empty, std::ios::binary).close();
  expectOneErrorNaming(SegmentRepairer(inPlace).repair(empty, validation), "Cannot read header",
                       empty);

  // Not something that opens for writing.
  const auto notAFile = dir / "directory.lrvx";
  std::filesystem::create_directories(notAFile);
  expectOneErrorNaming(SegmentRepairer(inPlace).repair(notAFile, validation),
                       "Cannot open file for writing", notAFile);

  // The backup's name is taken by a directory.
  const auto segment = dir / "segment.lrvx";
  std::ofstream(segment, std::ios::binary) << "not a segment";
  std::filesystem::create_directories(dir / "segment.lrvx.backup");
  const RepairResult backedUp = SegmentRepairer().repair(segment, validation);
  expectOneErrorNaming(backedUp, "Failed to create backup", segment);
  EXPECT_NE(backedUp.errors.front().find("segment.lrvx.backup"), std::string::npos)
      << backedUp.errors.front();

  std::filesystem::remove_all(dir);
}
