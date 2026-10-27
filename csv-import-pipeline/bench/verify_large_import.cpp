// verify_large_import.cpp
// Verifies the assignment dataset's complete classification and sampled error details.

#include "csvp/pipeline.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
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
constexpr std::size_t EXPECTED_MEMORY_SAMPLES = 200U;
constexpr char HEX_DIGITS[] = "0123456789ABCDEF";

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

std::string HexEncode(const std::string &value)
{
	std::string encoded;
	encoded.reserve(value.size() * 2U);
	for (const unsigned char character : value)
	{
		encoded.push_back(HEX_DIGITS[(character >> 4U) & 0x0FU]);
		encoded.push_back(HEX_DIGITS[character & 0x0FU]);
	}
	return encoded;
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

bool CheckLogRecord(const std::string &record, std::uint64_t expectedId)
{
	const std::string lineToken = "\"line_number\":" + std::to_string(expectedId + 1U);
	const std::string rawToken =
		"\"raw_line_hex\":\"" + HexEncode(ExpectedRawLine(expectedId)) + "\"";
	if (record.find(lineToken) == std::string::npos
		|| record.find(rawToken) == std::string::npos
		|| record.find("\"code\":\"TypeConversionError\"") == std::string::npos)
	{
		std::cerr << "Error log record mismatch for id " << expectedId << '\n';
		return false;
	}
	return true;
}

} // namespace

int main(int argumentCount, char *arguments[])
{
	if (argumentCount != 2 && argumentCount != 3)
	{
		std::cerr << "Usage: csvp_large_verify <sample_1gb.csv> [error_log.jsonl]\n";
		return 2;
	}

	csvp::ImportPipeline::Config config;
	config.skip_bad_rows = true;
	const bool useErrorLog = argumentCount == 3;
	config.error_limit = useErrorLog ? EXPECTED_MEMORY_SAMPLES : 0U;
	if (useErrorLog)
		config.error_log_path = arguments[2];
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
	if (!result.completed || result.canceled || !result.failure_message.empty())
	{
		std::cerr << "Import did not report complete success: " << result.failure_message << '\n';
		passed = false;
	}
	if (result.total_error_count != EXPECTED_ERROR_COUNT)
	{
		std::cerr << "Total error count mismatch: " << result.total_error_count << '\n';
		passed = false;
	}
	const std::size_t expectedMemoryErrors =
		useErrorLog ? EXPECTED_MEMORY_SAMPLES : EXPECTED_ERROR_COUNT;
	if (result.errors.size() != expectedMemoryErrors)
	{
		std::cerr << "Memory error sample count mismatch: " << result.errors.size() << '\n';
		passed = false;
	}
	if (statistics.total_lines != EXPECTED_DATA_ROWS
		|| statistics.rows_imported != result.rows_imported
		|| statistics.rows_skipped != result.rows_skipped)
	{
		std::cerr << "Final statistics do not match the result\n";
		passed = false;
	}

	if (!useErrorLog && result.errors.size() == EXPECTED_ERROR_COUNT)
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
	if (useErrorLog)
	{
		if (!result.error_log_complete || result.error_log_path != arguments[2])
		{
			std::cerr << "Error log metadata reports an incomplete log\n";
			passed = false;
		}
		std::ifstream logInput(arguments[2], std::ios::binary);
		if (!logInput.is_open())
		{
			std::cerr << "Cannot reopen error log\n";
			passed = false;
		}
		else
		{
			std::size_t recordCount = 0U;
			std::string firstRecord;
			std::string middleRecord;
			std::string lastRecord;
			std::string record;
			while (std::getline(logInput, record))
			{
				++recordCount;
				if (recordCount == 1U)
					firstRecord = record;
				else if (recordCount == 5000U)
					middleRecord = record;
				else if (recordCount == EXPECTED_ERROR_COUNT)
					lastRecord = record;
			}
			if (recordCount != EXPECTED_ERROR_COUNT)
			{
				std::cerr << "Error log record count mismatch: " << recordCount << '\n';
				passed = false;
			}
			passed = CheckLogRecord(firstRecord, 1000U) && passed;
			passed = CheckLogRecord(middleRecord, 5000000U) && passed;
			passed = CheckLogRecord(lastRecord, 10000000U) && passed;
		}
	}

	std::cout << "imported=" << result.rows_imported
		<< " skipped=" << result.rows_skipped
		<< " total_errors=" << result.total_error_count
		<< " errors_in_memory=" << result.errors.size()
		<< " total_lines=" << statistics.total_lines
		<< " elapsed_ms=" << result.elapsed_ms
		<< " error_log_complete=" << (result.error_log_complete ? "true" : "false") << '\n';
	std::cout << (passed ? "LARGE_IMPORT_INTEGRITY_PASS" : "LARGE_IMPORT_INTEGRITY_FAIL") << '\n';
	return passed ? 0 : 1;
}
