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
