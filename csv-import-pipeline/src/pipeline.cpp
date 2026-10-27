#include "csvp/pipeline.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

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

ImportPipeline::Result ImportPipeline::importSync(const char* filepath) {
    Result result;
    auto t0 = std::chrono::steady_clock::now();
	m_totalLines.store(0, std::memory_order_relaxed);
	m_bytesRead.store(0, std::memory_order_relaxed);
	m_rowsImported.store(0, std::memory_order_relaxed);
	m_rowsSkipped.store(0, std::memory_order_relaxed);
	m_elapsedMilliseconds.store(0.0, std::memory_order_relaxed);

    CsvParser parser(m_config.parser);
    if (!parser.parse(filepath)) {
        // 文件打开或表头解析失败
        RowError fatal;
        fatal.line_number = 0;
        fatal.raw_line = parser.lastError();
        result.errors.push_back(std::move(fatal));
		const auto finishTime = std::chrono::steady_clock::now();
		result.elapsed_ms = std::chrono::duration<double, std::milli>(finishTime - t0).count();
		m_elapsedMilliseconds.store(result.elapsed_ms, std::memory_order_relaxed);
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
		result.errors.push_back(std::move(schemaError));
		m_bytesRead.store(parser.bytesRead(), std::memory_order_relaxed);
		const auto finishTime = std::chrono::steady_clock::now();
		result.elapsed_ms = std::chrono::duration<double, std::milli>(finishTime - t0).count();
		m_elapsedMilliseconds.store(result.elapsed_ms, std::memory_order_relaxed);
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
		result.errors.push_back(std::move(schemaError));
		m_bytesRead.store(parser.bytesRead(), std::memory_order_relaxed);
		const auto finishTime = std::chrono::steady_clock::now();
		result.elapsed_ms = std::chrono::duration<double, std::milli>(finishTime - t0).count();
		m_elapsedMilliseconds.store(result.elapsed_ms, std::memory_order_relaxed);
		return result;
	}

    Row row;
	while (true)
	{
		const CsvParser::ReadStatus readStatus = parser.readNext(row);
		if (readStatus == CsvParser::ReadStatus::EofReadStatus)
			break;

		m_totalLines.fetch_add(1, std::memory_order_relaxed);
		m_bytesRead.store(parser.bytesRead(), std::memory_order_relaxed);
		if (readStatus == CsvParser::ReadStatus::ErrorReadStatus)
		{
			std::ostringstream message;
			message << "CSV parse error at line " << parser.lineNumber()
				<< ": " << parser.lastError();
			if (!m_config.skip_bad_rows)
				throw std::runtime_error(message.str());

			RowError rowError;
			rowError.line_number = parser.lineNumber();
			parser.getRawLine(rowError.raw_line);
			CellError cellError;
			cellError.code = ErrorCode::ParseError;
			cellError.message = message.str();
			rowError.cell_errors.push_back(std::move(cellError));
			if (m_config.error_limit == 0 || result.errors.size() < m_config.error_limit)
				result.errors.push_back(std::move(rowError));
			++result.rows_skipped;
			m_rowsSkipped.store(result.rows_skipped, std::memory_order_relaxed);
			continue;
		}

		if (row.size() != expectedColumnCount)
		{
			std::ostringstream message;
			message << "Column count mismatch at line " << parser.lineNumber()
				<< ": expected " << expectedColumnCount
				<< " columns, got " << row.size();

			if (!m_config.skip_bad_rows)
				throw std::runtime_error(message.str());

			RowError rowError;
			rowError.line_number = parser.lineNumber();
			parser.getRawLine(rowError.raw_line);

			CellError cellError;
			cellError.code = ErrorCode::ColumnCountMismatch;
			cellError.message = message.str();
			rowError.cell_errors.push_back(std::move(cellError));
			if (m_config.error_limit == 0 || result.errors.size() < m_config.error_limit)
				result.errors.push_back(std::move(rowError));
			++result.rows_skipped;
			m_rowsSkipped.store(result.rows_skipped, std::memory_order_relaxed);
			continue;
		}

        // ---- 校验与收集 ----
        validateAndCollect(row, parser.lineNumber(), parser, result);
		m_rowsImported.store(result.rows_imported, std::memory_order_relaxed);
		m_rowsSkipped.store(result.rows_skipped, std::memory_order_relaxed);
    }

    // 后处理
	m_bytesRead.store(parser.bytesRead(), std::memory_order_relaxed);

    auto t1 = std::chrono::steady_clock::now();
    result.elapsed_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
	m_elapsedMilliseconds.store(result.elapsed_ms, std::memory_order_relaxed);

    return result;
}

// ============================================================
// validateAndCollect
// ============================================================

void ImportPipeline::validateAndCollect(Row &row, std::uint64_t lineNumber,
	const CsvParser &parser, Result &result)
{
	std::vector<CellError> cellErrors = m_validator.validate(row);
	if (cellErrors.empty())
	{
		++result.rows_imported;
		return;
	}

	RowError rowError;
	rowError.line_number = lineNumber;
	parser.getRawLine(rowError.raw_line);
	rowError.cell_errors = std::move(cellErrors);
	if (!m_config.skip_bad_rows)
	{
		result.errors.push_back(std::move(rowError));
		throw std::runtime_error("Validation failed and skip_bad_rows is disabled");
	}

	if (m_config.error_limit == 0 || result.errors.size() < m_config.error_limit)
		result.errors.push_back(std::move(rowError));
	++result.rows_skipped;
}

// ============================================================
// importStreaming（骨架）
//
// 现在的 importSync 是"一口气读完"：调用方一旦调用它，当前线程就被占住，
// 直到整个文件处理完才返回。这期间既看不到进度，也没办法喊停，
// 文件一大就要干等很久。
//
// importStreaming 做的是同一件事，只是换了干活的方式：
// 调用后马上返回一个 future，真正的读文件、解析、校验交给后台线程；
// 干活过程中定期汇报进度，调用方不满意可以随时叫停。
// ============================================================

std::future<ImportPipeline::Result> ImportPipeline::importStreaming(
    const char* /*filepath*/,
    std::function<bool(size_t)> /*progress*/) {
    // TODO: 把上面说的"后台干活"实现出来。
    //
    // 整体流程还是那几步：打开文件 -> 一行行读出来解析 -> 逐字段校验
    // -> 累计成功/跳过/错误明细，只不过这一整套不要占着调用方的线程，
    // 而是放到后台线程里跑。
    //
    // 具体要求：
    // 1. 本函数要快速返回，不能在这里等文件读完；导入工作在后台线程里进行。
    //    实现时可以一次读一批（比如 1000 行）再统一处理，这只是手段，
    //    不要求死。真正的导入流程（读、解析、校验）必须都由后台线程完成。
    // 2. 后台线程每处理完 1000 行，调用一次 progress(已处理的行数)。
    //    注意这个回调是在后台线程上执行的，回调里别做太慢的事情。
    // 3. 碰到下面两种情况要尽快停下来：
    //    - progress 回调返回 false，表示调用方不想继续了；
    //    - stop_requested_ 被置成 true，这是 shutdown() 发出的停止信号。
    //    检查的时机自己把握，但别等到整个文件读完才看一眼。
    // 4. 最终结果放进返回的 future 里。就算中途被叫停也没关系，
    //    把已经处理完的那部分结果交出去即可，不要抛异常。
    // 5. 每处理一行，更新原子统计计数器，stats() 靠它们反映实时进度。

    std::promise<Result> promise;
    promise.set_value(Result{});  // 占位：返回空结果
    return promise.get_future();
}

void ImportPipeline::shutdown(std::chrono::milliseconds timeout)
{
    // TODO: 让正在跑的导入停下来。
    //
    // 注意：这里做的是"打个招呼，让它自己收尾"，而不是把线程强行杀掉。
    // 线程没办法从外面直接掐断，只能靠后台那边看到信号后主动退出。
    //
    // 具体要求：
    // 1. 先把 stop_requested_ 置为 true，告诉后台线程"该收尾了"。
    // 2. 最多等 timeout 毫秒。到点就返回，不管后台有没有彻底停干净，
    //    绝不能无限期卡在这里。
    // 3. 返回时，把还没完成的任务数量写进 pending_count_，方便调用方查看。
    // 4. 整个过程不能死锁、不能泄漏线程或资源、不能崩溃。
    // 5. 这一条是对后台线程的配合要求：它必须隔一段时间看一眼 stop_requested_，
    //    否则如果它正卡在一个耗时的操作里（比如 std::stod 解析超长数字），
    //    shutdown 就只能一直干等到 timeout 才返回。
	m_stopRequested.store(true, std::memory_order_release);
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
