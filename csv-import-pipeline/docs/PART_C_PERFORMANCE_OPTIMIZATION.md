# Part C 性能优化补充证据

## 回退基线

- 稳定提交：`d68026d`。
- 稳定分支：`part-c`。
- 稳定标签：`part-c-pre-optimization`。
- 原始提交包：`D:\c++development\week01\submission\MaYinlong_Week01_CSV_ORIGINAL.zip`。
- 原始提交包 SHA-256：`E1495DF06DE88DFF9EF8C6C0EA83C33E5E199DB2A4304A19D82AA585321FF58A`。
- 所有实验均在 `part-c-perf` 分支进行，不修改上述稳定版本。

## 统一测试条件

- 构建：Visual Studio 2022 x64 Release，使用 `/O2`。
- 输入：`data/sample_1gb.csv`，1,003,898,922 bytes，10,000,000 条数据行。
- 输入 SHA-256：`89FA6C306B9730EA71A782DA699C64CEC1EFC6B4097852BB457BC30ABC9477D0`。
- 每个版本连续运行 3 次，并在每轮写出完整错误 sidecar。
- 每轮期望 9,990,000 条导入、10,000 条跳过、10,000 条错误。
- 错误 sidecar 使用独立脚本逐条解析并核对行号、原因和原始内容。

## P1：合法单元格无分配验证快路径

### 根因

旧实现对每个合法单元格也构造 `CellError`，并复制列名和原始值。大文件的绝大多数数据合法，这些字符串复制随后立即销毁，形成纯粹的 CPU 与分配开销。

### 修改

- 新增内部 `validateValue` 快路径，先完成类型、范围和正则验证。
- 仅在验证失败时构造 `CellError` 并复制完整错误详情。
- 保持公开 `validateField` 接口、错误码、错误消息和原始值语义不变。

### 同时段 A/B 结果

| 指标 | 原版 `d68026d` | P1 优化版 | 变化 |
| --- | ---: | ---: | ---: |
| 3 轮平均耗时 | 18,022.881 ms | 16,429.346 ms | -8.842% |
| 平均吞吐 | 554,850.253 rows/s | 608,666.955 rows/s | +9.699% |
| 平均输入吞吐 | 55.701 MB/s | 61.104 MB/s | +9.700% |
| 峰值工作集 | 5,439,488 bytes | 5,906,432 bytes | 均远低于 50 MB |

P1 三轮耗时分别为 17,083.119 ms、16,140.809 ms、16,064.110 ms。原版反向对照三轮分别为 18,023.5 ms、18,037.0 ms、18,008.2 ms。

### 正确性证据

- Release：65/65 测试通过。
- Debug：65/65 测试通过。
- P1 三轮均为 imported=9,990,000、skipped=10,000、total_error_count=10,000。
- 三份 sidecar 均为 10,000 条、3,896,682 bytes、mismatch=0。
- 三份 sidecar SHA-256 均为 `C237216C682B60FDF8B633807D023E52BDF5778C769DD9599A267D73C1BF9E4C`，与稳定版本完全一致。
- 最高峰值工作集为 5,906,432 bytes，仅占 50,000,000-byte 硬上限的 11.813%。

### 复现命令

```powershell
cmake -S . -B build-perf -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_TESTS=ON -DBUILD_BENCH=ON
cmake --build build-perf --config Release --target csvp_bench csvp_tests
ctest --test-dir build-perf -C Release --output-on-failure
build-perf/Release/csvp_bench.exe data/sample_1gb.csv 3 `
  build-perf/perf_validate_fastpath_report.txt `
  build-perf/perf_validate_fastpath_errors.jsonl
python tools/verify_error_log.py build-perf/perf_validate_fastpath_errors.jsonl.run1
python tools/verify_error_log.py build-perf/perf_validate_fastpath_errors.jsonl.run2
python tools/verify_error_log.py build-perf/perf_validate_fastpath_errors.jsonl.run3
```

## P2：连续字段批量扫描与复制

### 根因

旧解析器逐字节调用编码检查，并通过 `std::string::append` 逐字符写入字段。对约 1 GB 的 ASCII 主体数据，这会产生接近十亿次细粒度判断和追加操作。

### 修改

- 未加引号字段按连续区段扫描，并在字段结束时一次性复制。
- ASCII 字节直接前进，仅在遇到非 ASCII 首字节时调用 UTF-8/GBK 序列验证。
- 加引号字段按引号分段复制；仍逐个处理 `""` 转义。
- 保留非法引号、非法 UTF-8、非法 GBK、GBK 尾字节、CRLF 和空字段的原有语义。

### 同时段 A/B 结果

| 指标 | P1 独立对照 | P2 优化版 | 变化 |
| --- | ---: | ---: | ---: |
| 3 轮平均耗时 | 15,129.801 ms | 6,598.286 ms | -56.389% |
| 平均吞吐 | 660,947.214 rows/s | 1,515,545.121 rows/s | +129.299% |
| 平均输入吞吐 | 66.352 MB/s | 152.145 MB/s | +129.301% |
| 峰值工作集 | 5,890,048 bytes | 5,496,832 bytes | 均远低于 50 MB |

P2 三轮耗时分别为 6,603.89 ms、6,601.51 ms、6,589.47 ms，波动小于 0.3%。相对同一测试会话中的原版 18,022.881 ms，累计耗时下降 63.389%，速度为原版的 2.731 倍。

### 正确性证据

- Release：65/65 测试通过。
- Debug：65/65 测试通过。
- Parser 专项覆盖 UTF-8、GBK、GBK 尾字节与分隔符冲突、引号转义、非法引号、CRLF/LF/CR、跨缓冲区行和无尾换行。
- P2 三轮均为 imported=9,990,000、skipped=10,000、total_error_count=10,000。
- 三份 sidecar 均为 10,000 条、3,896,682 bytes、mismatch=0。
- 三份 sidecar SHA-256 均为 `C237216C682B60FDF8B633807D023E52BDF5778C769DD9599A267D73C1BF9E4C`，证明错误完整性与稳定版本一致。
- 最高峰值工作集为 5,496,832 bytes，仅占 50,000,000-byte 硬上限的 10.994%。

### 复现命令

```powershell
cmake --build build-perf --config Release --target csvp_bench csvp_tests
ctest --test-dir build-perf -C Release --output-on-failure
build-perf/Release/csvp_bench.exe data/sample_1gb.csv 3 `
  build-perf/perf_parser_batch_report.txt `
  build-perf/perf_parser_batch_errors.jsonl
python tools/verify_error_log.py build-perf/perf_parser_batch_errors.jsonl.run1
python tools/verify_error_log.py build-perf/perf_parser_batch_errors.jsonl.run2
python tools/verify_error_log.py build-perf/perf_parser_batch_errors.jsonl.run3
```

## 未采用实验

曾实验把 `Stats` 原子计数从逐行发布改为每 1,000 行发布。独立同机 A/B 中，P1 为 15,129.801 ms，批量统计版为约 15,465.8 ms，后者慢约 2.2%。该实验已完整撤销，未提交到 Git。

在 P2 快速解析版本上又以 1,024 行为周期复测批量统计。逐行统计版为 6,598.286 ms，批量统计版为约 6,864.8 ms，后者慢约 4.0%。说明原子统计不是新热路径的主要瓶颈，该实验再次完整撤销。

输入缓冲区也使用同一 1 GB 数据重新扫描：64 KiB、256 KiB、1 MiB 的三轮平均耗时分别约为 6,698.077 ms、6,598.286 ms、6,645.593 ms。1 MiB 的峰值工作集还从约 5.50 MB 增至约 6.71 MB，因此继续保留 256 KiB。

## P3：8 字节 ASCII 块扫描

### 根因

P2 已把字段内容合并为一次复制，但仍逐字节判断分隔符、引号和编码。样例的长姓名字段以 ASCII 为主，逐字节循环仍占用明显 CPU 时间。

### 修改

- 使用 `memcpy` 安全读取未对齐的 8 字节块，不执行越界读取。
- 通过可移植的位运算同时检测分隔符、引号和非 ASCII 高位。
- 普通 ASCII 块一次前进 8 字节；检测到特殊字节后继续使用原有逐字符和编码校验逻辑。
- 不依赖 SSE/AVX 或 Windows 专属接口，非 ASCII、引号与自定义分隔符语义不变。

### 5+5 轮独立 A/B 结果

| 指标 | P2 独立对照 | P3 优化版 | 变化 |
| --- | ---: | ---: | ---: |
| 5 轮平均耗时 | 6,745.562 ms | 6,180.238 ms | -8.381% |
| 平均吞吐 | 1,482,456.107 rows/s | 1,618,060.627 rows/s | +9.147% |
| 平均输入吞吐 | 148.824 MB/s | 162.437 MB/s | +9.147% |
| 峰值工作集 | 6,283,264 bytes | 5,795,840 bytes | 均远低于 50 MB |

P3 五轮耗时分别为 6,165.25 ms、6,126.79 ms、6,161.00 ms、6,275.54 ms、6,172.61 ms；中位数为 6,165.25 ms。相对原版同会话基线 18,022.881 ms，累计耗时下降 65.709%，速度为原版的 2.916 倍。

### 正确性与安全性证据

- Release：65/65 测试通过。
- Debug：65/65 测试通过。
- MSVC AddressSanitizer RelWithDebInfo：65/65 测试通过，零 ASan 报告。
- P3 五轮均为 imported=9,990,000、skipped=10,000、total_error_count=10,000。
- 五份 sidecar 均为 10,000 条、3,896,682 bytes、mismatch=0。
- 五份 sidecar SHA-256 均为 `C237216C682B60FDF8B633807D023E52BDF5778C769DD9599A267D73C1BF9E4C`。
- 最高峰值工作集为 5,795,840 bytes，仅占 50,000,000-byte 硬上限的 11.592%。

### ASan 复现命令

```powershell
cmake -S . -B build-perf-asan -G "Visual Studio 17 2022" -A x64 `
  -DENABLE_ASAN=ON -DBUILD_TESTS=ON -DBUILD_BENCH=OFF
cmake --build build-perf-asan --config RelWithDebInfo --target csvp_tests
$asanRuntime = "D:/vs2022application/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64"
$env:Path = "$asanRuntime;$env:Path"
ctest --test-dir build-perf-asan -C RelWithDebInfo --output-on-failure
```

## P4：浮点验证快速转换

### 根因

每条数据都要验证 `score` 浮点字段。旧实现只使用 `strtod`，需要处理 C locale 和全局 `errno`，对简单十进制字段存在不必要开销。

### 修改

- 优先使用 C++17 `std::from_chars` 完成无 locale、无分配的十进制转换。
- 仅当快路径不接受输入时回退到原有 `strtod`，保留 `+1.5` 等旧语法兼容性。
- 两条路径都要求完整消费输入、拒绝溢出并拒绝 `NaN`/`Inf`。
- 新增 `+1.5` 兼容性回归断言。

### 5+5 轮独立 A/B 结果

| 指标 | P3 独立对照 | P4 优化版 | 变化 |
| --- | ---: | ---: | ---: |
| 5 轮平均耗时 | 6,124.975 ms | 5,661.871 ms | -7.561% |
| 平均吞吐 | 1,632,659.757 rows/s | 1,766,200.470 rows/s | +8.179% |
| 平均输入吞吐 | 163.903 MB/s | 177.309 MB/s | +8.179% |
| 峰值工作集 | 6,066,176 bytes | 5,591,040 bytes | 均远低于 50 MB |

P4 五轮耗时分别为 5,624.29 ms、5,643.13 ms、5,649.75 ms、5,759.13 ms、5,633.05 ms。相对原版同会话基线 18,022.881 ms，累计耗时下降 68.585%，速度为原版的 3.183 倍。

### 正确性与安全性证据

- Release：65/65 测试通过。
- Debug：65/65 测试通过。
- MSVC AddressSanitizer RelWithDebInfo：65/65 测试通过，零 ASan 报告。
- P4 五轮均为 imported=9,990,000、skipped=10,000、total_error_count=10,000。
- 五份 sidecar 均为 10,000 条、3,896,682 bytes、mismatch=0。
- 五份 sidecar SHA-256 均为 `C237216C682B60FDF8B633807D023E52BDF5778C769DD9599A267D73C1BF9E4C`。
- 最高峰值工作集为 5,591,040 bytes，仅占 50,000,000-byte 硬上限的 11.182%。

## 最终正式验收

- 被测代码：`b8e0d5c`。
- 正式三轮平均：5,841.035 ms、1,712,025.430 rows/s、171.870 MB/s。
- 正式最高峰值工作集：5,443,584 bytes（5.444 MB / 5.191 MiB）。
- 三轮均处理 10,000,000 条数据，完整记录 10,000 条错误；三份 sidecar 均 mismatch=0 且 SHA-256 与稳定原版一致。
- 独立生产验收输出 `LARGE_IMPORT_INTEGRITY_PASS`，耗时 5,796.1 ms。
- Release/Debug/ASan 各 65/65 通过；Release 随机压力复跑 100 轮，共 6,500 项测试全部通过。
- 正式报告和机器汇总分别为 `docs/evidence/part_c_perf_final_1gb_report.txt`、`docs/evidence/part_c_perf_acceptance_summary.json`。

未采用的直接读缓冲解析实验三轮平均约 5,703.030 ms，没有优于当时 P4 的 5,661.871 ms，因此完整撤销。最终版本只保留有独立 A/B 证据且行为回归通过的优化。
