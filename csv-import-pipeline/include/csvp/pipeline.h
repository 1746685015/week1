#ifndef CSVP_PIPELINE_H_
#define CSVP_PIPELINE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "csvp/parser.h"
#include "csvp/types.h"
#include "csvp/validator.h"

namespace csvp {

class ErrorSink;

class ImportPipeline
{
public:
	struct Config
	{
		CsvParser::Config parser;
		bool skip_bad_rows = false;
		std::size_t error_limit = 0;
		std::string error_log_path;
	};

	struct Result
	{
		std::size_t rows_imported = 0;
		std::size_t rows_skipped = 0;
		std::size_t total_error_count = 0;
		std::vector<RowError> errors;
		std::string error_log_path;
		std::string failure_message;
		double elapsed_ms = 0.0;
		bool completed = false;
		bool canceled = false;
		bool error_log_complete = true;
	};

	ImportPipeline() = default;
	ImportPipeline(const Config &config, std::vector<FieldRule> rules);
	~ImportPipeline();

	ImportPipeline(const ImportPipeline &) = delete;
	ImportPipeline &operator=(const ImportPipeline &) = delete;

	Result importSync(const char *filepath);
	std::future<Result> importStreaming(const char *filepath,
		std::function<bool(std::size_t processed)> progress);
	void shutdown(std::chrono::milliseconds timeout);
	std::size_t pendingCount() const;
	Stats stats() const;

private:
	static constexpr std::size_t PROGRESS_INTERVAL = 1000U;

	Result runImport(const char *filepath,
		const std::function<bool(std::size_t)> *progress);
	bool validateAndCollect(Row &row, std::uint64_t lineNumber, const CsvParser &parser,
		ErrorSink *errorSink, Result &result);
	bool recordRowError(RowError rowError, ErrorSink *errorSink, Result &result);
	void finishErrorLog(ErrorSink *errorSink, Result &result);
	void completeSynchronousTask();
	void completeStreamingTask();
	void joinWorker();

	Config m_config;
	std::vector<FieldRule> m_rules;
	FieldValidator m_validator;

	std::atomic<std::size_t> m_totalLines = 0;
	std::atomic<std::size_t> m_bytesRead = 0;
	std::atomic<std::size_t> m_rowsImported = 0;
	std::atomic<std::size_t> m_rowsSkipped = 0;
	std::atomic<double> m_elapsedMilliseconds = 0.0;
	std::atomic<bool> m_stopRequested = false;

	mutable std::mutex m_stateMutex;
	std::mutex m_lifecycleMutex;
	std::condition_variable m_stateChanged;
	std::thread m_worker;
	std::size_t m_pendingCount = 0;
	bool m_importActive = false;
	bool m_streamingActive = false;
};

} // namespace csvp

#endif // CSVP_PIPELINE_H_
