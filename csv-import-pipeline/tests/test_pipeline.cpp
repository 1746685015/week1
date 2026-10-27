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
