#include "csvp/pipeline.h"

#include <sstream>
#include <stdexcept>
#include <utility>

#include "csvp/error_sink.h"

namespace csvp {

ImportPipeline::ImportPipeline(const Config &config, std::vector<FieldRule> rules)
	: m_config(config)
	, m_rules(std::move(rules))
{
	m_validator.addRules(m_rules);
}

ImportPipeline::~ImportPipeline()
{
	m_stopRequested.store(true, std::memory_order_release);
	joinWorker();
}

// ============================================================
// importSync
// ============================================================

ImportPipeline::Result ImportPipeline::importSync(const char *filepath)
{
	std::unique_lock<std::mutex> lifecycleLock(m_lifecycleMutex);
	{
		std::lock_guard<std::mutex> stateLock(m_stateMutex);
		if (m_importActive)
			throw std::logic_error("An import is already active");
	}
	if (m_worker.joinable())
		m_worker.join();

	{
		std::lock_guard<std::mutex> stateLock(m_stateMutex);
		m_importActive = true;
		m_stopRequested.store(false, std::memory_order_release);
	}
	lifecycleLock.unlock();

	try
	{
		Result result = runImport(filepath, nullptr);
		completeSynchronousTask();
		return result;
	}
	catch (...)
	{
		completeSynchronousTask();
		throw;
	}
}

ImportPipeline::Result ImportPipeline::runImport(const char *filepath,
	const std::function<bool(std::size_t)> *progress)
{
	Result result;
	const auto startTime = std::chrono::steady_clock::now();
	m_totalLines.store(0, std::memory_order_relaxed);
	m_bytesRead.store(0, std::memory_order_relaxed);
	m_rowsImported.store(0, std::memory_order_relaxed);
	m_rowsSkipped.store(0, std::memory_order_relaxed);
	m_elapsedMilliseconds.store(0.0, std::memory_order_relaxed);

	ErrorSink errorSink;
	ErrorSink *activeErrorSink = nullptr;
	result.error_log_path = m_config.error_log_path;
	if (!m_config.error_log_path.empty())
	{
		result.error_log_complete = false;
		std::string errorMessage;
		if (!errorSink.open(m_config.error_log_path, errorMessage))
		{
			result.failure_message = errorMessage;
			const auto finishTime = std::chrono::steady_clock::now();
			result.elapsed_ms =
				std::chrono::duration<double, std::milli>(finishTime - startTime).count();
			m_elapsedMilliseconds.store(result.elapsed_ms, std::memory_order_relaxed);
			return result;
		}
		activeErrorSink = &errorSink;
	}

	CsvParser parser(m_config.parser);
	auto finishResult = [&]()
	{
		finishErrorLog(activeErrorSink, result);
		m_bytesRead.store(parser.bytesRead(), std::memory_order_relaxed);
		const auto finishTime = std::chrono::steady_clock::now();
		result.elapsed_ms =
			std::chrono::duration<double, std::milli>(finishTime - startTime).count();
		m_elapsedMilliseconds.store(result.elapsed_ms, std::memory_order_relaxed);
	};

	if (!parser.parse(filepath))
	{
		RowError fatalError;
		fatalError.line_number = 0U;
		fatalError.raw_line = parser.lastError();
		CellError cellError;
		cellError.code = ErrorCode::FileNotOpen;
		cellError.message = parser.lastError();
		fatalError.cell_errors.push_back(std::move(cellError));
		(void)recordRowError(std::move(fatalError), activeErrorSink, result);
		result.failure_message = parser.lastError();
		finishResult();
		return result;
	}

	const std::size_t expectedColumnCount = m_config.parser.has_header
		? parser.header().size()
		: m_rules.size();
	if (m_config.parser.has_header && expectedColumnCount != m_rules.size())
	{
		std::ostringstream message;
		message << "Schema mismatch: CSV header has " << expectedColumnCount
			<< " columns, but " << m_rules.size() << " validation rules were configured";
		RowError schemaError;
		schemaError.line_number = parser.lineNumber();
		parser.getRawLine(schemaError.raw_line);
		CellError cellError;
		cellError.code = ErrorCode::ColumnCountMismatch;
		cellError.message = message.str();
		schemaError.cell_errors.push_back(std::move(cellError));
		(void)recordRowError(std::move(schemaError), activeErrorSink, result);
		result.failure_message = message.str();
		finishResult();
		return result;
	}

	std::string configurationError;
	if (m_validator.getConfigurationError(configurationError))
	{
		RowError schemaError;
		CellError cellError;
		cellError.code = ErrorCode::RegexMismatch;
		cellError.message = configurationError;
		schemaError.cell_errors.push_back(std::move(cellError));
		(void)recordRowError(std::move(schemaError), activeErrorSink, result);
		result.failure_message = configurationError;
		finishResult();
		return result;
	}

	Row row;
	bool reachedEnd = false;
	while (true)
	{
		if (m_stopRequested.load(std::memory_order_acquire))
		{
			result.canceled = true;
			break;
		}

		const CsvParser::ReadStatus readStatus = parser.readNext(row);
		if (readStatus == CsvParser::ReadStatus::EofReadStatus)
		{
			reachedEnd = true;
			break;
		}

		m_totalLines.fetch_add(1, std::memory_order_relaxed);
		m_bytesRead.store(parser.bytesRead(), std::memory_order_relaxed);
		bool continueImport = true;
		if (readStatus == CsvParser::ReadStatus::ErrorReadStatus)
		{
			std::ostringstream message;
			message << "CSV parse error at line " << parser.lineNumber()
				<< ": " << parser.lastError();

			RowError rowError;
			rowError.line_number = parser.lineNumber();
			parser.getRawLine(rowError.raw_line);
			CellError cellError;
			cellError.code = ErrorCode::ParseError;
			cellError.message = message.str();
			rowError.cell_errors.push_back(std::move(cellError));
			++result.rows_skipped;
			continueImport = recordRowError(std::move(rowError), activeErrorSink, result);
			if (!continueImport)
				result.failure_message = result.failure_message.empty()
					? "Failed to persist a parse error"
					: result.failure_message;
			if (!m_config.skip_bad_rows)
			{
				if (!continueImport)
					throw std::runtime_error(result.failure_message);
				throw std::runtime_error(message.str());
			}
		}
		else if (row.size() != expectedColumnCount)
		{
			std::ostringstream message;
			message << "Column count mismatch at line " << parser.lineNumber()
				<< ": expected " << expectedColumnCount
				<< " columns, got " << row.size();

			RowError rowError;
			rowError.line_number = parser.lineNumber();
			parser.getRawLine(rowError.raw_line);

			CellError cellError;
			cellError.code = ErrorCode::ColumnCountMismatch;
			cellError.message = message.str();
			rowError.cell_errors.push_back(std::move(cellError));
			++result.rows_skipped;
			continueImport = recordRowError(std::move(rowError), activeErrorSink, result);
			if (!m_config.skip_bad_rows)
			{
				if (!continueImport)
					throw std::runtime_error(result.failure_message);
				throw std::runtime_error(message.str());
			}
		}
		else
		{
			continueImport = validateAndCollect(row, parser.lineNumber(), parser,
				activeErrorSink, result);
		}

		m_rowsImported.store(result.rows_imported, std::memory_order_relaxed);
		m_rowsSkipped.store(result.rows_skipped, std::memory_order_relaxed);
		if (!continueImport)
			break;

		const std::size_t processed = result.rows_imported + result.rows_skipped;
		if (progress != nullptr && static_cast<bool>(*progress)
			&& processed % PROGRESS_INTERVAL == 0)
		{
			if (!(*progress)(processed))
			{
				result.canceled = true;
				break;
			}
		}

		if (m_stopRequested.load(std::memory_order_acquire))
		{
			result.canceled = true;
			break;
		}
	}

	result.completed = reachedEnd && result.failure_message.empty();
	finishResult();
	return result;
}

bool ImportPipeline::validateAndCollect(Row &row, std::uint64_t lineNumber,
	const CsvParser &parser, ErrorSink *errorSink, Result &result)
{
	std::vector<CellError> cellErrors = m_validator.validate(row);
	if (cellErrors.empty())
	{
		++result.rows_imported;
		return true;
	}

	RowError rowError;
	rowError.line_number = lineNumber;
	parser.getRawLine(rowError.raw_line);
	rowError.cell_errors = std::move(cellErrors);
	++result.rows_skipped;
	const bool recorded = recordRowError(std::move(rowError), errorSink, result);
	if (!recorded)
		return false;
	if (!m_config.skip_bad_rows)
		throw std::runtime_error("Validation failed and skip_bad_rows is disabled");
	return true;
}

bool ImportPipeline::recordRowError(RowError rowError, ErrorSink *errorSink, Result &result)
{
	++result.total_error_count;
	bool recorded = true;
	if (errorSink != nullptr)
	{
		std::string errorMessage;
		if (!errorSink->write(rowError, errorMessage))
		{
			result.failure_message = errorMessage;
			result.error_log_complete = false;
			recorded = false;
		}
	}
	if (m_config.error_limit == 0 || result.errors.size() < m_config.error_limit)
		result.errors.push_back(std::move(rowError));
	return recorded;
}

void ImportPipeline::finishErrorLog(ErrorSink *errorSink, Result &result)
{
	if (errorSink == nullptr)
		return;

	std::string errorMessage;
	if (errorSink->close(errorMessage))
	{
		result.error_log_complete = true;
		return;
	}
	result.error_log_complete = false;
	result.completed = false;
	if (result.failure_message.empty())
		result.failure_message = errorMessage;
}

std::future<ImportPipeline::Result> ImportPipeline::importStreaming(
	const char *filepath, std::function<bool(std::size_t)> progress)
{
	const std::string ownedFilepath = filepath == nullptr ? std::string() : filepath;

	std::unique_lock<std::mutex> lifecycleLock(m_lifecycleMutex);
	{
		std::lock_guard<std::mutex> stateLock(m_stateMutex);
		if (m_importActive)
			throw std::logic_error("An import is already active");
	}
	if (m_worker.joinable())
		m_worker.join();

	{
		std::lock_guard<std::mutex> stateLock(m_stateMutex);
		m_importActive = true;
		m_streamingActive = true;
		m_pendingCount = 1;
		m_stopRequested.store(false, std::memory_order_release);
	}

	// packaged_task stores task exceptions in the associated future.
	std::packaged_task<Result()> task(
		[this, ownedFilepath, progress = std::move(progress)]() // NOLINT(bugprone-exception-escape)
		mutable
	{
		try
		{
			Result result = runImport(ownedFilepath.c_str(), &progress);
			completeStreamingTask();
			return result;
		}
		catch (...)
		{
			completeStreamingTask();
			throw;
		}
	});
	std::future<Result> future = task.get_future();
	try
	{
		m_worker = std::thread(std::move(task));
	}
	catch (...)
	{
		completeStreamingTask();
		throw;
	}

	return future;
}

void ImportPipeline::shutdown(std::chrono::milliseconds timeout)
{
	std::unique_lock<std::mutex> lifecycleLock(m_lifecycleMutex);
	{
		std::lock_guard<std::mutex> stateLock(m_stateMutex);
		if (!m_streamingActive)
			return;
	}
	m_stopRequested.store(true, std::memory_order_release);
	if (m_worker.joinable() && m_worker.get_id() == std::this_thread::get_id())
		return;

	std::unique_lock<std::mutex> stateLock(m_stateMutex);
	m_stateChanged.wait_for(stateLock, timeout, [this]()
	{
		return !m_streamingActive;
	});
}

std::size_t ImportPipeline::pendingCount() const
{
	std::lock_guard<std::mutex> stateLock(m_stateMutex);
	return m_pendingCount;
}

void ImportPipeline::completeSynchronousTask()
{
	{
		std::lock_guard<std::mutex> stateLock(m_stateMutex);
		m_importActive = false;
	}
	m_stateChanged.notify_all();
}

void ImportPipeline::completeStreamingTask()
{
	{
		std::lock_guard<std::mutex> stateLock(m_stateMutex);
		m_importActive = false;
		m_streamingActive = false;
		m_pendingCount = 0;
	}
	m_stateChanged.notify_all();
}

void ImportPipeline::joinWorker()
{
	std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
	if (m_worker.joinable())
		m_worker.join();
}

// ============================================================
// stats
// ============================================================

Stats ImportPipeline::stats() const
{
	Stats statistics;
	statistics.total_lines = m_totalLines.load(std::memory_order_relaxed);
	statistics.bytes_read = m_bytesRead.load(std::memory_order_relaxed);
	statistics.rows_imported = m_rowsImported.load(std::memory_order_relaxed);
	statistics.rows_skipped = m_rowsSkipped.load(std::memory_order_relaxed);
	statistics.elapsed_ms = m_elapsedMilliseconds.load(std::memory_order_relaxed);
	return statistics;
}

} // namespace csvp
