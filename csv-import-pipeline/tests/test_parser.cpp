#include "csvp/parser.h"
#include "csvp/row.h"
#include "csvp/types.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <filesystem>
#include <type_traits>
#include <utility>

namespace {

// Helper to get the test data file path
std::string dataPath(const char* filename) {
    return std::string(TEST_DATA_DIR) + filename;
}

// ============================================================
// Parser.UTF8Basic (PASS)
// ============================================================
TEST(Parser, UTF8Basic) {
    csvp::CsvParser::Config cfg;
    cfg.delimiter = ',';
    cfg.has_header = true;

    csvp::CsvParser parser(cfg);
    ASSERT_TRUE(parser.parse(dataPath("sample_utf8.csv").c_str()));

    // Check header
    ASSERT_EQ(parser.header().size(), 5u);
    EXPECT_EQ(parser.header()[0], "id");
    EXPECT_EQ(parser.header()[1], "name");
    EXPECT_EQ(parser.header()[2], "age");

    // Read all rows
    int row_count = 0;
    csvp::Row row;
    while (parser.readRow(row)) {
        ++row_count;
        EXPECT_EQ(row.size(), 5u);
    }
    EXPECT_EQ(row_count, 10);
}

// ============================================================
// Parser.HeaderMapping (PASS)
// ============================================================
TEST(Parser, HeaderMapping) {
    csvp::CsvParser::Config cfg;
    cfg.has_header = true;

    csvp::CsvParser parser(cfg);
    ASSERT_TRUE(parser.parse(dataPath("sample_utf8.csv").c_str()));

    EXPECT_EQ(parser.header().size(), 5u);
    EXPECT_EQ(parser.header()[3], "score");
    EXPECT_EQ(parser.header()[4], "active");

    // First row
    csvp::Row row;
    ASSERT_TRUE(parser.readRow(row));
    EXPECT_EQ(row[0], "1");
    EXPECT_EQ(row[1], "Alice");
    EXPECT_EQ(row[2], "25");
    EXPECT_EQ(row[3], "95.5");
    EXPECT_EQ(row[4], "true");
}

// ============================================================
// Parser.NoTrailingNewline
// 已知现象见 docs/KNOWN_ISSUES.md #1。修复后把前缀 DISABLED_ 去掉以启用验证。
// ============================================================
TEST(Parser, NoTrailingNewline)
{
	csvp::CsvParser::Config config;
	config.has_header = true;

	csvp::CsvParser parser(config);
	ASSERT_TRUE(parser.parse(dataPath("sample_noeol.csv").c_str()));
	EXPECT_EQ(parser.header().size(), 3U);

	int rowCount = 0;
	csvp::Row row;
	csvp::Row lastRow;
	while (parser.readRow(row))
	{
		++rowCount;
		lastRow = row;
	}

	EXPECT_EQ(rowCount, 3);
	ASSERT_EQ(lastRow.size(), 3U);
	EXPECT_EQ(lastRow[0], "3");
	EXPECT_EQ(lastRow[1], "gamma");
	EXPECT_EQ(lastRow[2], "300");
}

// ============================================================
// Parser.ReentryState
// 已知现象见 docs/KNOWN_ISSUES.md #2。修复后去掉前缀 DISABLED_ 以启用验证。
// ============================================================
TEST(Parser, ReentryState)
{
	csvp::CsvParser::Config config;
	config.has_header = true;
	csvp::CsvParser parser(config);

	ASSERT_TRUE(parser.parse(dataPath("sample_utf8.csv").c_str()));
	int firstRowCount = 0;
	csvp::Row row;
	while (parser.readRow(row))
		++firstRowCount;
	EXPECT_EQ(firstRowCount, 10);
	EXPECT_EQ(parser.lineNumber(), 11);
	EXPECT_EQ(parser.bytesRead(), 236U);

	ASSERT_TRUE(parser.parse(dataPath("sample_noeol.csv").c_str()));
	EXPECT_TRUE(parser.lastError().empty());
	EXPECT_EQ(parser.lineNumber(), 1);
	int secondRowCount = 0;
	while (parser.readRow(row))
		++secondRowCount;

	EXPECT_EQ(secondRowCount, 3);
	EXPECT_EQ(parser.lineNumber(), 4);
	EXPECT_EQ(parser.bytesRead(), 51U);
}

TEST(Parser, ReentryAfterOpenFailure)
{
	csvp::CsvParser parser;
	csvp::Row row;
	ASSERT_TRUE(parser.parse(dataPath("sample_utf8.csv").c_str()));
	ASSERT_TRUE(parser.readRow(row));
	EXPECT_GT(parser.lineNumber(), 0U);

	EXPECT_FALSE(parser.parse("nonexistent_reentry_file_12345.csv"));
	EXPECT_TRUE(parser.header().empty());
	EXPECT_EQ(parser.lineNumber(), 0U);
	EXPECT_EQ(parser.bytesRead(), 0U);
	EXPECT_FALSE(parser.lastError().empty());

	ASSERT_TRUE(parser.parse(dataPath("sample_noeol.csv").c_str()));
	EXPECT_EQ(parser.header().size(), 3U);
	EXPECT_TRUE(parser.lastError().empty());
	int rowCount = 0;
	while (parser.readRow(row))
		++rowCount;
	EXPECT_EQ(rowCount, 3);
}

TEST(Parser, RejectsNullAndEmptyPaths)
{
	csvp::CsvParser parser;
	EXPECT_FALSE(parser.parse(nullptr));
	EXPECT_NE(parser.lastError().find("null or empty"), std::string::npos);
	EXPECT_TRUE(parser.header().empty());
	EXPECT_EQ(parser.lineNumber(), 0U);
	EXPECT_EQ(parser.bytesRead(), 0U);

	EXPECT_FALSE(parser.parse(""));
	EXPECT_NE(parser.lastError().find("null or empty"), std::string::npos);
	ASSERT_TRUE(parser.parse(dataPath("sample_noeol.csv").c_str()));
	EXPECT_EQ(parser.header().size(), 3U);
}

// ============================================================
// Parser.ChineseGBKEncoding
// 已知现象见 docs/KNOWN_ISSUES.md #3。修复后去掉前缀 DISABLED_ 以启用验证。
// ============================================================
TEST(Parser, ChineseGBKEncoding)
{
	csvp::CsvParser::Config config;
	config.has_header = true;
	config.encoding = "GBK";

	csvp::CsvParser parser(config);
	ASSERT_TRUE(parser.parse(dataPath("sample_gbk.csv").c_str()));
	ASSERT_EQ(parser.header().size(), 4U);

	csvp::Row row;
	ASSERT_TRUE(parser.readRow(row));
	EXPECT_EQ(row.size(), 4U);

	EXPECT_FALSE(parser.readRow(row));
	EXPECT_NE(parser.lastError().find("Invalid GBK"), std::string::npos);
}

TEST(Parser, GbkTrailingByteMatchesDelimiter)
{
	const auto uniqueValue = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path()
		/ ("csvp_gbk_delimiter_" + std::to_string(uniqueValue) + ".csv");
	const char content[] = {
		'f', 'i', 'r', 's', 't', '\\', 's', 'e', 'c', 'o', 'n', 'd', '\n',
		'A', static_cast<char>(0x81), static_cast<char>(0x5C), 'B', '\\', 'C', '\n'
	};
	{
		std::ofstream output(path, std::ios::binary);
		ASSERT_TRUE(output.is_open());
		output.write(content, static_cast<std::streamsize>(sizeof(content)));
	}

	csvp::CsvParser::Config config;
	config.has_header = true;
	config.delimiter = '\\';
	config.encoding = "GBK";
	{
		csvp::CsvParser parser(config);
		ASSERT_TRUE(parser.parse(path.string().c_str()));
		csvp::Row row;
		ASSERT_TRUE(parser.readRow(row));
		ASSERT_EQ(row.size(), 2U);
		EXPECT_EQ(row[0].size(), 4U);
		EXPECT_EQ(static_cast<unsigned char>(row[0][1]), 0x81U);
		EXPECT_EQ(static_cast<unsigned char>(row[0][2]), 0x5CU);
		EXPECT_EQ(row[1], "C");
	}
	std::filesystem::remove(path);
}

TEST(Parser, LineCrossesReadBufferBoundary)
{
	const auto uniqueValue = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path()
		/ ("csvp_buffer_boundary_" + std::to_string(uniqueValue) + ".csv");
	const std::string longField(100000U, 'x');
	{
		std::ofstream output(path, std::ios::binary);
		ASSERT_TRUE(output.is_open());
		output << "payload,status\n" << longField << ",tail\n";
	}

	{
		csvp::CsvParser parser;
		ASSERT_TRUE(parser.parse(path.string().c_str()));
		csvp::Row row;
		ASSERT_TRUE(parser.readRow(row));
		ASSERT_EQ(row.size(), 2U);
		EXPECT_EQ(row[0].size(), longField.size());
		EXPECT_EQ(row[0], longField);
		EXPECT_EQ(row[1], "tail");
	}
	std::filesystem::remove(path);
}

TEST(Parser, OversizedLineDoesNotLeakIntoNextRow)
{
	const auto uniqueValue = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path()
		/ ("csvp_oversized_line_" + std::to_string(uniqueValue) + ".csv");
	const std::string oversizedField(16U * 1024U * 1024U + 1U, 'x');
	{
		std::ofstream output(path, std::ios::binary);
		ASSERT_TRUE(output.is_open());
		output << oversizedField << "\nnext,tail\n";
	}

	csvp::CsvParser::Config config;
	config.has_header = false;
	{
		csvp::CsvParser parser(config);
		ASSERT_TRUE(parser.parse(path.string().c_str()));
		csvp::Row row;
		EXPECT_EQ(parser.readNext(row), csvp::CsvParser::ReadStatus::ErrorReadStatus);
		EXPECT_EQ(parser.lineNumber(), 1U);
		EXPECT_NE(parser.lastError().find("16 MB limit"), std::string::npos);

		EXPECT_EQ(parser.readNext(row), csvp::CsvParser::ReadStatus::RowReadStatus);
		EXPECT_EQ(parser.lineNumber(), 2U);
		ASSERT_EQ(row.size(), 2U);
		EXPECT_EQ(row[0], "next");
		EXPECT_EQ(row[1], "tail");
		EXPECT_EQ(parser.readNext(row), csvp::CsvParser::ReadStatus::EofReadStatus);
	}
	std::filesystem::remove(path);
}

TEST(Parser, SkipsManyEmptyLinesWithoutRecursion)
{
	const auto uniqueValue = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path()
		/ ("csvp_many_empty_lines_" + std::to_string(uniqueValue) + ".csv");
	constexpr std::size_t EMPTY_LINE_COUNT = 100000U;
	{
		std::ofstream output(path, std::ios::binary);
		ASSERT_TRUE(output.is_open());
		for (std::size_t lineIndex = 0; lineIndex < EMPTY_LINE_COUNT; ++lineIndex)
			output.put('\n');
		output << "value\n";
	}

	csvp::CsvParser::Config config;
	config.has_header = false;
	{
		csvp::CsvParser parser(config);
		ASSERT_TRUE(parser.parse(path.string().c_str()));
		csvp::Row row;
		EXPECT_EQ(parser.readNext(row), csvp::CsvParser::ReadStatus::RowReadStatus);
		EXPECT_EQ(parser.lineNumber(), EMPTY_LINE_COUNT + 1U);
		ASSERT_EQ(row.size(), 1U);
		EXPECT_EQ(row[0], "value");
		EXPECT_EQ(parser.bytesRead(), EMPTY_LINE_COUNT + 6U);
		EXPECT_EQ(parser.readNext(row), csvp::CsvParser::ReadStatus::EofReadStatus);
	}
	std::filesystem::remove(path);
}

TEST(Parser, LineNumberUsesFixedWidthType)
{
	static_assert(std::is_same_v<decltype(std::declval<const csvp::CsvParser &>().lineNumber()),
		std::uint64_t>);
	static_assert(std::is_same_v<decltype(csvp::RowError::line_number), std::uint64_t>);

	csvp::RowError error;
	error.line_number = static_cast<std::uint64_t>(UINT32_MAX) + 1U;
	EXPECT_EQ(error.line_number, 4294967296ULL);
}

// ============================================================
// Parser.EmptyFile (PASS)
// ============================================================
TEST(Parser, EmptyFile) {
    // Test with a non-existent file
    csvp::CsvParser::Config cfg;
    csvp::CsvParser parser(cfg);
    EXPECT_FALSE(parser.parse("nonexistent_file_12345.csv"));
    EXPECT_FALSE(parser.lastError().empty());
}

// ============================================================
// Parser.QuotedEscapedQuote
// 现象见 docs/KNOWN_ISSUES.md #9。修复后去掉前缀 DISABLED_ 启用验证。
// ============================================================
TEST(Parser, QuotedEscapedQuote) {
    csvp::CsvParser::Config cfg;
    cfg.has_header = true;
    csvp::CsvParser parser(cfg);
    ASSERT_TRUE(parser.parse(dataPath("sample_quote.csv").c_str()));

    csvp::Row row;
    ASSERT_TRUE(parser.readRow(row));  // row1: 1,"he said ""hi""",x

    // 第 2 列（phrase）应完整保留：he said "hi"
    EXPECT_EQ(row.size(), 3u);
    EXPECT_EQ(row[1], "he said \"hi\"");

    EXPECT_EQ(row[2], "x");
}

TEST(Parser, QuotedFieldBoundaries)
{
	const auto uniqueValue = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path()
		/ ("csvp_quoted_boundaries_" + std::to_string(uniqueValue) + ".csv");
	{
		std::ofstream output(path, std::ios::binary);
		ASSERT_TRUE(output.is_open());
		output << "one,two,three,four\n"
			<< "\"\",\"a,b\",plain,\"last\"\n"
			<< "a,b,c,\n";
	}

	{
		csvp::CsvParser parser;
		ASSERT_TRUE(parser.parse(path.string().c_str()));
		csvp::Row row;
		ASSERT_TRUE(parser.readRow(row));
		ASSERT_EQ(row.size(), 4U);
		EXPECT_TRUE(row[0].empty());
		EXPECT_EQ(row[1], "a,b");
		EXPECT_EQ(row[2], "plain");
		EXPECT_EQ(row[3], "last");

		ASSERT_TRUE(parser.readRow(row));
		ASSERT_EQ(row.size(), 4U);
		EXPECT_TRUE(row[3].empty());
	}
	std::filesystem::remove(path);
}

TEST(Parser, TrimsEveryUnquotedFieldOnlyWhenEnabled)
{
	const auto uniqueValue = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path()
		/ ("csvp_trim_fields_" + std::to_string(uniqueValue) + ".csv");
	{
		std::ofstream output(path, std::ios::binary);
		ASSERT_TRUE(output.is_open());
		output << "  first  ,\tsecond\t, third \n"
			<< "\"  inside  \", \"\tkept\t\" \n";
	}

	csvp::CsvParser::Config trimConfig;
	trimConfig.has_header = false;
	trimConfig.trim_spaces = true;
	{
		csvp::CsvParser parser(trimConfig);
		ASSERT_TRUE(parser.parse(path.string().c_str()));
		csvp::Row row;
		ASSERT_TRUE(parser.readRow(row));
		ASSERT_EQ(row.size(), 3U);
		EXPECT_EQ(row[0], "first");
		EXPECT_EQ(row[1], "second");
		EXPECT_EQ(row[2], "third");

		ASSERT_TRUE(parser.readRow(row));
		ASSERT_EQ(row.size(), 2U);
		EXPECT_EQ(row[0], "  inside  ");
		EXPECT_EQ(row[1], "\tkept\t");
	}

	csvp::CsvParser::Config preserveConfig;
	preserveConfig.has_header = false;
	preserveConfig.trim_spaces = false;
	{
		csvp::CsvParser parser(preserveConfig);
		ASSERT_TRUE(parser.parse(path.string().c_str()));
		csvp::Row row;
		ASSERT_TRUE(parser.readRow(row));
		ASSERT_EQ(row.size(), 3U);
		EXPECT_EQ(row[0], "  first  ");
		EXPECT_EQ(row[1], "\tsecond\t");
		EXPECT_EQ(row[2], " third ");
	}
	std::filesystem::remove(path);
}

TEST(Parser, RejectsMalformedQuotes)
{
	const auto uniqueValue = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path unquotedPath = std::filesystem::temp_directory_path()
		/ ("csvp_unquoted_quote_" + std::to_string(uniqueValue) + ".csv");
	const std::filesystem::path unclosedPath = std::filesystem::temp_directory_path()
		/ ("csvp_unclosed_quote_" + std::to_string(uniqueValue) + ".csv");
	{
		std::ofstream output(unquotedPath, std::ios::binary);
		ASSERT_TRUE(output.is_open());
		output << "ab\"cd,tail\n";
	}
	{
		std::ofstream output(unclosedPath, std::ios::binary);
		ASSERT_TRUE(output.is_open());
		output << "\"unclosed,tail\n";
	}

	csvp::CsvParser::Config config;
	config.has_header = false;
	{
		csvp::CsvParser parser(config);
		ASSERT_TRUE(parser.parse(unquotedPath.string().c_str()));
		csvp::Row row;
		EXPECT_FALSE(parser.readRow(row));
		EXPECT_NE(parser.lastError().find("Unexpected quote"), std::string::npos);
	}
	{
		csvp::CsvParser parser(config);
		ASSERT_TRUE(parser.parse(unclosedPath.string().c_str()));
		csvp::Row row;
		EXPECT_FALSE(parser.readRow(row));
		EXPECT_NE(parser.lastError().find("Unclosed quote"), std::string::npos);
	}

	std::filesystem::remove(unquotedPath);
	std::filesystem::remove(unclosedPath);
}

// ============================================================
// Parser.CRLFLFMixedCR
// 现象见 docs/KNOWN_ISSUES.md #10。修复后去掉前缀 DISABLED_ 启用验证。
// ============================================================
TEST(Parser, CRLFLFMixedCR)
{
	const auto uniqueValue = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path()
		/ ("csvp_mixed_line_endings_" + std::to_string(uniqueValue) + ".csv");
	const std::string content = "id,name,note\r\n"
		"1,alice,lf-only\n"
		"2,bob,\"has\rcr\"\r\n"
		"3,carol,final-cr\r";
	{
		std::ofstream output(path, std::ios::binary);
		ASSERT_TRUE(output.is_open());
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
	}

	{
		csvp::CsvParser parser;
		ASSERT_TRUE(parser.parse(path.string().c_str()));
		ASSERT_EQ(parser.header().size(), 3U);
		EXPECT_EQ(parser.header()[2], "note");

		csvp::Row row;
		ASSERT_TRUE(parser.readRow(row));
		ASSERT_EQ(row.size(), 3U);
		EXPECT_EQ(row[2], "lf-only");

		ASSERT_TRUE(parser.readRow(row));
		ASSERT_EQ(row.size(), 3U);
		EXPECT_EQ(row[2], std::string("has\rcr", 6U));

		ASSERT_TRUE(parser.readRow(row));
		ASSERT_EQ(row.size(), 3U);
		EXPECT_EQ(row[2], std::string("final-cr\r", 9U));
		EXPECT_FALSE(parser.readRow(row));
		EXPECT_EQ(parser.bytesRead(), content.size());
	}
	std::filesystem::remove(path);
}

} // anonymous namespace
