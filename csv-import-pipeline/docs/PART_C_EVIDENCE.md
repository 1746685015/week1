# Part C：内存受限大文件导入实验与验收证据

## 依据与证据纪律

- 原始要求来源：仓库外的《周作业：CSV 数据导入：缺陷修复与特性实现作业》PDF，Part C 与提交要求部分。
- 实施建议来源：`C:\Users\Admin\Downloads\PART_C_Detailed_Task_Checklist.md`。该文件是建议和实验计划，不替代老师要求；其中预期值必须由指定数据与程序实测确认。
- 原始 PDF 保持不修改。2026-10-28 复核文件大小为 1,420,624 bytes，SHA-256 为 `D1A2084ADAF2CB09C22A677D403D951DAEFD94CEAEE9CCB1495CB755D69AAA63`。
- `data/sample_1gb.csv` 是本机实验输入，不提交 Git；仓库只保存路径说明、验证脚本、报告和可复现实验命令。
- 本文件严格区分计划、推导值和实测值。命令未实际成功运行前，不写成“通过”。

## 对 Part C 建议清单的审查结论

清单正确覆盖了指定数据集、10,000,000 条数据、10,000 条错误、完整错误详情、50 MB 峰值内存、吞吐和 I/O 背压等老师硬约束。实施时作以下收紧：

1. 50 MB 采用更严格的十进制 `50,000,000 bytes` 判定，同时报告 MB 与 MiB；峰值必须是整个进程工作集，包含运行库、错误收集和错误日志。
2. ASan 用于内存安全，不等价于峰值内存证明；Windows 正式证据使用 `GetProcessMemoryInfo()` 的 `PeakWorkingSetSize`，并记录当前工作集与私有用量。
3. sidecar 日志不是题目唯一允许的架构，但它能在错误数增长时给出有界内存；是否保留要由完整性、峰值内存和吞吐实测共同决定。
4. JSONL 必须对任意原始字节无损。最终格式将把原始行和字段值编码为十六进制，并另外保存可读消息，避免非法 UTF-8 破坏 JSON。
5. 不能仅以 `Result.errors.size()` 代表错误总数；需要独立的 `total_error_count` 和日志完整性状态。
6. 性能改动分开测量；没有稳定收益的候选优化不保留。1 GB 文件只在基线与最终候选阶段运行，不在普通单元测试中反复读取。

## C0：固定数据集与原始基线

### 起点

- 分支：`part-c`。
- 起点提交：`e1b30220505632948496345f81a9a599f4fe5ded`（Part B 验收提交）。
- 数据验证工具：`tools/validate_large_dataset.py`，以二进制流逐行处理并在同一遍读取中计算 SHA-256；不会把 1 GB 文件载入内存。

### 数据规格验证

命令：

```powershell
python tools/validate_large_dataset.py data/sample_1gb.csv
(Get-Item data/sample_1gb.csv).Length
(Get-FileHash data/sample_1gb.csv -Algorithm SHA256).Hash
```

实测结果（退出码 0）：

- 路径：`D:\c++development\week01\csv-import-pipeline\csv-import-pipeline\data\sample_1gb.csv`。
- 大小：`1,003,898,922 bytes`。
- SHA-256：`89FA6C306B9730EA71A782DA699C64CEC1EFC6B4097852BB457BC30ABC9477D0`；PowerShell 独立复算一致。
- 表头：`id,name,age,score,active`。
- 数据行：10,000,000；坏行：10,000；末行以 LF 结束；字段数、顺序 ID、name 格式、age 规则、score 一位小数和 active 布尔值均无不匹配。
- 抽样坏行：物理行 1001/id 1000、物理行 5,000,001/id 5,000,000、物理行 10,000,001/id 10,000,000，均为 `age=N/A`。
- 单遍流式验证和哈希耗时 51.092 秒；完整 JSON 保存为 `docs/evidence/part_c_c0_dataset.json`。

### 原始基线环境与结果

环境：

- Windows 11 家庭版 10.0.26200（Build 26200）。
- AMD Ryzen 7 5800H，16 个逻辑处理器；物理内存 16,487,870,464 bytes（15.36 GiB）。
- Visual Studio 2022 x64 / MSVC 19.44.35229.0；CMake 3.28.1；Release `/O2`，`/W4`。
- Git SHA：`e1b30220505632948496345f81a9a599f4fe5ded`。

命令：

```powershell
tools/run_benchmark_with_memory.ps1 `
  -Executable build-review-baseline/Release/csvp_bench.exe `
  -CsvPath data/sample_1gb.csv `
  -ReportPath docs/evidence/part_c_c0_baseline.txt `
  -RepeatCount 1 -PollMilliseconds 50
```

实测结果：

- 程序退出码 0；pipeline 耗时 18,794.8 ms；外部墙钟 19,138.130 ms。
- imported=9,990,000；skipped=10,000；内存中错误对象=10,000；分类总数=10,000,000。
- 原 bench 错误地仅用 imported 计算吞吐，报告值为 531,529 rows/s；按全部已分类行复算为 532,062.060 rows/s。
- 输入吞吐为 53.414 MB/s（十进制）。
- 50 ms 外部采样峰值工作集 8,818,688 bytes（8.819 MB / 8.410 MiB），峰值私有字节 5,066,752；低于 50,000,000 bytes。
- 基线 pipeline 虽在内存中保留全部 10,000 条错误，但 bench 报告只持久化前 200 条，因此尚不满足“全部错误可事后排查”；C2 将补全持久化语义。
- 原始报告保存为 `docs/evidence/part_c_c0_baseline.txt`，内存摘要保存为 `docs/evidence/part_c_c0_memory.json`。外部采样仅作为基线；C3 将使用进程内 `GetProcessMemoryInfo()` 生成正式证据。

### Git 证据

提交信息：`test: record Part C dataset and baseline`。

标签：`part-c-c0-baseline`。

## C1：大文件完整性验收

### 验收工具

- 新增独立目标 `csvp_large_verify`，不注册为日常 ctest，避免每次单元测试都读取 1 GB。
- 工具使用生产 `ImportPipeline` 和与 bench 相同的 5 列规则；验证 imported、skipped、总分类数、错误对象总数与最终 stats。
- 精确核对第 1、第 5000、第 10000 条错误的物理行号、完整原始行、age 列索引/名称/原值和错误码，覆盖文件前部、中部与末尾。
- 输入不存在的处理已由 `Pipeline.MissingFile` 与 `Pipeline.StreamingMissingFile` 回归测试覆盖；错误日志失败将在 C2 引入 sink 后增加确定性测试。

### 验证命令

```powershell
cmake --build build-review-baseline --config Release
build-review-baseline/Release/csvp_large_verify.exe data/sample_1gb.csv
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

实测结果：Release 目标构建退出码 0、C++ 编译警告 0；`csvp_large_verify` 退出码 0，输出 imported=9,990,000、skipped=10,000、errors=10,000、total_lines=10,000,000，耗时 19,766.9 ms，并输出 `LARGE_IMPORT_INTEGRITY_PASS`。Release/Debug 日常全量回归均为 61/61 通过。原始摘要保存为 `docs/evidence/part_c_c1_integrity.txt`。

### Git 证据

提交信息：`test: add large-file integrity assertions`。

标签：`part-c-c1-integrity`。

## C2：完整错误 sidecar 与有界内存样本

### 设计与实现

- `ImportPipeline::Config::error_log_path` 非空时启用 JSONL sidecar；现有 `error_limit` 继续控制 `Result.errors` 的内存样本数。未启用日志时，`error_limit=0` 仍保持“全部错误留在内存”的兼容语义。
- `Result` 新增 `total_error_count`、`error_log_path`、`error_log_complete`、`completed`、`canceled` 和 `failure_message`，区分全量错误数、内存样本、日志完整性、正常取消与失败。
- 每条 `RowError` 先由 `ErrorSink` 写入 sidecar，再按 `error_limit` 决定是否保留内存样本；写入失败立即终止，结果不会标记 completed。
- `ErrorSink` 使用固定 64 KiB 输出缓冲，正常结束显式 flush/close 并检查状态；不逐条 flush、不建立异步无界队列。
- JSONL 保存物理行号、错误码、可读原因、列索引/名称。`raw_line` 与 `raw_value` 使用大写十六进制编码，能无损保存非法 UTF-8/GBK 和控制字节；其他字符串只输出 ASCII 或 JSON `\u00XX` 转义。

### 自动化测试

- `Pipeline.CompleteErrorLogIgnoresMemorySampleLimit`：生成 10,000 条错误，内存只留 3 条，sidecar 仍有 10,000 条，并核对首/中/末记录。
- `Pipeline.ErrorLogPreservesArbitraryRawBytes`：输入含 `0xFF`，日志以 `782CFF` 保存完整原始行且整条 JSONL 为 ASCII。
- `Pipeline.ErrorLogOpenFailureIsReported`：把目录当日志文件，确定性触发打开失败；结果 completed=false、error_log_complete=false、处理行数为 0，并带失败原因。

### 指定 1 GB 文件实测

命令：

```powershell
build-review-baseline/Release/csvp_large_verify.exe `
  data/sample_1gb.csv docs/evidence/part_c_c2_errors.jsonl
python tools/verify_error_log.py docs/evidence/part_c_c2_errors.jsonl
```

结果：两个命令退出码均为 0。生产管道 imported=9,990,000、skipped=10,000、total_errors=10,000、内存样本=200、total_lines=10,000,000、error_log_complete=true，并输出 `LARGE_IMPORT_INTEGRITY_PASS`。sidecar 共 10,000 行、3,896,682 bytes，SHA-256 为 `C237216C682B60FDF8B633807D023E52BDF5778C769DD9599A267D73C1BF9E4C`；独立 JSON 解析逐条复核 mismatch=0，首/中/末分别对应 id 1000、5,000,000、10,000,000。

该版本含日志的 pipeline 耗时 25,247.4 ms，吞吐 396,080.388 rows/s、39.762 MB/s，比 C0 不落盘基线慢 34.33%。这是完整错误持久化的初始成本，将在 C6 调优，但不会用丢错误换速度。

Release/Debug 全量回归均为 64/64 通过；C2 三项定向测试 3/3 通过；MSVC C++ 编译警告 0；`error_sink.cpp` 与 `pipeline.cpp` 的 clang-tidy 均退出码 0、可见告警/错误 0。完整 sidecar 与摘要分别保存为 `docs/evidence/part_c_c2_errors.jsonl`、`docs/evidence/part_c_c2_error_log_summary.json`。

### Git 证据

提交信息：`feat: persist complete large-import error details`。

标签：`part-c-c2-error-sidecar`。

## C3：进程内峰值内存实测

### 实现

- 新增 Windows benchmark 辅助模块 `process_memory.cpp/.h`，调用 `GetProcessMemoryInfo()` 并读取 `PROCESS_MEMORY_COUNTERS_EX`。
- 每轮在完整 import 返回后读取 `PeakWorkingSetSize`、`WorkingSetSize` 与 `PrivateUsage`。`PeakWorkingSetSize` 是进程生命周期峰值，覆盖 CSV 输入、校验、内存错误样本、JSONL 写入和 flush/close。
- 报告同时给出 bytes、十进制 MB 与二进制 MiB；50 MB 判定使用严格的 `50,000,000 bytes`。
- Windows API 规定参数类型为基类布局指针而扩展结构由 `cb` 指定，因此此平台边界保留一个有注释的 `reinterpret_cast`；没有在通用业务代码引入该转换。
- `run_benchmark_with_memory.ps1` 增加可选 error-log 参数，用 50 ms 外部轮询对进程内值交叉核验。

### 指定 1 GB 文件实测

命令：

```powershell
tools/run_benchmark_with_memory.ps1 `
  -Executable build-review-baseline/Release/csvp_bench.exe `
  -CsvPath data/sample_1gb.csv `
  -ReportPath docs/evidence/part_c_c3_1gb_memory_report.txt `
  -ErrorLogPath docs/evidence/part_c_c2_errors.jsonl `
  -RepeatCount 1 -PollMilliseconds 50
```

结果：程序退出码 0；pipeline 耗时 23,286.8 ms；完整 sidecar 仍为 10,000 条且 SHA-256 未变。进程内峰值工作集为 `4,980,736 bytes`（4.980736 MB / 4.750 MiB），结束时工作集 4,980,736 bytes、PrivateUsage 913,408 bytes。外部轮询测得峰值工作集 4,972,544 bytes（4.973 MB / 4.742 MiB）和峰值私有字节 913,408，和进程内证据一致；采用较高的进程内值判定，仅占 50 MB 限制的 9.96%，余量约 45.019 MB。

本次报告仍保留 C0 的旧吞吐口径（只算 imported）以隔离 C3 的主要变量；报告显示 428,998 rows/s，按全部 10,000,000 条复算为 429,427.830 rows/s、43.110 MB/s。C4 将修正工具本身的口径和标签。

原始报告：`docs/evidence/part_c_c3_1gb_memory_report.txt`；交叉核验摘要：`docs/evidence/part_c_c3_memory_crosscheck.json`。

### 验证

- Release 构建退出码 0，MSVC `/W4` C++ 编译警告 0。
- `bench_import.cpp` clang-tidy 退出码 0、可见告警/错误 0；标准 openmode 位掩码与 `main` 入口各有一个带英文原因的精确 `NOLINT`。
- 内存测量失败时 `available=false` 且数值为 0，不会把不可用数据伪装成达标；正式 Windows 运行已返回有效非零数据。

### Git 证据

提交信息：`test: measure peak memory for large import`。

标签：`part-c-c3-memory-measurement`。

## C4：修正 benchmark 统计与失败语义

### 修正内容

- `rows/sec` 改为 `(rows_imported + rows_skipped) / elapsed_seconds`，坏行也属于已分类记录。
- 每轮和汇总均报告 input bytes、elapsed、processed rows、rows/sec、十进制 MB/sec。
- 分开报告 imported、skipped、`total_error_count`、`memory_error_samples` 和是否达到内存样本上限，不再把 `errors.size()` 误称为总错误数。
- 报告 error-log 实际路径和 `error_log_complete`；多轮运行时为每轮生成带 `.runN` 后缀的独立 sidecar，避免后一轮覆盖前一轮证据。
- 每轮保留独立耗时与指标，同时计算同一文件、同一配置的平均值。
- 报告无法创建或写入时返回非零退出码；不存在“控制台成功但报告缺失”的假成功。
- repeat_count 改用 `std::from_chars` 严格解析，非法或非正整数返回错误，不再使用无法报告转换失败的 `atoi`。

### 100k 与 1 GB 验证

100k 命令：

```powershell
build-review-baseline/Release/csvp_bench.exe `
  data/sample_100k.csv 1 docs/evidence/part_c_c4_100k_report.txt `
  build-review-baseline/c4_100k_errors.jsonl
```

实测 input=3,146,588 bytes、imported=98,000、skipped=2,000、processed=100,000、total errors=2,000、内存样本=200、elapsed=135.197 ms、739,663.571 rows/s、23.274 MB/s，sidecar 完整。

1 GB 命令：

```powershell
build-review-baseline/Release/csvp_bench.exe `
  data/sample_1gb.csv 1 docs/evidence/part_c_c4_1gb_report.txt `
  docs/evidence/part_c_c2_errors.jsonl
```

实测 input=1,003,898,922 bytes、imported=9,990,000、skipped=10,000、processed=10,000,000、total errors=10,000、内存样本=200、elapsed=26,140.458 ms、382,548.771 rows/s、38.404 MB/s、峰值工作集 5,169,152 bytes，sidecar 完整且独立复核仍为 10,000 条、mismatch=0。

另以 `sample_utf8.csv` 把报告路径设为目录，报告打开失败时进程退出码为 1。Release/Debug 构建均退出码 0、C++ 编译警告 0；Release ctest 64/64 通过；bench clang-tidy 退出码 0、可见告警/错误 0。

报告与机器可读摘要：`docs/evidence/part_c_c4_100k_report.txt`、`docs/evidence/part_c_c4_1gb_report.txt`、`docs/evidence/part_c_c4_benchmark_summary.json`。

### Git 证据

提交信息：`fix(bench): report complete rows and error counts`。

标签：`part-c-c4-benchmark-statistics`。

## C5：复用 Row 字段存储并消除字段复制

### 分步实验与取舍

所有 100k 实验均使用同一台机器、同一 Release 构建、同一 `data/sample_100k.csv`、5 轮、不写 sidecar；每轮均分类 98,000 条合法记录和 2,000 条非法记录。结果如下：

| 实验 | 唯一主要变化 | 平均耗时 | 平均吞吐 | 平均输入吞吐 | 峰值工作集 |
|---|---|---:|---:|---:|---:|
| P0 | C4 原始实现 | 134.034 ms | 746,079.798 rows/s | 23.476 MB/s | 6,021,120 bytes |
| P1 | 删除每行临时字段的 `reserve(256)` | 131.908 ms | 758,105.398 rows/s | 23.854 MB/s | 6,041,600 bytes |
| P2 | `Row::clear()` 仅重置逻辑字段数，`append()` 复用槽位 | 123.542 ms | 809,439.096 rows/s | 25.470 MB/s | 6,107,136 bytes |
| P3 | 解析器通过 `appendField()` 直接写入复用槽位 | 46.203 ms | 2,164,369.117 rows/s | 68.104 MB/s | 6,017,024 bytes |

P1 单独收益约 1.6%，P2 相对 P0 的总耗时下降约 7.8%；P3 相对 P0 的总耗时下降约 65.5%，处理吞吐约为 2.90 倍，且峰值工作集没有增加。最终保留三项组合：去掉没有稳定价值的固定 256 字节临时预留、复用 Row 内已有字符串容量，并直接写入目标字段以消除逐字段临时字符串复制。没有引入会跨行悬空的 `string_view`。

原始逐轮报告：`docs/evidence/part_c_c5_p0_100k.txt`、`part_c_c5_p1_no_reserve_100k.txt`、`part_c_c5_p2_row_reuse_100k.txt`、`part_c_c5_p3_direct_field_100k.txt`。

### 正确性与大文件复核

- 新增 `Row.ClearHidesPreviousFieldsAndReusesSlots`，验证 `clear()` 后逻辑大小归零、旧字段不可见、有效性状态复位、越界 `at()` 抛出异常，并覆盖直接字段写入。
- Parser、Row、Pipeline 定向回归 34/34 通过；Release 与 Debug 全量回归均为 65/65 通过。
- `src/parser.cpp` 使用 clang-tidy 的 `clang-analyzer-*` 与 `bugprone-*` 检查，退出码 0、项目代码可见告警/错误 0；MSVC `/W4` 构建无 C++ 警告。
- 1 GB + 完整 sidecar 实测：imported=9,990,000、skipped=10,000、processed=10,000,000、total errors=10,000、elapsed=18,427.118 ms、542,678.471 rows/s、54.479 MB/s，峰值工作集 5,054,464 bytes。相对 C4 同类实测耗时降低约 29.5%，吞吐提高约 41.9%。
- 独立日志验证仍为 10,000 条、mismatch=0，SHA-256 仍为 `C237216C682B60FDF8B633807D023E52BDF5778C769DD9599A267D73C1BF9E4C`，证明性能修改没有少记错误或改变原始内容。

1 GB 报告：`docs/evidence/part_c_c5_1gb_report.txt`。

### Git 证据

提交信息：`perf(parser): reuse row field storage`。

标签：`part-c-c5-row-reuse`。

## C6：调优有界输入与错误日志 I/O

### 实验方法

- 新增经过 CMake 校验的 `CSVP_INPUT_BUFFER_KIB` 与 `CSVP_ERROR_LOG_BUFFER_KIB`，只接受不超过 16,384 KiB 的正整数；benchmark 报告会写出实际编译使用的字节数，避免仅凭构建参数猜测。
- 缓冲区改为固定初始大小的堆存储，防止 1 MiB 对照实验把大数组放入 Windows 线程栈；大小在构造后保持有界。
- 每组均使用同一 Release 工具、同一 `sample_100k.csv`、完整 sidecar、200 条内存错误样本和 5 轮测试。输入缓冲实验固定日志缓冲 64 KiB；日志缓冲实验固定输入缓冲 256 KiB。
- 日志仍为单线程顺序写入，不逐条 flush，不引入无界异步队列；结束时继续显式检查 flush 与 close。

### 输入缓冲对照

| 输入缓冲 | 平均耗时 | 平均吞吐 | 平均输入吞吐 | 峰值工作集 |
|---:|---:|---:|---:|---:|
| 64 KiB | 114.355 ms | 874,469.088 rows/s | 27.516 MB/s | 5,758,976 bytes |
| 256 KiB | 110.340 ms | 906,285.872 rows/s | 28.517 MB/s | 5,505,024 bytes |
| 1 MiB | 113.467 ms | 881,313.665 rows/s | 27.731 MB/s | 6,287,360 bytes |

256 KiB 相对 64 KiB 平均耗时降低约 3.5%、吞吐提高约 3.6%；1 MiB 没有继续提速且峰值更高，因此最终输入缓冲选择 256 KiB。

### 错误日志缓冲对照

| 日志缓冲 | 平均耗时 | 平均吞吐 | 平均输入吞吐 | 峰值工作集 |
|---:|---:|---:|---:|---:|
| 64 KiB | 110.340 ms | 906,285.872 rows/s | 28.517 MB/s | 5,505,024 bytes |
| 256 KiB | 110.827 ms | 902,305.408 rows/s | 28.392 MB/s | 5,701,632 bytes |
| 1 MiB | 111.636 ms | 895,768.230 rows/s | 28.186 MB/s | 6,488,064 bytes |

增大日志缓冲没有产生收益，1 MiB 还额外增加约 0.98 MB 峰值工作集；最终继续使用 64 KiB。选择结果由实测决定，而不是默认假设缓冲越大越快。

原始报告为 `docs/evidence/part_c_c6_input_64k_100k.txt`、`part_c_c6_input_256k_100k.txt`、`part_c_c6_input_1024k_100k.txt`、`part_c_c6_log_256k_100k.txt`、`part_c_c6_log_1024k_100k.txt`；汇总为 `part_c_c6_benchmark_summary.json`。

### 回归与静态检查

最终配置为输入 256 KiB、日志 64 KiB。Release/Debug 全量回归均为 65/65 通过；MSVC `/W4` 构建无 C++ 警告。`parser.cpp`、`error_sink.cpp`、`bench_import.cpp` 的 clang-tidy 均完成，项目代码可见告警/错误 0，第三方/系统告警被抑制，已有 5 个精确 `NOLINT` 均保留原因和局部范围。

### Git 证据

提交信息：`perf(io): tune bounded input and error-log buffers`。

标签：`part-c-c6-io-tuning`。

## C7：指定 1 GB 文件正式验收

### 固定输入与被测版本

- 被测代码 SHA：`12163362e995fed8256c11d15b55888a85886c7b`（C6 实现提交）。
- Release / VS2022 x64；输入缓冲 262,144 bytes，错误日志缓冲 65,536 bytes。
- 输入 `data/sample_1gb.csv`：1,003,898,922 bytes；SHA-256 `89FA6C306B9730EA71A782DA699C64CEC1EFC6B4097852BB457BC30ABC9477D0`，与 C0 完整流式规格验证的文件完全一致。
- 正式命令为 `csvp_bench data/sample_1gb.csv 3 docs/evidence/part_c_c7_final_1gb_report.txt build-review-baseline/part_c_c7_final_errors.jsonl`，退出码 0；每轮生成独立 `.run1`、`.run2`、`.run3` sidecar。

### 三轮正式结果

| 轮次 | imported | skipped/errors | elapsed | rows/s | MB/s | 峰值工作集 |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 9,990,000 | 10,000 | 18,021.592 ms | 554,889.922 | 55.705 | 5,189,632 bytes |
| 2 | 9,990,000 | 10,000 | 17,951.359 ms | 557,060.883 | 55.923 | 5,361,664 bytes |
| 3 | 9,990,000 | 10,000 | 18,031.770 ms | 554,576.734 | 55.674 | 5,382,144 bytes |

平均耗时 18,001.574 ms，平均吞吐 555,506.986 rows/s、55.767 MB/s。三轮均满足 imported + skipped = 10,000,000、total_error_count=10,000、内存样本=200、`error_log_complete=true`。最高峰值工作集为 5,382,144 bytes（5.382 MB / 5.133 MiB），仅占严格 50,000,000-byte 上限约 10.76%，余量 44,617,856 bytes。

相对 C4 的同文件、同完整 sidecar Release 实测，平均耗时由 26,140.458 ms 降至 18,001.574 ms，约降低 31.1%；吞吐由 382,548.771 提高到 555,506.986 rows/s，约提高 45.2%。

### 独立完整性复核

- `verify_error_log.py` 分别完整解析三份正式 sidecar；每份均为 10,000 条、3,896,682 bytes、mismatch=0，SHA-256 均为 `C237216C682B60FDF8B633807D023E52BDF5778C769DD9599A267D73C1BF9E4C`。
- 三份日志的第 1/5000/10000 条分别对应物理行 1001、5,000,001、10,000,001 和 id 1000、5,000,000、10,000,000，原始行均与指定 CSV 一致。
- 独立生产管道验收工具再次运行，输出 imported=9,990,000、skipped=10,000、total_errors=10,000、total_lines=10,000,000、`error_log_complete=true` 和 `LARGE_IMPORT_INTEGRITY_PASS`；其 sidecar 再次得到相同条数、SHA 和 mismatch=0。
- 正式多轮 sidecar 位于被忽略的本机构建目录，避免向仓库重复提交三份完全相同的 3.9 MB 文件；仓库内 `docs/evidence/part_c_c2_errors.jsonl` 保存了同一 SHA 的完整规范副本，正式汇总保存每轮校验值与抽样内容。
- C6 最终配置下 Release/Debug 全量回归均为 65/65 通过，因此 Part A/B 行为未被性能优化破坏。

原始 benchmark 报告：`docs/evidence/part_c_c7_final_1gb_report.txt`；机器可读验收摘要：`docs/evidence/part_c_c7_acceptance_summary.json`。

### Git 证据

提交信息：`test: verify 1GB import memory and integrity`。

标签：`part-c-c7-1gb-acceptance`。
