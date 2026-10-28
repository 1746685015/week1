// bench_import.cpp
// Measures complete-row throughput and process memory, then writes an auditable report.

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "csvp/io_config.h"
#include "csvp/pipeline.h"
#include "process_memory.h"

namespace {

constexpr std::size_t MAX_ERROR_SAMPLES = 200U;
constexpr double BYTES_PER_MB = 1000000.0;
constexpr double BYTES_PER_MIB = 1024.0 * 1024.0;

struct RunSnapshot
{
	std::size_t rowsImported = 0U;
	std::size_t rowsSkipped = 0U;
	std::size_t totalErrorCount = 0U;
	std::size_t memoryErrorSamples = 0U;
	std::uint64_t inputBytes = 0U;
	double milliseconds = 0.0;
	double rowsPerSecond = 0.0;
	double megabytesPerSecond = 0.0;
	std::string errorLogPath;
	bool errorLogComplete = true;
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

std::string ErrorLogPathForRun(const BenchmarkOptions &options, int run)
{
	if (options.errorLogPath.empty() || options.repeat == 1)
		return options.errorLogPath;
	return options.errorLogPath + ".run" + std::to_string(run + 1);
}

bool WriteReport(const BenchmarkOptions &options, const std::vector<RunSnapshot> &runs,
	double averageMilliseconds, double averageProcessedRows,
	double averageRowsPerSecond, double averageMegabytesPerSecond)
{
	// Standard openmode flags are combined with their required bitmask operator.
	std::ofstream output(options.reportPath,
		std::ios::out | std::ios::trunc); // NOLINT(bugprone-signed-bitwise)
	if (!output)
	{
		std::cerr << "Cannot open report file: " << options.reportPath << '\n';
		return false;
	}

	std::uint64_t peakWorkingSetBytes = 0U;
	for (const RunSnapshot &snapshot : runs)
	{
		if (snapshot.memory.peakWorkingSetBytes > peakWorkingSetBytes)
			peakWorkingSetBytes = snapshot.memory.peakWorkingSetBytes;
	}

	output << std::fixed << std::setprecision(3)
		<< "CSV Import Pipeline Benchmark Report\n"
		<< "====================================\n\n"
		<< "File                         : " << options.filepath << '\n'
		<< "Input bytes                  : "
		<< (runs.empty() ? 0U : runs.front().inputBytes) << '\n'
		<< "Input buffer bytes           : " << csvp::INPUT_BUFFER_SIZE << '\n'
		<< "Error-log buffer bytes       : " << csvp::ERROR_LOG_BUFFER_SIZE << '\n'
		<< "Repeats                      : " << options.repeat << '\n'
		<< "Average processed rows       : " << averageProcessedRows << '\n'
		<< "Average elapsed ms           : " << averageMilliseconds << '\n'
		<< "Average rows/sec             : " << averageRowsPerSecond << '\n'
		<< "Average input MB/sec         : " << averageMegabytesPerSecond << '\n'
		<< "Peak working set bytes       : " << peakWorkingSetBytes << '\n'
		<< "Peak working set MB          : "
		<< static_cast<double>(peakWorkingSetBytes) / BYTES_PER_MB << '\n'
		<< "Peak working set MiB         : "
		<< static_cast<double>(peakWorkingSetBytes) / BYTES_PER_MIB << "\n\n"
		<< "--- Per-run Summary ---\n";

	for (std::size_t index = 0U; index < runs.size(); ++index)
	{
		const RunSnapshot &snapshot = runs[index];
		const std::size_t processedRows = snapshot.rowsImported + snapshot.rowsSkipped;
		output << "Run " << (index + 1U) << '\n'
			<< "  rows_imported              : " << snapshot.rowsImported << '\n'
			<< "  rows_skipped               : " << snapshot.rowsSkipped << '\n'
			<< "  processed_rows             : " << processedRows << '\n'
			<< "  total_error_count          : " << snapshot.totalErrorCount << '\n'
			<< "  memory_error_samples       : " << snapshot.memoryErrorSamples << '\n'
			<< "  memory_sample_limit_hit    : "
			<< (snapshot.totalErrorCount > snapshot.memoryErrorSamples ? "yes" : "no") << '\n'
			<< "  input_bytes                : " << snapshot.inputBytes << '\n'
			<< "  elapsed_ms                 : " << snapshot.milliseconds << '\n'
			<< "  rows_per_second            : " << snapshot.rowsPerSecond << '\n'
			<< "  input_MB_per_second        : " << snapshot.megabytesPerSecond << '\n'
			<< "  memory_measurement         : "
			<< (snapshot.memory.available ? "available" : "unavailable") << '\n'
			<< "  peak_working_set_bytes     : " << snapshot.memory.peakWorkingSetBytes << '\n'
			<< "  peak_working_set_MB        : "
			<< static_cast<double>(snapshot.memory.peakWorkingSetBytes) / BYTES_PER_MB << '\n'
			<< "  peak_working_set_MiB       : "
			<< static_cast<double>(snapshot.memory.peakWorkingSetBytes) / BYTES_PER_MIB << '\n'
			<< "  working_set_bytes          : " << snapshot.memory.workingSetBytes << '\n'
			<< "  private_usage_bytes        : " << snapshot.memory.privateUsageBytes << '\n'
			<< "  error_log_path             : " << snapshot.errorLogPath << '\n'
			<< "  error_log_complete         : "
			<< (snapshot.errorLogComplete ? "true" : "false") << "\n\n";
	}

	output << "--- Error Rows (in-memory sample from final run) ---\n";
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

	output.flush();
	if (!output.good())
	{
		std::cerr << "Failed while writing report file: " << options.reportPath << '\n';
		return false;
	}
	return true;
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
	// Standard openmode flags are combined with their required bitmask operator.
	std::ifstream sizeInput(options.filepath,
		std::ios::binary | std::ios::ate); // NOLINT(bugprone-signed-bitwise)
	if (!sizeInput.is_open())
	{
		std::cerr << "Cannot open input file for size measurement\n";
		return 1;
	}
	const std::streampos inputEnd = sizeInput.tellg();
	if (inputEnd < std::streampos(0))
	{
		std::cerr << "Cannot read input file size\n";
		return 1;
	}
	const std::uint64_t inputBytes =
		static_cast<std::uint64_t>(static_cast<std::streamoff>(inputEnd));
	sizeInput.close();

	double totalMilliseconds = 0.0;
	double totalProcessedRows = 0.0;
	std::vector<RunSnapshot> runs;
	runs.reserve(static_cast<std::size_t>(options.repeat));

	for (int run = 0; run < options.repeat; ++run)
	{
		csvp::ImportPipeline::Config config;
		config.parser.delimiter = ',';
		config.parser.has_header = true;
		config.skip_bad_rows = true;
		config.error_log_path = ErrorLogPathForRun(options, run);
		if (!config.error_log_path.empty())
			config.error_limit = MAX_ERROR_SAMPLES;

		csvp::ImportPipeline pipeline(config, MakeDefaultRules());
		const auto startTime = std::chrono::steady_clock::now();
		const csvp::ImportPipeline::Result result =
			pipeline.importSync(options.filepath.c_str());
		const auto finishTime = std::chrono::steady_clock::now();
		const double milliseconds =
			std::chrono::duration<double, std::milli>(finishTime - startTime).count();
		if (!result.completed)
		{
			std::cerr << "Import did not complete: " << result.failure_message << '\n';
			return 1;
		}
		if (!config.error_log_path.empty() && !result.error_log_complete)
		{
			std::cerr << "Error log did not complete: " << result.failure_message << '\n';
			return 1;
		}

		const std::size_t processedRows = result.rows_imported + result.rows_skipped;
		const double elapsedSeconds = milliseconds / 1000.0;
		RunSnapshot snapshot;
		snapshot.rowsImported = result.rows_imported;
		snapshot.rowsSkipped = result.rows_skipped;
		snapshot.totalErrorCount = result.total_error_count;
		snapshot.memoryErrorSamples = result.errors.size();
		snapshot.inputBytes = inputBytes;
		snapshot.milliseconds = milliseconds;
		snapshot.rowsPerSecond = static_cast<double>(processedRows) / elapsedSeconds;
		snapshot.megabytesPerSecond =
			static_cast<double>(inputBytes) / BYTES_PER_MB / elapsedSeconds;
		snapshot.errorLogPath = result.error_log_path;
		snapshot.errorLogComplete = result.error_log_complete;
		const std::size_t reportSampleCount = result.errors.size() < MAX_ERROR_SAMPLES
			? result.errors.size()
			: MAX_ERROR_SAMPLES;
		snapshot.errors.reserve(reportSampleCount);
		for (std::size_t index = 0U; index < reportSampleCount; ++index)
			snapshot.errors.push_back(result.errors[index]);
		snapshot.memory = csvp_bench::ReadProcessMemory();
		runs.push_back(std::move(snapshot));

		totalMilliseconds += milliseconds;
		totalProcessedRows += static_cast<double>(processedRows);
		const RunSnapshot &saved = runs.back();
		std::cout << "Run " << (run + 1)
			<< ": imported=" << saved.rowsImported
			<< " skipped=" << saved.rowsSkipped
			<< " total_errors=" << saved.totalErrorCount
			<< " memory_error_samples=" << saved.memoryErrorSamples
			<< " elapsed_ms=" << saved.milliseconds
			<< " rows_per_second=" << saved.rowsPerSecond
			<< " input_MB_per_second=" << saved.megabytesPerSecond
			<< " peak_working_set_bytes=" << saved.memory.peakWorkingSetBytes << '\n';
	}

	const double averageMilliseconds =
		totalMilliseconds / static_cast<double>(options.repeat);
	const double averageProcessedRows =
		totalProcessedRows / static_cast<double>(options.repeat);
	const double averageSeconds = averageMilliseconds / 1000.0;
	const double averageRowsPerSecond = averageProcessedRows / averageSeconds;
	const double averageMegabytesPerSecond =
		static_cast<double>(inputBytes) / BYTES_PER_MB / averageSeconds;
	if (!WriteReport(options, runs, averageMilliseconds, averageProcessedRows,
		averageRowsPerSecond, averageMegabytesPerSecond))
		return 1;
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
