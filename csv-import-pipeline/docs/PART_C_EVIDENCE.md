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
