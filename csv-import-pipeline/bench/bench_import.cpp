// bench_import.cpp
// Measures synchronous import throughput and process memory, then writes a text report.

#include <charconv>
#include <chrono>
#include <cstddef>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "csvp/pipeline.h"
#include "process_memory.h"

namespace {

constexpr std::size_t MAX_ERROR_SAMPLES = 200U;

struct RunSnapshot
{
	std::size_t rowsImported = 0U;
	std::size_t rowsSkipped = 0U;
	std::size_t errorCount = 0U;
	bool errorLimitHit = false;
	double milliseconds = 0.0;
	csvp_bench::ProcessMemorySnapshot memory;
	std::vector<csvp::RowError> errors;
};

struct BenchmarkOptions
{
	std::string filepath;
	std::string reportPath = "bench_report.txt";
	std::string errorLogPath;
	int repeat = 3;
};

const char *ErrorCodeName(csvp::ErrorCode code)
{
	switch (code)
	{
	case csvp::ErrorCode::Ok:
		return "Ok";
	case csvp::ErrorCode::ParseError:
		return "ParseError";
	case csvp::ErrorCode::ColumnCountMismatch:
		return "ColumnCountMismatch";
	case csvp::ErrorCode::TypeConversionError:
		return "TypeConversionError";
	case csvp::ErrorCode::RequiredFieldMissing:
		return "RequiredFieldMissing";
	case csvp::ErrorCode::RangeError:
		return "RangeError";
	case csvp::ErrorCode::RegexMismatch:
		return "RegexMismatch";
	case csvp::ErrorCode::EncodingError:
		return "EncodingError";
	case csvp::ErrorCode::FileNotOpen:
		return "FileNotOpen";
	case csvp::ErrorCode::InternalError:
		return "InternalError";
	}
	return "Unknown";
}

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

void WriteReport(const std::string &reportPath, const std::string &filepath, int repeat,
	const std::vector<RunSnapshot> &runs, double averageMilliseconds,
	std::size_t rowsPerSecond)
{
	// Standard openmode flags are combined with their required bitmask operator.
	std::ofstream output(reportPath, std::ios::out | std::ios::trunc); // NOLINT(bugprone-signed-bitwise)
	if (!output)
	{
		std::cerr << "Cannot open report file: " << reportPath << '\n';
		return;
	}

	output << "CSV Import Pipeline Benchmark Report\n"
		<< "====================================\n\n"
		<< "File            : " << filepath << '\n'
		<< "Repeats         : " << repeat << '\n'
		<< "Avg time        : " << averageMilliseconds << " ms\n"
		<< "Throughput      : " << rowsPerSecond << " rows/sec\n\n"
		<< "--- Per-run Summary ---\n"
		<< "run  rows_imported  rows_skipped  errors  error_limit_hit  time_ms  "
		<< "peak_working_set_bytes  peak_working_set_MB  peak_working_set_MiB  "
		<< "working_set_bytes  private_usage_bytes\n";
	for (std::size_t index = 0U; index < runs.size(); ++index)
	{
		const RunSnapshot &snapshot = runs[index];
		output << (index + 1U) << "    "
			<< snapshot.rowsImported << "              "
			<< snapshot.rowsSkipped << "            "
			<< snapshot.errorCount << "       "
			<< (snapshot.errorLimitHit ? "yes" : "no") << "              "
			<< snapshot.milliseconds << "    "
			<< snapshot.memory.peakWorkingSetBytes << "    "
			<< static_cast<double>(snapshot.memory.peakWorkingSetBytes) / 1000000.0 << "    "
			<< static_cast<double>(snapshot.memory.peakWorkingSetBytes) / (1024.0 * 1024.0) << "    "
			<< snapshot.memory.workingSetBytes << "    "
			<< snapshot.memory.privateUsageBytes << '\n';
	}

	output << "\n--- Error Rows (sample) ---\n";
	if (runs.empty() || runs.back().errors.empty())
		output << "(no error rows collected in this run)\n";
	else
	{
		for (const csvp::RowError &error : runs.back().errors)
		{
			output << "line " << error.line_number << ": " << error.raw_line << '\n';
			for (const csvp::CellError &cellError : error.cell_errors)
			{
				output << "    [col " << cellError.column_index << " '"
					<< cellError.column_name << "'] value='" << cellError.raw_value
					<< "' code=" << ErrorCodeName(cellError.code)
					<< " msg=" << cellError.message << '\n';
			}
		}
	}

	output << "\n--- Note ---\n"
		<< "Peak working set is read in-process with GetProcessMemoryInfo on Windows.\n"
		<< "The throughput formula is retained from the C0 baseline and is corrected in C4.\n";
}

bool ParseRepeatCount(const char *text, int &repeat)
{
	if (text == nullptr || text[0] == '\0')
		return false;
	const char *end = text;
	while (*end != '\0')
		++end;
	int parsed = 0;
	const std::from_chars_result conversion = std::from_chars(text, end, parsed);
	if (conversion.ec != std::errc() || conversion.ptr != end || parsed < 1)
		return false;
	repeat = parsed;
	return true;
}

int RunBench(const BenchmarkOptions &options)
{
	csvp::ImportPipeline::Config config;
	config.parser.delimiter = ',';
	config.parser.has_header = true;
	config.skip_bad_rows = true;
	if (!options.errorLogPath.empty())
	{
		config.error_limit = MAX_ERROR_SAMPLES;
		config.error_log_path = options.errorLogPath;
	}

	double totalMilliseconds = 0.0;
	std::size_t totalRows = 0U;
	std::vector<RunSnapshot> runs;
	runs.reserve(static_cast<std::size_t>(options.repeat));

	for (int run = 0; run < options.repeat; ++run)
	{
		csvp::ImportPipeline pipeline(config, MakeDefaultRules());
		const auto startTime = std::chrono::steady_clock::now();
		const csvp::ImportPipeline::Result result = pipeline.importSync(options.filepath.c_str());
		const auto finishTime = std::chrono::steady_clock::now();
		const double milliseconds =
			std::chrono::duration<double, std::milli>(finishTime - startTime).count();
		if (!result.completed)
		{
			std::cerr << "Import did not complete: " << result.failure_message << '\n';
			return 1;
		}
		if (!options.errorLogPath.empty() && !result.error_log_complete)
		{
			std::cerr << "Error log did not complete: " << result.failure_message << '\n';
			return 1;
		}

		totalMilliseconds += milliseconds;
		totalRows += result.rows_imported;
		RunSnapshot snapshot;
		snapshot.rowsImported = result.rows_imported;
		snapshot.rowsSkipped = result.rows_skipped;
		snapshot.errorCount = result.errors.size();
		snapshot.errorLimitHit =
			config.error_limit > 0U && result.errors.size() >= config.error_limit;
		snapshot.milliseconds = milliseconds;
		snapshot.memory = csvp_bench::ReadProcessMemory();
		const std::size_t sampleCount = result.errors.size() < MAX_ERROR_SAMPLES
			? result.errors.size()
			: MAX_ERROR_SAMPLES;
		for (std::size_t index = 0U; index < sampleCount; ++index)
			snapshot.errors.push_back(result.errors[index]);
		runs.push_back(std::move(snapshot));

		const RunSnapshot &saved = runs.back();
		std::cout << "Run " << (run + 1) << ": imported=" << result.rows_imported
			<< " skipped=" << result.rows_skipped
			<< " elapsed_ms=" << milliseconds
			<< " peak_working_set_bytes=" << saved.memory.peakWorkingSetBytes << '\n';
	}

	const double averageMilliseconds =
		totalMilliseconds / static_cast<double>(options.repeat);
	const std::size_t rowsPerSecond = static_cast<std::size_t>(
		(static_cast<double>(totalRows) / static_cast<double>(options.repeat))
		/ (averageMilliseconds / 1000.0));
	WriteReport(options.reportPath, options.filepath, options.repeat, runs,
		averageMilliseconds, rowsPerSecond);
	std::cout << "Report written to: " << options.reportPath << '\n';
	return 0;
}

} // namespace

// Function-level handlers convert all benchmark failures to a nonzero process result.
int main(int argumentCount, char *arguments[]) try // NOLINT(bugprone-exception-escape)
{
	if (argumentCount < 2)
	{
		std::cerr << "Usage: csvp_bench <csv_file> [repeat_count] [report_path] "
			<< "[error_log_path]\n";
		return 1;
	}

	BenchmarkOptions options;
	options.filepath = arguments[1];
	if (argumentCount >= 3 && !ParseRepeatCount(arguments[2], options.repeat))
	{
		std::cerr << "repeat_count must be a positive integer\n";
		return 1;
	}
	if (argumentCount >= 4)
		options.reportPath = arguments[3];
	if (argumentCount >= 5)
		options.errorLogPath = arguments[4];
	return RunBench(options);
}
catch (const std::exception &error)
{
	std::cerr << "Benchmark failed: " << error.what() << '\n';
	return 1;
}
catch (...)
{
	std::cerr << "Benchmark failed with an unknown exception\n";
	return 1;
}
