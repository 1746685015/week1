# data 目录说明

本目录保存单元测试和性能实验样本。CSV 文件不参与库编译；测试通过 CMake 写入的 `TEST_DATA_DIR` 定位它们，benchmark 则直接接收命令行路径。

## 样本一览

| 文件 | 内容 | 主要用途 |
|---|---|---|
| `sample_utf8.csv` | UTF-8 表头 + 10 条合法数据 | 基本解析、表头、同步导入 |
| `sample_noeol.csv` | 末行没有换行符 | EOF 边界回归 |
| `sample_gbk.csv` | 合法及非法 GBK 字节序列 | 编码验证与分隔符边界 |
| `sample_quote.csv` | `""` 转义引号 | CSV 引号字段回归 |
| `sample_crlf.csv` | CRLF、LF 与字段内回车边界 | 行尾处理回归 |
| `sample_dirty.csv` | 列数、类型、必填和布尔错误 | 跳过坏行、错误上限 |
| `demo_err.csv` | 小型错误报告演示 | benchmark 输出示例 |
| `sample_100k.csv` | 100,000 条数据，其中 2,000 条非法 | 并发、进度、性能实验 |
| `sample_1gb.txt` | 本机 1 GB 数据集路径说明 | 不提交大文件本体 |

本机 `sample_1gb.csv` 的正式规格为：

- 1,003,898,922 bytes；
- SHA-256 `89FA6C306B9730EA71A782DA699C64CEC1EFC6B4097852BB457BC30ABC9477D0`；
- 表头 `id,name,age,score,active`；
- 10,000,000 条数据，其中 id 能被 1000 整除的 10,000 条记录使用非法 `age=N/A`。

该 CSV 被 `.gitignore` 排除，仓库保存路径说明、验证脚本、完整错误日志规范副本和实验报告。运行 1 GB 验收前应先执行 `tools/validate_large_dataset.py` 或核对文件大小与 SHA。

## 构建与日常测试

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Visual Studio 是多配置生成器，`ctest` 的 `-C Release` 或 `-C Debug` 不能省略。当前所有 Part A/B/C 日常用例均已启用，没有依赖 `DISABLED_` 才能发现的已知缺陷。

定向与压力测试示例：

```powershell
build/Release/csvp_tests.exe --gtest_filter="Parser.*:Pipeline.*"
build/Release/csvp_tests.exe --gtest_repeat=100 --gtest_shuffle
```

## benchmark 用法

```text
csvp_bench <csv_file> [repeat_count] [report_path] [error_log_path]
```

- `repeat_count` 默认 3，必须是正整数；
- `report_path` 默认 `bench_report.txt`；
- 提供 `error_log_path` 时，每条错误都会写入完整 JSONL，内存仍只保留有限样本；
- 多轮运行会为日志追加 `.run1`、`.run2` 等后缀，防止后一次覆盖前一次；
- 报告包含每轮 imported、skipped、processed、total errors、内存样本数、耗时、rows/s、MB/s、峰值工作集和日志完整性。

```powershell
# 100k，三轮，保留完整错误日志
build/Release/csvp_bench.exe `
  data/sample_100k.csv 3 build/sample_100k_report.txt build/sample_100k_errors.jsonl

# 指定 1 GB 文件正式三轮验收
build/Release/csvp_bench.exe `
  data/sample_1gb.csv 3 build/sample_1gb_report.txt build/sample_1gb_errors.jsonl

# 独立核对完整错误日志
python tools/verify_error_log.py build/sample_1gb_errors.jsonl.run1
```

性能比较必须使用同一机器、同一 Release 配置、同一文件和相同错误策略。Debug 数字不能与 Release 直接比较。

## 1 GB 完整性验收

日常 `ctest` 不读取 1 GB 文件。最终候选版本使用独立工具：

```powershell
build/Release/csvp_large_verify.exe `
  data/sample_1gb.csv build/sample_1gb_errors.jsonl
```

成功时输出 `LARGE_IMPORT_INTEGRITY_PASS`，并精确核对：

- imported = 9,990,000；
- skipped = total errors = 10,000；
- processed = 10,000,000；
- 首条、中间、末条坏行的物理行号、id、原始内容和错误字段；
- sidecar 完整结束。

正式三轮结果和峰值内存证据见 `docs/PART_C_EVIDENCE.md` 与 `docs/evidence/part_c_c7_*`。

## 添加小型样本

1. 在 `data/` 新增尽量小的 CSV；
2. 在 `tests/` 中使用 `TEST_DATA_DIR` 拼路径并增加精确断言；
3. 重新配置/构建后运行定向用例；
4. 更新上方样本表，说明文件字节特征与测试目的。

边界样本可能故意包含非法编码或特殊行尾。不要用普通编辑器无意转换编码/换行；必要时使用 PowerShell `Format-Hex` 核对原始字节。
