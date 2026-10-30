# 缺陷修复与特性实现报告

> 学生：________马银龙_______  日期：2026-10-28  用时：_______________

---

## Part A：缺陷修复

### 缺陷 A-1

- **复现步骤**：运行无末尾换行样本，修复前只能读取 2 行而不是 3 行。
- **根因分析**：读取块找不到 `\n` 时直接返回 EOF，已经读入的最后一行没有交付。
- **修复方法**：EOF 且当前行非空时返回该行；非 EOF 的超长行明确报错。
- **验证证据**：`Parser.NoTrailingNewline` 在 Release、Debug、MSVC ASan Debug 均通过；末行字段精确为 `3,gamma,300`。标签 `part-a-a1-no-trailing-newline`。

---

### 缺陷 A-2

- **复现步骤**：同一 Parser 连续打开两个文件，修复前行号从 11 累加到 14，旧 16 MiB 缓冲未释放。
- **根因分析**：`parse()` 没有完整重置行号、字节数、错误和缓冲状态，失败路径也未完整关闭资源。
- **修复方法**：重入前统一关闭文件、清理状态；分配/表头失败立即关闭；使用 RAII 缓冲。
- **验证证据**：`Parser.ReentryState` 在 Release、Debug、ASan 均通过，两个文件的行号和字节数分别独立。标签 `part-a-a2-reentry-state`。

---

### 缺陷 A-3

- **复现步骤**：`Pipeline.SkipBadRows` 遇到 6 列坏行时仍抛异常，`ErrorLimit` 因此被跳过。
- **根因分析**：列数检查无条件抛出，没有遵守 `skip_bad_rows`，也没有产生可审计错误。
- **修复方法**：跳过模式生成 `ColumnCountMismatch` 后继续；严格模式保持失败；错误样本上限不影响总跳过数。
- **验证证据**：精确得到 imported=2、skipped=5、total errors=5；`error_limit=3` 时只保留 3 个内存样本。标签 `part-a-a3-skip-bad-rows`。

---

### 缺陷 A-4

- **复现步骤**：字节审计发现原 GBK 样本包含非法 `D4 2C`，同时解析器未实际使用 encoding 配置。
- **根因分析**：逐字节比较分隔符，既不能验证 UTF-8/GBK，也可能把合法多字节字符的尾字节误当分隔符。
- **修复方法**：验证编码名及字节序列，按完整字符推进；修正非法样本预期，并新增“GBK 尾字节等于自定义分隔符”的合法用例。
- **验证证据**：`ChineseGBKEncoding` 与 `GbkTrailingByteMatchesDelimiter` 在三种配置均 2/2 通过。标签 `part-a-a4-gbk-boundaries`。

---

### 缺陷 A-5

- **复现步骤**：旧 `toInt/toDouble/toBool` 无法区分合法 0/false 与空值、非法值、溢出或越界。
- **根因分析**：接口只有回退值而无错误通道，且 `atof()` 不能报告转换状态。
- **修复方法**：新增 `tryToInt/Double/Bool`，检查完整消费、范围、有限值和空白；兼容保留旧访问器。
- **验证证据**：`Row.TypedConversions` 覆盖成功、失败、溢出和越界并在 Release、Debug、ASan 通过。标签 `part-a-a5-safe-conversions`。

---

### 缺陷 A-6

- **复现步骤**：旧 Parser 每实例固定分配 16 MiB，并逐行 `fseek` 后重复读取最多 16 MiB；100k 三轮平均 28,530.1 ms、3,434 rows/s。
- **根因分析**：只消费大块首行后丢弃剩余数据，导致同一后续内容被反复读取和复制。
- **修复方法**：改为有界顺序块读取并保留未消费字节，当前行按需增长，删除裸 `malloc/free`；新增跨块长字段测试。
- **验证证据**：当时优化后平均 107.16 ms、914,516 rows/s，约 266.2 倍；最终 C7 的 1 GB 完整 sidecar 平均 555,506.986 rows/s。标签 `part-a-a6-buffered-io`。

---

### 缺陷 A-7

- **复现步骤**：原行号为 32 位 `int`，超过 `INT_MAX` 会溢出，且 Parser、RowError、Pipeline 类型不一致。
- **根因分析**：超大文件计数没有使用固定宽度类型，隐式转换可能隐藏截断。
- **修复方法**：端到端统一为 `std::uint64_t`，删除多余转换。
- **验证证据**：编译期类型断言和运行时 `UINT32_MAX + 1` 均通过。标签 `part-a-a7-wide-line-number`。

---

### 缺陷 A-8

- **复现步骤**：`"he said ""hi"""` 在首个引号处提前结束，引号边界、末尾空字段及非法引号处理不一致。
- **根因分析**：旧实现没有把 `""` 识别为字面双引号，字段循环也没有统一状态机语义。
- **修复方法**：每次完整解析一个字段，正确处理转义引号、引号内分隔符、空字段和所有非法引号路径。
- **验证证据**：`QuotedEscapedQuote`、`QuotedFieldBoundaries`、`RejectsMalformedQuotes` 三种配置均 3/3 通过。标签 `part-a-a8-quoted-fields`。

> 另完成 A-0、A-9～A-28 的边界、统计、错误保真、正则热路径与静态分析修复；逐项复现、根因、命令、提交和验证见 `docs/PART_A_EVIDENCE.md`。

---

## Part B：特性实现

### B-1: 流式导入（importStreaming + progress 回调）

- **设计思路**：同步与流式入口复用私有 `runImport()`，保证解析、列数检查、校验、坏行和统计语义一致。流式入口复制文件路径和 callback 后立即返回 future；每完成 1000 条数据记录（imported + skipped，不含表头）在 worker 线程调用一次 callback。callback 返回 `false` 属于正常取消，future 返回已完成部分的 Result。
- **线程模型**（如何管理后台线程与主线程的交互）：每个 `ImportPipeline` 最多有一个活动导入。`m_lifecycleMutex` 只串行化启动、join 和 shutdown 生命周期操作，`m_stateMutex` 只保护 active/pending 复合状态，统计使用原子变量。用户 callback 在所有内部锁之外执行。worker 不 detach；完成的 worker 在下一次启动时 join，活动 worker 在析构时收到停止信号并被 join。
- **关键代码变更摘要**：新增成员 worker、条件变量、生命周期/状态锁、`pendingCount()`、统一任务清理函数和非复制约束；使用 `std::packaged_task<Result()>` 将正常结果及异常交付 future；同步和流式入口通过 `m_importActive` 拒绝同实例并发写统计；文件失败、严格模式异常、callback 异常和线程创建异常路径都会恢复状态。

### B-2: 优雅停止（shutdown + timeout）

- **设计思路**：采用 C++17 可安全实现的协作停止，不强杀线程。`shutdown(timeout)` 发布原子停止信号，通过条件变量等待 worker 完成，等待达到 timeout 后无条件返回。
- **取消机制**（如何通知工作线程停止、超时如何处理）：worker 在每次读取前以及每次进度 callback 返回后检查停止标志。callback 阻塞时，shutdown 仅等待到 timeout；callback 自己调用 shutdown 时检测 worker 线程 ID 并立即返回，避免 self-join。析构函数最终负责停止并 join，保证对象销毁后没有 worker 访问成员。
- **未完成任务数汇报**：`pendingCount()` 在状态锁下读取实时值。活动流式任务为 1，所有正常、取消和异常退出路径最终设为 0。受控门闩测试确认 `shutdown(50ms)` 超时返回时 pending=1，释放 callback 后 future 返回 1000 行部分结果且 pending=0。

### Part B 验证摘要

- 最终 Release/Debug 全量回归：均为 65/65 通过，MSVC 编译告警为 0。
- 进度：`sample_100k.csv` 回调严格为 1000、2000……100000，共 100 次。
- callback 取消：在 2000 返回 `false`，future 返回恰好 2000 条已分类记录。
- 异常与生命周期：callback/严格模式异常、重复启动、异常后重启、callback 内 shutdown、活动对象析构均有自动化测试。
- 压力与工具：Part B 验收时 Release 随机顺序重复 100 轮全部通过；最终 MSVC ASan、clang-tidy 和复跑结果见附件及各 Part 证据文件。
- 逐提交命令、结果、Git SHA 和限制见 `docs/PART_B_EVIDENCE.md`。

---

## Part C：1 GB 内存受限导入与性能优化

### C-1: 编译告警清理

- 最终 MSVC 2022 x64 Release/Debug 均在 `/W4 /permissive- /utf-8` 下构建，项目 C++ 编译告警 0。
- clang-tidy 对生产源与 benchmark 启用 `clang-analyzer-*`、`bugprone-*`，项目代码可见告警/错误 0；保留的 `NOLINT` 均局部、带英文原因。
- CMake 对输入/日志缓冲配置做正整数和 16 MiB 上限校验；benchmark 明确报告实际编译值。

### C-2: 性能优化

- **发现的性能瓶颈**：逐行临时 `std::string` 固定预留、字段复制和 `Row::clear()` 销毁字符串容量；输入块大小也需要用数据选择。
- **优化方法**：Row 保留字段槽和字符串容量，Parser 直接写目标字段；输入块 64/256/1024 KiB、日志块 64/256/1024 KiB 分别做控制变量实验。最终选择输入 256 KiB、日志 64 KiB。
- **100k 对比**：无 sidecar 的 C4 基线平均 134.034 ms、746,079.798 rows/s；直接复用字段后 46.203 ms、2,164,369.117 rows/s，约 2.90 倍。完整 sidecar 下 256 KiB 输入为 110.340 ms，优于 64 KiB 的 114.355 ms 和 1 MiB 的 113.467 ms。
- **1 GB 正式三轮**：平均 18,001.574 ms、555,506.986 rows/s、55.767 MB/s；相对 C4 完整 sidecar 的 26,140.458 ms / 382,548.771 rows/s，耗时降低约 31.1%、吞吐提高约 45.2%。
- **内存与完整性**：最高峰值工作集 5,382,144 bytes（5.382 MB / 5.133 MiB），低于 50,000,000 bytes；每轮 imported=9,990,000、skipped/errors=10,000，三份完整日志均 10,000 条、mismatch=0。

### C-3: 额外发现

- benchmark 原先只用 imported 计算吞吐，已改为 imported + skipped，并增加 input bytes、MB/s、每轮值、内存、错误总数和日志完整性。
- `Result.errors` 不能同时承担“完整审计”和“有界内存”：新增 64 KiB 缓冲 JSONL sidecar，原始行/字段以十六进制无损保存，内存只留有限样本；日志打开、写入、flush 或 close 失败都会使导入明确失败。
- Windows 正式内存证据使用 `GetProcessMemoryInfo(PROCESS_MEMORY_COUNTERS_EX)`，而不是用“输入缓冲大小”或“ASan 无报错”冒充峰值内存。
- 详细实验过程、原始报告、机器可读汇总和 Git 标签见 `docs/PART_C_EVIDENCE.md` 与 `docs/evidence/`。

---

## 附件

以下文件随报告一并提交：

1. `docs/evidence/final_git_log.txt`：提交历史(当时写作业时还未有wps仓库，暂时提交到github仓库。https: https://github.com/1746685015/week1.git)。
2. `docs/evidence/final_ctest_release.xml`、`final_ctest_debug.xml`：最终测试结果。
3. `docs/evidence/final_asan_tests.xml` 与 `final_tool_verification.md`：ASan / clang-tidy 验收摘要。
4. `docs/PART_A_EVIDENCE.md`、`PART_B_EVIDENCE.md`、`PART_C_EVIDENCE.md`：逐任务可复现证据。
5. `docs/evidence/part_c_c7_final_1gb_report.txt`、`part_c_c7_acceptance_summary.json`：1 GB 正式三轮报告与汇总。
