#include "csvp/pipeline.h"
#include "csvp/parser.h"
#include "csvp/validator.h"

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>

namespace {

using namespace csvp;

// Helper to get test data path
std::string dataPath(const char* filename) {
    return std::string(TEST_DATA_DIR) + filename;
}

// Helper: create default rules for sample_utf8.csv schema
std::vector<FieldRule> makeDefaultRules() {
    return {
        {"id", FieldType::Int, true},
        {"name", FieldType::String, true},
        {"age", FieldType::Int, true},
        {"score", FieldType::Double, true},
        {"active", FieldType::Bool, false},
    };
}

// ============================================================
// Pipeline.SyncSmallClean (PASS)
// ============================================================
TEST(Pipeline, SyncSmallClean) {
    ImportPipeline::Config cfg;
    cfg.parser.delimiter = ',';
    cfg.parser.has_header = true;
    cfg.skip_bad_rows = false;

    ImportPipeline pipeline(cfg, makeDefaultRules());
    auto result = pipeline.importSync(dataPath("sample_utf8.csv").c_str());

    EXPECT_EQ(result.rows_imported, 10u);
    EXPECT_EQ(result.rows_skipped, 0u);
    EXPECT_TRUE(result.errors.empty());
    EXPECT_GT(result.elapsed_ms, 0.0);

    std::cout << "Pipeline.SyncSmallClean: imported " << result.rows_imported
              << " rows in " << result.elapsed_ms << " ms" << std::endl;
}

// ============================================================
// Pipeline.MissingFile (PASS)
// ============================================================
TEST(Pipeline, MissingFile) {
    ImportPipeline::Config cfg;
    ImportPipeline pipeline(cfg, {});
    auto result = pipeline.importSync("nonexistent_file_12345.csv");

    // Should report error, not crash
    EXPECT_EQ(result.rows_imported, 0u);
    std::cout << "Pipeline.MissingFile: errors=" << result.errors.size() << std::endl;
}

// ============================================================
// Pipeline.SkipBadRows
// ============================================================
TEST(Pipeline, SkipBadRows)
{
	ImportPipeline::Config config;
	config.parser.delimiter = ',';
	config.parser.has_header = true;
	config.skip_bad_rows = true;
	config.error_limit = 50;

	ImportPipeline pipeline(config, makeDefaultRules());
	ImportPipeline::Result result;
	EXPECT_NO_THROW(result = pipeline.importSync(dataPath("sample_dirty.csv").c_str()));

	EXPECT_EQ(result.rows_imported, 2U);
	EXPECT_EQ(result.rows_skipped, 5U);
	ASSERT_EQ(result.errors.size(), 5U);
	ASSERT_FALSE(result.errors[0].cell_errors.empty());
	EXPECT_EQ(result.errors[0].line_number, 4U);
	EXPECT_EQ(result.errors[0].cell_errors[0].code, ErrorCode::ColumnCountMismatch);
}

// ============================================================
// Pipeline.ErrorLimit (PASS)
// ============================================================
TEST(Pipeline, ErrorLimit)
{
	ImportPipeline::Config config;
	config.parser.delimiter = ',';
	config.parser.has_header = true;
	config.skip_bad_rows = true;
	config.error_limit = 3;

	ImportPipeline pipeline(config, makeDefaultRules());
	ImportPipeline::Result result;
	EXPECT_NO_THROW(result = pipeline.importSync(dataPath("sample_dirty.csv").c_str()));
	EXPECT_EQ(result.rows_imported, 2U);
	EXPECT_EQ(result.rows_skipped, 5U);
	EXPECT_EQ(result.errors.size(), config.error_limit);
}

TEST(Pipeline, NoHeaderUsesRuleCount)
{
	const auto uniqueValue = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path()
		/ ("csvp_no_header_" + std::to_string(uniqueValue) + ".csv");
	{
		std::ofstream output(path, std::ios::binary);
		ASSERT_TRUE(output.is_open());
		output << "1,Alice,true\n"
			<< "2,Bob,false\n"
			<< "bad,row\n"
			<< "3,Carol,true\n";
	}

	ImportPipeline::Config config;
	config.parser.has_header = false;
	config.skip_bad_rows = true;
	const std::vector<FieldRule> rules = {
		{"id", FieldType::Int, true},
		{"name", FieldType::String, true},
		{"active", FieldType::Bool, true},
	};
	ImportPipeline pipeline(config, rules);
	const ImportPipeline::Result result = pipeline.importSync(path.string().c_str());

	EXPECT_EQ(result.rows_imported, 3U);
	EXPECT_EQ(result.rows_skipped, 1U);
	ASSERT_EQ(result.errors.size(), 1U);
	EXPECT_EQ(result.errors[0].line_number, 3U);
	ASSERT_EQ(result.errors[0].cell_errors.size(), 1U);
	EXPECT_EQ(result.errors[0].cell_errors[0].code, ErrorCode::ColumnCountMismatch);
	std::filesystem::remove(path);
}

TEST(Pipeline, ParseErrorDoesNotBecomeEof)
{
	const auto uniqueValue = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path()
		/ ("csvp_parse_error_" + std::to_string(uniqueValue) + ".csv");
	{
		std::ofstream output(path, std::ios::binary);
		ASSERT_TRUE(output.is_open());
		output << "id,name\n"
			<< "1,Alice\n"
			<< "2,\"unclosed\n"
			<< "3,Carol\n";
	}

	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	const std::vector<FieldRule> rules = {
		{"id", FieldType::Int, true},
		{"name", FieldType::String, true},
	};
	ImportPipeline pipeline(config, rules);
	const ImportPipeline::Result result = pipeline.importSync(path.string().c_str());

	EXPECT_EQ(result.rows_imported, 2U);
	EXPECT_EQ(result.rows_skipped, 1U);
	ASSERT_EQ(result.errors.size(), 1U);
	EXPECT_EQ(result.errors[0].line_number, 3U);
	ASSERT_EQ(result.errors[0].cell_errors.size(), 1U);
	EXPECT_EQ(result.errors[0].cell_errors[0].code, ErrorCode::ParseError);
	EXPECT_NE(result.errors[0].cell_errors[0].message.find("Unclosed quote"), std::string::npos);

	config.skip_bad_rows = false;
	ImportPipeline strictPipeline(config, rules);
	EXPECT_THROW(strictPipeline.importSync(path.string().c_str()), std::runtime_error);
	std::filesystem::remove(path);
}

TEST(Pipeline, ErrorPreservesRawCsvLine)
{
	const auto uniqueValue = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path()
		/ ("csvp_raw_error_line_" + std::to_string(uniqueValue) + ".csv");
	const std::string rawLine = "not-int;\"  has; delimiter and \"\"quote\"\"  \"; maybe ";
	{
		std::ofstream output(path, std::ios::binary);
		ASSERT_TRUE(output.is_open());
		output << "id;note;active\n" << rawLine << "\n";
	}

	ImportPipeline::Config config;
	config.parser.delimiter = ';';
	config.skip_bad_rows = true;
	const std::vector<FieldRule> rules = {
		{"id", FieldType::Int, true},
		{"note", FieldType::String, true},
		{"active", FieldType::Bool, true},
	};
	ImportPipeline pipeline(config, rules);
	const ImportPipeline::Result result = pipeline.importSync(path.string().c_str());

	EXPECT_EQ(result.rows_imported, 0U);
	EXPECT_EQ(result.rows_skipped, 1U);
	ASSERT_EQ(result.errors.size(), 1U);
	EXPECT_EQ(result.errors[0].raw_line.size(), rawLine.size());
	EXPECT_EQ(result.errors[0].raw_line, rawLine);
	std::filesystem::remove(path);
}

TEST(Pipeline, RejectsHeaderRuleCountMismatch)
{
	ImportPipeline::Config config;
	std::vector<FieldRule> tooFewRules = makeDefaultRules();
	tooFewRules.pop_back();
	ImportPipeline tooFewPipeline(config, tooFewRules);
	const ImportPipeline::Result tooFewResult =
		tooFewPipeline.importSync(dataPath("sample_utf8.csv").c_str());
	EXPECT_EQ(tooFewResult.rows_imported, 0U);
	ASSERT_EQ(tooFewResult.errors.size(), 1U);
	ASSERT_EQ(tooFewResult.errors[0].cell_errors.size(), 1U);
	EXPECT_EQ(tooFewResult.errors[0].cell_errors[0].code, ErrorCode::ColumnCountMismatch);
	EXPECT_NE(tooFewResult.errors[0].cell_errors[0].message.find("5 columns"), std::string::npos);
	EXPECT_NE(tooFewResult.errors[0].cell_errors[0].message.find("4 validation rules"),
		std::string::npos);

	std::vector<FieldRule> tooManyRules = makeDefaultRules();
	tooManyRules.push_back({"extra", FieldType::String, false});
	ImportPipeline tooManyPipeline(config, tooManyRules);
	const ImportPipeline::Result tooManyResult =
		tooManyPipeline.importSync(dataPath("sample_utf8.csv").c_str());
	EXPECT_EQ(tooManyResult.rows_imported, 0U);
	ASSERT_EQ(tooManyResult.errors.size(), 1U);
	ASSERT_EQ(tooManyResult.errors[0].cell_errors.size(), 1U);
	EXPECT_EQ(tooManyResult.errors[0].cell_errors[0].code, ErrorCode::ColumnCountMismatch);
	EXPECT_NE(tooManyResult.errors[0].cell_errors[0].message.find("6 validation rules"),
		std::string::npos);
}

TEST(Pipeline, InvalidRegexErrorIsTraceable)
{
	const auto uniqueValue = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path()
		/ ("csvp_invalid_regex_" + std::to_string(uniqueValue) + ".csv");
	{
		std::ofstream output(path, std::ios::binary);
		ASSERT_TRUE(output.is_open());
		output << "value\ntext\n";
	}

	FieldRule rule;
	rule.name = "value";
	rule.type = FieldType::String;
	rule.regex = "[";
	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	ImportPipeline pipeline(config, {rule});
	const ImportPipeline::Result result = pipeline.importSync(path.string().c_str());

	EXPECT_EQ(result.rows_imported, 0U);
	EXPECT_EQ(result.rows_skipped, 0U);
	ASSERT_EQ(result.errors.size(), 1U);
	EXPECT_EQ(result.errors[0].line_number, 0U);
	EXPECT_TRUE(result.errors[0].raw_line.empty());
	ASSERT_EQ(result.errors[0].cell_errors.size(), 1U);
	EXPECT_EQ(result.errors[0].cell_errors[0].code, ErrorCode::RegexMismatch);
	EXPECT_NE(result.errors[0].cell_errors[0].message.find("Invalid regex pattern"),
		std::string::npos);
	std::filesystem::remove(path);
}

// ============================================================
// Pipeline.StreamingImportNotImplemented (PASS - skeleton check)
// ============================================================
TEST(Pipeline, StreamingImportNotImplemented) {
    ImportPipeline::Config cfg;
    ImportPipeline pipeline(cfg, makeDefaultRules());

    auto future = pipeline.importStreaming("dummy.csv",
        [](size_t) { return true; });

    auto result = future.get();
    // Skeleton should return empty result without crashing
    EXPECT_EQ(result.rows_imported, 0u);
}

// ============================================================
// Pipeline.ShutdownNotImplemented (PASS - skeleton check)
// ============================================================
TEST(Pipeline, ShutdownNotImplemented) {
    ImportPipeline::Config cfg;
    ImportPipeline pipeline(cfg, {});
    // Should not crash
    pipeline.shutdown(std::chrono::milliseconds(100));
}

} // anonymous namespace
