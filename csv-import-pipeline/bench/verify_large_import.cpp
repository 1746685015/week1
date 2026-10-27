// verify_large_import.cpp
// Verifies the assignment dataset's complete classification and sampled error details.

#include "csvp/pipeline.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t EXPECTED_IMPORTED_ROWS = 9990000U;
constexpr std::size_t EXPECTED_SKIPPED_ROWS = 10000U;
constexpr std::size_t EXPECTED_DATA_ROWS = 10000000U;
constexpr std::size_t EXPECTED_ERROR_COUNT = 10000U;
constexpr std::size_t EXPECTED_NAME_LENGTH = 78U;

std::vector<csvp::FieldRule> MakeDefaultRules()
{
	return {
		{"id", csvp::FieldType::Int, true},
		{"name", csvp::FieldType::String, true},
		{"age", csvp::FieldType::Int, true},
		{"score", csvp::FieldType::Double, true},
		{"active", csvp::FieldType::Bool, false},
	};
}

std::string ExpectedRawLine(std::uint64_t id)
{
	std::ostringstream name;
	name << "User" << std::setw(8) << std::setfill('0') << id;
	std::string paddedName = name.str();
	paddedName.append(EXPECTED_NAME_LENGTH - paddedName.size(), 'x');

	std::ostringstream line;
	line << id << ',' << paddedName << ",N/A,50.0,true";
	return line.str();
}

bool CheckError(const csvp::RowError &error, std::uint64_t expectedId)
{
	const std::uint64_t expectedPhysicalLine = expectedId + 1U;
	if (error.line_number != expectedPhysicalLine)
	{
		std::cerr << "Unexpected error line: expected " << expectedPhysicalLine
			<< ", got " << error.line_number << '\n';
		return false;
	}
	if (error.raw_line != ExpectedRawLine(expectedId))
	{
		std::cerr << "Raw line mismatch for id " << expectedId << '\n';
		return false;
	}
	if (error.cell_errors.size() != 1U)
	{
		std::cerr << "Unexpected cell error count for id " << expectedId << '\n';
		return false;
	}

	const csvp::CellError &cellError = error.cell_errors.front();
	if (cellError.column_index != 2U
		|| cellError.column_name != "age"
		|| cellError.raw_value != "N/A"
		|| cellError.code != csvp::ErrorCode::TypeConversionError)
	{
		std::cerr << "Error detail mismatch for id " << expectedId << '\n';
		return false;
	}
	return true;
}

} // namespace

int main(int argumentCount, char *arguments[])
{
	if (argumentCount != 2)
	{
		std::cerr << "Usage: csvp_large_verify <sample_1gb.csv>\n";
		return 2;
	}

	csvp::ImportPipeline::Config config;
	config.skip_bad_rows = true;
	config.error_limit = 0U;
	csvp::ImportPipeline pipeline(config, MakeDefaultRules());
	const csvp::ImportPipeline::Result result = pipeline.importSync(arguments[1]);
	const csvp::Stats statistics = pipeline.stats();

	bool passed = true;
	if (result.rows_imported != EXPECTED_IMPORTED_ROWS)
	{
		std::cerr << "Imported rows mismatch: " << result.rows_imported << '\n';
		passed = false;
	}
	if (result.rows_skipped != EXPECTED_SKIPPED_ROWS)
	{
		std::cerr << "Skipped rows mismatch: " << result.rows_skipped << '\n';
		passed = false;
	}
	if (result.rows_imported + result.rows_skipped != EXPECTED_DATA_ROWS)
	{
		std::cerr << "Not every data row was classified\n";
		passed = false;
	}
	if (result.errors.size() != EXPECTED_ERROR_COUNT)
	{
		std::cerr << "Error count mismatch: " << result.errors.size() << '\n';
		passed = false;
	}
	if (statistics.total_lines != EXPECTED_DATA_ROWS
		|| statistics.rows_imported != result.rows_imported
		|| statistics.rows_skipped != result.rows_skipped)
	{
		std::cerr << "Final statistics do not match the result\n";
		passed = false;
	}

	if (result.errors.size() == EXPECTED_ERROR_COUNT)
	{
		const std::array<std::pair<std::size_t, std::uint64_t>, 3> samples = {{
			{0U, 1000U},
			{4999U, 5000000U},
			{9999U, 10000000U},
		}};
		for (const auto &sample : samples)
		{
			if (!CheckError(result.errors[sample.first], sample.second))
				passed = false;
		}
	}

	std::cout << "imported=" << result.rows_imported
		<< " skipped=" << result.rows_skipped
		<< " errors=" << result.errors.size()
		<< " total_lines=" << statistics.total_lines
		<< " elapsed_ms=" << result.elapsed_ms << '\n';
	std::cout << (passed ? "LARGE_IMPORT_INTEGRITY_PASS" : "LARGE_IMPORT_INTEGRITY_FAIL") << '\n';
	return passed ? 0 : 1;
}
