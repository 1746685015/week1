#include "csvp/pipeline.h"
#include "csvp/parser.h"
#include "csvp/validator.h"

#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {

using namespace csvp;

std::string dataPath(const char* filename) {
    return std::string(TEST_DATA_DIR) + filename;
}

std::vector<FieldRule> makeDefaultRules() {
    return {
        {"id", FieldType::Int, true},
        {"name", FieldType::String, true},
        {"age", FieldType::Int, true},
        {"score", FieldType::Double, true},
        {"active", FieldType::Bool, false},
    };
}

class CallbackGate
{
public:
	void waitAt(std::size_t processed, std::size_t trigger)
	{
		if (processed != trigger)
			return;

		std::unique_lock<std::mutex> gateLock(m_mutex);
		m_entered = true;
		m_changed.notify_all();
		m_changed.wait(gateLock, [this]()
		{
			return m_released;
		});
	}

	bool waitUntilEntered(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> gateLock(m_mutex);
		return m_changed.wait_for(gateLock, timeout, [this]()
		{
			return m_entered;
		});
	}

	void release()
	{
		{
			std::lock_guard<std::mutex> gateLock(m_mutex);
			m_released = true;
		}
		m_changed.notify_all();
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_changed;
	bool m_entered = false;
	bool m_released = false;
};

// ============================================================
// Concurrent.ImportAndStats
// ============================================================
TEST(Concurrent, ImportAndStats) {
    ImportPipeline::Config cfg;
    cfg.parser.delimiter = ',';
    cfg.parser.has_header = true;
    cfg.skip_bad_rows = true;

    ImportPipeline pipeline(cfg, makeDefaultRules());

    // 启动流式导入：导入在后台线程里跑，这一行马上返回
    std::atomic<bool> stop_flag{false};
    auto future = pipeline.importStreaming(
        dataPath("sample_100k.csv").c_str(),
        [&stop_flag](size_t processed) -> bool {
            // 这里不主动叫停，只记下进度；返回 true 表示"继续"
            (void)processed;
            return !stop_flag.load();
        });

    // 导入还在跑的时候，另起一个线程反复读 stats()，看会不会出问题
    std::atomic<bool> done{false};
    std::thread reader([&pipeline, &done, &stop_flag]() {
        for (int i = 0; i < 100 && !done.load(); ++i) {
            auto s = pipeline.stats();
            (void)s;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stop_flag.store(true);
    });

    // 等导入结束：future 就绪，说明后台已经收工
    future.wait();
    auto result = future.get();
    done.store(true);
    reader.join();

    std::cout << "Concurrent.ImportAndStats: imported=" << result.rows_imported << std::endl;
}

// ============================================================
// Concurrent.StatsThreadSafety
// ============================================================
TEST(Concurrent, StatsThreadSafety) {
    ImportPipeline pipeline(ImportPipeline::Config{}, {});

    // 多个线程同时调用 stats() 应当是安全的
    std::atomic<int> read_count{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&pipeline, &read_count]() {
            for (int j = 0; j < 1000; ++j) {
                auto s = pipeline.stats();
                (void)s;
                ++read_count;
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(read_count.load(), 4000);
    std::cout << "Concurrent.StatsThreadSafety: "
              << read_count.load() << " reads completed." << std::endl;
}

// ============================================================
// Concurrent.StatsAtomicRead (PASS)
// stats() 从原子计数器读取当前进度，不应崩溃。
// ============================================================
TEST(Concurrent, StatsAtomicRead) {
    ImportPipeline pipeline(ImportPipeline::Config{}, {});
    auto s = pipeline.stats();
    EXPECT_EQ(s.total_lines, 0u);
    std::cout << "Concurrent.StatsAtomicRead: stats() ok, total_lines="
              << s.total_lines << std::endl;
}

TEST(Concurrent, PendingCountInitiallyZero)
{
	ImportPipeline pipeline(ImportPipeline::Config{}, {});
	EXPECT_EQ(pipeline.pendingCount(), 0U);
}

TEST(Concurrent, ShutdownWhenIdle)
{
	ImportPipeline pipeline(ImportPipeline::Config{}, {});
	const auto startTime = std::chrono::steady_clock::now();
	pipeline.shutdown(std::chrono::milliseconds(25));
	const auto elapsed = std::chrono::steady_clock::now() - startTime;

	EXPECT_EQ(pipeline.pendingCount(), 0U);
	EXPECT_LT(elapsed, std::chrono::milliseconds(200));
}

TEST(Concurrent, StreamingProgressEveryThousand)
{
	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	ImportPipeline pipeline(config, makeDefaultRules());
	std::vector<std::size_t> progressValues;
	std::thread::id callbackThreadId;
	const std::thread::id callerThreadId = std::this_thread::get_id();

	std::future<ImportPipeline::Result> future = pipeline.importStreaming(
		dataPath("sample_100k.csv").c_str(),
		[&progressValues, &callbackThreadId](std::size_t processed)
		{
			callbackThreadId = std::this_thread::get_id();
			progressValues.push_back(processed);
			return true;
		});
	const ImportPipeline::Result result = future.get();

	EXPECT_EQ(result.rows_imported, 98000U);
	EXPECT_EQ(result.rows_skipped, 2000U);
	ASSERT_EQ(progressValues.size(), 100U);
	for (std::size_t index = 0; index < progressValues.size(); ++index)
		EXPECT_EQ(progressValues[index], (index + 1U) * 1000U);
	EXPECT_EQ(progressValues.back(), 100000U);
	EXPECT_NE(callbackThreadId, callerThreadId);
	EXPECT_EQ(pipeline.pendingCount(), 0U);
}

TEST(Concurrent, StreamingProgressCanCancel)
{
	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	ImportPipeline pipeline(config, makeDefaultRules());
	std::vector<std::size_t> progressValues;

	std::future<ImportPipeline::Result> future = pipeline.importStreaming(
		dataPath("sample_100k.csv").c_str(),
		[&progressValues](std::size_t processed)
		{
			progressValues.push_back(processed);
			return processed < 2000U;
		});
	const ImportPipeline::Result result = future.get();

	ASSERT_EQ(progressValues.size(), 2U);
	EXPECT_EQ(progressValues[0], 1000U);
	EXPECT_EQ(progressValues[1], 2000U);
	EXPECT_EQ(result.rows_imported + result.rows_skipped, 2000U);
	EXPECT_EQ(pipeline.pendingCount(), 0U);
}

TEST(Concurrent, SyncImportStatsMatchResult)
{
	ImportPipeline::Config config;
	ImportPipeline pipeline(config, makeDefaultRules());
	const ImportPipeline::Result result = pipeline.importSync(dataPath("sample_utf8.csv").c_str());
	const Stats statistics = pipeline.stats();

	EXPECT_EQ(statistics.total_lines, 10U);
	EXPECT_EQ(statistics.bytes_read, 236U);
	EXPECT_EQ(statistics.rows_imported, result.rows_imported);
	EXPECT_EQ(statistics.rows_skipped, result.rows_skipped);
	EXPECT_DOUBLE_EQ(statistics.elapsed_ms, result.elapsed_ms);
}

TEST(Concurrent, ReadsStatsDuringSyncImport)
{
	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	ImportPipeline pipeline(config, makeDefaultRules());
	ImportPipeline::Result result;
	std::atomic<bool> done = false;
	std::thread worker([&pipeline, &result, &done]()
	{
		result = pipeline.importSync(dataPath("sample_100k.csv").c_str());
		done.store(true, std::memory_order_release);
	});

	std::size_t previousTotalLines = 0;
	while (!done.load(std::memory_order_acquire))
	{
		const Stats statistics = pipeline.stats();
		EXPECT_GE(statistics.total_lines, previousTotalLines);
		previousTotalLines = statistics.total_lines;
		std::this_thread::yield();
	}
	worker.join();

	const Stats finalStatistics = pipeline.stats();
	EXPECT_EQ(finalStatistics.total_lines, 100000U);
	EXPECT_EQ(finalStatistics.rows_imported, result.rows_imported);
	EXPECT_EQ(finalStatistics.rows_skipped, result.rows_skipped);
	EXPECT_EQ(result.rows_imported, 98000U);
	EXPECT_EQ(result.rows_skipped, 2000U);
}

TEST(Concurrent, StatsDuringStreamingImport)
{
	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	ImportPipeline pipeline(config, makeDefaultRules());
	std::future<ImportPipeline::Result> future = pipeline.importStreaming(
		dataPath("sample_100k.csv").c_str(),
		[](std::size_t)
		{
			return true;
		});
	std::size_t previousTotalLines = 0;

	while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
	{
		const Stats statistics = pipeline.stats();
		EXPECT_GE(statistics.total_lines, previousTotalLines);
		previousTotalLines = statistics.total_lines;
		std::this_thread::yield();
	}
	const ImportPipeline::Result result = future.get();
	const Stats statistics = pipeline.stats();

	EXPECT_EQ(statistics.total_lines, 100000U);
	EXPECT_EQ(statistics.rows_imported, result.rows_imported);
	EXPECT_EQ(statistics.rows_skipped, result.rows_skipped);
	EXPECT_DOUBLE_EQ(statistics.elapsed_ms, result.elapsed_ms);
	EXPECT_EQ(pipeline.pendingCount(), 0U);
}

TEST(Concurrent, StreamingCallbackThrows)
{
	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	ImportPipeline pipeline(config, makeDefaultRules());
	std::future<ImportPipeline::Result> future = pipeline.importStreaming(
		dataPath("sample_100k.csv").c_str(),
		[](std::size_t) -> bool
		{
			throw std::runtime_error("Progress callback failed");
		});

	EXPECT_THROW(future.get(), std::runtime_error);
	EXPECT_EQ(pipeline.pendingCount(), 0U);
}

TEST(Concurrent, StrictModeBadRowMatchesSyncImport)
{
	ImportPipeline::Config config;
	config.skip_bad_rows = false;
	ImportPipeline syncPipeline(config, makeDefaultRules());
	ImportPipeline streamingPipeline(config, makeDefaultRules());
	std::string syncMessage;
	std::string streamingMessage;

	try
	{
		(void)syncPipeline.importSync(dataPath("sample_100k.csv").c_str());
	}
	catch (const std::runtime_error &error)
	{
		syncMessage = error.what();
	}
	std::future<ImportPipeline::Result> future = streamingPipeline.importStreaming(
		dataPath("sample_100k.csv").c_str(),
		[](std::size_t)
		{
			return true;
		});
	try
	{
		(void)future.get();
	}
	catch (const std::runtime_error &error)
	{
		streamingMessage = error.what();
	}

	EXPECT_FALSE(syncMessage.empty());
	EXPECT_EQ(streamingMessage, syncMessage);
	EXPECT_EQ(streamingPipeline.pendingCount(), 0U);
}

TEST(Concurrent, RejectsSyncImportWhileStreaming)
{
	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	ImportPipeline pipeline(config, makeDefaultRules());
	CallbackGate gate;
	std::future<ImportPipeline::Result> future = pipeline.importStreaming(
		dataPath("sample_100k.csv").c_str(),
		[&gate](std::size_t processed)
		{
			gate.waitAt(processed, 1000U);
			return processed < 1000U;
		});

	ASSERT_TRUE(gate.waitUntilEntered(std::chrono::seconds(2)));
	EXPECT_THROW((void)pipeline.importSync(dataPath("sample_utf8.csv").c_str()), std::logic_error);
	gate.release();
	EXPECT_NO_THROW((void)future.get());
	EXPECT_EQ(pipeline.pendingCount(), 0U);
}

TEST(Concurrent, ShutdownNoDeadlock)
{
	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	ImportPipeline pipeline(config, makeDefaultRules());
	CallbackGate gate;

	std::future<ImportPipeline::Result> future = pipeline.importStreaming(
		dataPath("sample_100k.csv").c_str(),
		[&gate](std::size_t processed)
		{
			gate.waitAt(processed, 1000U);
			return true;
		});

	ASSERT_TRUE(gate.waitUntilEntered(std::chrono::seconds(2)));

	const auto startTime = std::chrono::steady_clock::now();
	pipeline.shutdown(std::chrono::milliseconds(50));
	const auto elapsed = std::chrono::steady_clock::now() - startTime;
	EXPECT_LT(elapsed, std::chrono::milliseconds(500));
	EXPECT_EQ(pipeline.pendingCount(), 1U);
	pipeline.shutdown(std::chrono::milliseconds(10));
	EXPECT_EQ(pipeline.pendingCount(), 1U);

	gate.release();

	ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
	const ImportPipeline::Result result = future.get();
	EXPECT_EQ(result.rows_imported + result.rows_skipped, 1000U);
	EXPECT_EQ(pipeline.pendingCount(), 0U);
	pipeline.shutdown(std::chrono::milliseconds(50));
}

TEST(Concurrent, CompletedWorkerCanRestart)
{
	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	ImportPipeline pipeline(config, makeDefaultRules());

	std::future<ImportPipeline::Result> firstFuture = pipeline.importStreaming(
		dataPath("sample_utf8.csv").c_str(),
		[](std::size_t)
		{
			return true;
		});
	const ImportPipeline::Result firstResult = firstFuture.get();
	std::future<ImportPipeline::Result> secondFuture = pipeline.importStreaming(
		dataPath("sample_utf8.csv").c_str(),
		[](std::size_t)
		{
			return true;
		});
	const ImportPipeline::Result secondResult = secondFuture.get();

	EXPECT_EQ(secondResult.rows_imported, firstResult.rows_imported);
	EXPECT_EQ(secondResult.rows_skipped, firstResult.rows_skipped);
	EXPECT_EQ(pipeline.pendingCount(), 0U);
}

TEST(Concurrent, RejectsSecondStreamingImport)
{
	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	ImportPipeline pipeline(config, makeDefaultRules());
	CallbackGate gate;
	std::future<ImportPipeline::Result> future = pipeline.importStreaming(
		dataPath("sample_100k.csv").c_str(),
		[&gate](std::size_t processed)
		{
			gate.waitAt(processed, 1000U);
			return processed < 1000U;
		});

	ASSERT_TRUE(gate.waitUntilEntered(std::chrono::seconds(2)));
	EXPECT_THROW((void)pipeline.importStreaming(
		dataPath("sample_utf8.csv").c_str(),
		[](std::size_t)
		{
			return true;
		}), std::logic_error);
	gate.release();
	EXPECT_NO_THROW((void)future.get());
	EXPECT_EQ(pipeline.pendingCount(), 0U);
}

TEST(Concurrent, CallbackCanRequestShutdown)
{
	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	ImportPipeline pipeline(config, makeDefaultRules());
	std::atomic<std::size_t> pendingInCallback = 0U;
	std::future<ImportPipeline::Result> future = pipeline.importStreaming(
		dataPath("sample_100k.csv").c_str(),
		[&pipeline, &pendingInCallback](std::size_t)
		{
			pendingInCallback.store(pipeline.pendingCount(), std::memory_order_relaxed);
			pipeline.shutdown(std::chrono::seconds(1));
			return true;
		});
	const ImportPipeline::Result result = future.get();

	EXPECT_EQ(pendingInCallback.load(std::memory_order_relaxed), 1U);
	EXPECT_EQ(result.rows_imported + result.rows_skipped, 1000U);
	EXPECT_EQ(pipeline.pendingCount(), 0U);
}

TEST(Concurrent, DestructorJoinsActiveWorker)
{
	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	std::unique_ptr<ImportPipeline> pipeline =
		std::make_unique<ImportPipeline>(config, makeDefaultRules());
	CallbackGate gate;
	std::future<ImportPipeline::Result> importFuture = pipeline->importStreaming(
		dataPath("sample_100k.csv").c_str(),
		[&gate](std::size_t processed)
		{
			gate.waitAt(processed, 1000U);
			return true;
		});

	ASSERT_TRUE(gate.waitUntilEntered(std::chrono::seconds(2)));
	std::future<void> destructorFuture = std::async(std::launch::async, [&pipeline]()
	{
		pipeline.reset();
	});
	EXPECT_EQ(destructorFuture.wait_for(std::chrono::milliseconds(50)),
		std::future_status::timeout);
	gate.release();
	ASSERT_EQ(destructorFuture.wait_for(std::chrono::seconds(2)),
		std::future_status::ready);
	EXPECT_NO_THROW(destructorFuture.get());
	const ImportPipeline::Result result = importFuture.get();
	EXPECT_EQ(result.rows_imported + result.rows_skipped, 1000U);
}

TEST(Concurrent, RestartsAfterCallbackFailure)
{
	ImportPipeline::Config config;
	config.skip_bad_rows = true;
	ImportPipeline pipeline(config, makeDefaultRules());
	std::future<ImportPipeline::Result> failedFuture = pipeline.importStreaming(
		dataPath("sample_100k.csv").c_str(),
		[](std::size_t) -> bool
		{
			throw std::runtime_error("Progress callback failed");
		});

	EXPECT_THROW((void)failedFuture.get(), std::runtime_error);
	std::future<ImportPipeline::Result> recoveredFuture = pipeline.importStreaming(
		dataPath("sample_utf8.csv").c_str(),
		[](std::size_t)
		{
			return true;
		});
	const ImportPipeline::Result result = recoveredFuture.get();

	EXPECT_EQ(result.rows_imported, 10U);
	EXPECT_EQ(result.rows_skipped, 0U);
	EXPECT_EQ(pipeline.pendingCount(), 0U);
}

} // anonymous namespace
