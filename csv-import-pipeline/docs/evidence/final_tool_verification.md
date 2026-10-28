# 最终工具验收摘要

日期：2026-10-28（Asia/Shanghai）

## 常规回归

```powershell
ctest --test-dir build-review-baseline -C Release --output-on-failure `
  --output-junit docs/evidence/final_ctest_release.xml
ctest --test-dir build-review-baseline -C Debug --output-on-failure `
  --output-junit docs/evidence/final_ctest_debug.xml
```

- Release：65 tests，0 failures，0 disabled，退出码 0。
- Debug：65 tests，0 failures，0 disabled，退出码 0。
- XML 附件：`final_ctest_release.xml`、`final_ctest_debug.xml`。

## MSVC AddressSanitizer

```powershell
cmake -S . -B build-review-asan-msvc -DENABLE_ASAN=ON `
  -DCSVP_INPUT_BUFFER_KIB=256 -DCSVP_ERROR_LOG_BUFFER_KIB=64
cmake --build build-review-asan-msvc --config Debug --parallel
$env:ASAN_OPTIONS = "halt_on_error=1"
build-review-asan-msvc/Debug/csvp_tests.exe `
  --gtest_output=xml:docs/evidence/final_asan_tests.xml
```

结果：65/65 通过，0 failures，进程退出码 0，AddressSanitizer 错误 0。LNK4300 只说明含 ASan 元数据时忽略 `/INCREMENTAL`，不属于 C++ 源码警告。

## 随机顺序压力复跑

```powershell
build-review-baseline/Release/csvp_tests.exe `
  --gtest_repeat=100 --gtest_shuffle --gtest_brief=1
```

结果：100/100 轮通过，每轮 65 项，共 6,500 项，失败 0，退出码 0。

## clang-tidy

```powershell
D:/llvm/Installed/bin/clang-tidy.exe `
  src/error_sink.cpp src/parser.cpp src/validator.cpp src/pipeline.cpp `
  bench/bench_import.cpp bench/process_memory.cpp bench/verify_large_import.cpp `
  -checks="-*,clang-analyzer-*,bugprone-*" -- `
  -std=c++17 -Iinclude -Ibench -DWIN32 -D_WINDOWS `
  -DCSVP_INPUT_BUFFER_BYTES=262144U -DCSVP_ERROR_LOG_BUFFER_BYTES=65536U
```

首轮在 `verify_large_import.cpp` 检出 2 项：字符位运算类型、`main` 异常逃逸。前者改为先提升到 `unsigned int`；后者增加顶层 `try/catch`，并对 clang-tidy 不能证明的入口边界保留一个带英文理由的精确 `NOLINT`。修复后 7 个项目源文件可见告警 0、错误 0；系统/第三方告警被工具抑制。

## 1 GB 验收工具修复后复核

```powershell
build-review-baseline/Release/csvp_large_verify.exe `
  data/sample_1gb.csv build-review-baseline/part_c_c8_integrity_errors.jsonl
python tools/verify_error_log.py `
  build-review-baseline/part_c_c8_integrity_errors.jsonl
```

结果：imported=9,990,000，skipped=10,000，total_errors=10,000，total_lines=10,000,000，`error_log_complete=true`，输出 `LARGE_IMPORT_INTEGRITY_PASS`。独立日志复核为 10,000 条、3,896,682 bytes、mismatch=0，SHA-256 `C237216C682B60FDF8B633807D023E52BDF5778C769DD9599A267D73C1BF9E4C`。

## 证据关系

- 本文件保存最终命令与摘要；XML 保存逐测试结果。
- C7 正式三轮性能与内存数字以 `part_c_c7_final_1gb_report.txt` 和 `part_c_c7_acceptance_summary.json` 为准。
- 每个缺陷/特性的逐提交复现、根因和验证分别见 `PART_A_EVIDENCE.md`、`PART_B_EVIDENCE.md`、`PART_C_EVIDENCE.md`。
