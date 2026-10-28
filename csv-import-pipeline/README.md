# CSV Import Pipeline

一个轻量级的 CSV 数据导入与校验管道库，基于 `fast-cpp-csv-parser` 的设计思路改编。

## 功能概述

- CSV 文件解析（支持 UTF-8 / GBK 编码）
- 可配置的字段校验（类型、范围、正则、必填）
- 坏行跳过、有界内存错误样本与完整 JSONL sidecar
- 同步导入模式
- 后台流式导入、每 1000 行进度回调、协作取消与超时停止
- 1 GB 流式导入、进程峰值内存与完整性验收工具

## 代码架构

整体是一条「解析 → 校验 → 编排」的流水线，三个核心类各管一段，互相之间只通过公开头文件（`include/csvp/`）打交道：

```
             CSV 文件
                │
                ▼
      ┌────────────────────┐
      │      CsvParser     │  打开文件、按分隔符切分字段、逐行产出 Row
      │     (parser.h)     │  维护行号 / 已读字节数
      └─────────┬──────────┘
                │  Row（一行的字段列表，全部是字符串）
                ▼
      ┌────────────────────┐
      │   FieldValidator   │  按列规则（FieldRule）校验每个字段：
      │    (validator.h)   │  类型 / 必填 / 范围 / 正则
      └─────────┬──────────┘
                │  std::vector<CellError>（空 = 该行全部通过）
                ▼
      ┌────────────────────┐
      │   ImportPipeline   │  顶层入口：把上面两步串起来，
      │    (pipeline.h)    │  负责列数检查、错误收集、统计
      └─────────┬──────────┘
                │  Result（导入/跳过/总错误/内存样本/日志完整性/耗时）
                ▼
        调用方（csvp_tests / csvp_bench / 业务代码）
```

### 分层与依赖

| 模块 | 文件 | 职责 | 依赖 |
|---|---|---|---|
| 基础类型 | `include/csvp/types.h` | `ErrorCode`、`FieldType`、`FieldRule`、`CellError`、`RowError`、`Stats` 这些纯数据结构 | 无 |
| 行容器 | `include/csvp/row.h` | `Row`：一行数据的字段列表，提供 `toInt/toDouble/toBool/toString` 类型访问器 | types.h |
| I/O 配置 | `include/csvp/io_config.h` | 编译期有界缓冲配置；默认输入 256 KiB、错误日志 64 KiB | 无 |
| 解析器 | `include/csvp/parser.h` + `src/parser.cpp` | `CsvParser`：`parse()` 打开文件、`readNext()` 以三态结果逐行解析；顺序块读取并复用 Row 字段存储 | io_config.h、row.h |
| 校验器 | `include/csvp/validator.h` + `src/validator.cpp` | `FieldValidator`：规则由外部注入（`addRule/addRules`），`validate()` 校验一整行、`validateField()` 校验单个字段 | types.h、row.h |
| 错误日志 | `include/csvp/error_sink.h` + `src/error_sink.cpp` | `ErrorSink`：以 64 KiB 缓冲顺序写完整 JSONL，原始字节用十六进制无损保存 | io_config.h、types.h |
| 管道 | `include/csvp/pipeline.h` + `src/pipeline.cpp` | `ImportPipeline`：顶层编排，组合 Parser + Validator + ErrorSink，输出 `Result` 和 `Stats` | 上面全部 |

依赖方向是单向的：`types.h` 在最底层，`pipeline` 在最上层；`Parser` 和 `Validator` 互不认识，都只认 `Row` 和 `FieldRule`，由 `Pipeline` 负责把它们接在一起。因此解析和校验都可以单独构建、单独测试。

### 三个实现文件

**`src/parser.cpp` —— 解析器实现**

- `parse()`：关闭上一个打开的文件、以二进制方式打开新文件、重置表头与读取偏移，然后按 `has_header` 读入表头；
- `readNext()`：把下一行读进来，区分成功、EOF 和解析错误，处理行尾、跳过空行，再调用字段切分；
- `readLineFromFile()`：根据当前文件偏移把下一行读入缓冲区，同时更新偏移量和已读字节数（`bytesRead()` 的数据来源）；
- `splitLineToFields()` / `handleQuotedField()`：按 `delimiter` 切分普通字段和引号字段，直接写入可复用的 Row 字段槽；`trim_spaces` 开启时修剪字段前后的空格；
- `encodedCharacterLength()`：验证 UTF-8/GBK 字节序列并按完整字符推进，避免把多字节尾字节误切为分隔符；
- 析构函数负责关闭文件；默认 256 KiB 输入缓冲由 `std::vector` 管理，超长行由 16 MiB 上限约束。

**`src/validator.cpp` —— 校验器实现**

- `addRule()` / `addRules()`：保存外部传入的规则列表。规则是**按列顺序**存放的，校验时用列索引取对应规则；`FieldRule::name` 只用于填充报错信息里的列名；
- `validate()`：逐列校验一行，收集所有非 `Ok` 的结果；实际校验的列数取「行字段数」和「规则数」的较小值；
- `validateField()`：单个字段的检查顺序是 **必填 → 类型 → 范围 → 正则**。必填字段为空直接报 `RequiredFieldMissing`；非必填的空值跳过后续检查直接通过；
- `validateType()`：复用 Row 的严格 `tryTo*` 转换；整数使用 `from_chars`，浮点检查完整消费、范围与有限值，布尔接受 `true/false/TRUE/FALSE/1/0/yes/no`；
- `validateRange()`：只在 `has_range` 为真时生效，按 `min_val/max_val` 比较；
- `validateRegex()`：规则加入时预编译正则，热路径只执行 `regex_match`；非法正则保留可追踪配置错误。

**`src/pipeline.cpp` —— 管道实现**

- 构造函数：保存 `Config` 和规则列表，并把规则交给内部的 `FieldValidator`；
- `importSync()`：预约实例任务状态 → 复用导入核心 → 列数检查和字段校验 → 结束后填写 `Result` 和 `Stats`；
- `validateAndCollect()`：校验通过则 `rows_imported++`；失败则组装包含物理行号和原始 CSV 字节的 `RowError`。启用 `error_log_path` 时先写完整 sidecar，再按 `error_limit` 保留内存样本；
- `importStreaming()`：复制路径和 callback 后启动受管 worker；结果或异常通过 `std::future` 交付，进度每 1000 行报告一次，callback 返回 `false` 时返回部分结果；
- `shutdown()`：发布停止信号并通过条件变量最多等待指定时长；超时后立即返回，`pendingCount()` 可读取尚未完成的后台任务数；
- `stats()`：返回统计快照，流式实现后由原子计数器提供实时进度；日志打开、写入、flush/close 失败会设置 `completed=false` 和失败原因。

### 一行数据的生命周期

以 `importSync("data.csv")` 为例：

1. `ImportPipeline` 用配置里的 `CsvParser::Config` 建一个解析器，`parse()` 打开文件并读出表头（`has_header=true` 时）；
2. 循环调用三态 `readNext(row)`，明确区分成功、正常 EOF 和解析错误；每行先做列数检查，再交给 `FieldValidator::validate(row)`；
3. 校验通过 → `rows_imported++`；校验失败 → 组装 `RowError`，可选写入完整 JSONL，再按配置保留内存样本并继续或失败退出；
4. 循环结束后检查日志 flush/close，把行数、总错误数、日志完整性、字节数和耗时写进 `Result` 与 `Stats`。

错误信息是两层的：`RowError`（哪一行、整行内容）里挂着一组 `CellError`（哪一列、原始值、错误码、消息），`csvp_bench` 的 txt 报告就是按这个结构打印的。

### 并发模型

- `importSync`：在调用方线程上同步执行，跑完才返回；同一实例已有任务时会明确拒绝并发启动。
- `importStreaming`：解析和校验都在成员 worker 线程执行；worker 不 detach，由下一次启动或析构函数负责 join。
- `shutdown`：仅协作通知流式 worker；callback 可读取状态或请求停止，不持有内部状态锁调用用户代码。
- `stats()`：从原子计数器返回实时统计快照；同步与流式任务共享同一组统计，因此同一实例一次只允许一个导入任务。

### 构建产物

| 目标 | 内容 |
|---|---|
| `csvp` | 静态库，包含 Parser、Validator、Pipeline 和 ErrorSink |
| `csvp_tests` | gtest 单元测试，通过 `TEST_DATA_DIR` 宏定位 `data/` 目录 |
| `csvp_bench` | 命令行压测工具，校验规则固定为 5 列（`id/name/age/score/active`），结果写入 txt 报告；用法见 `data/ReadMe.md` |
| `csvp_large_verify` | 指定 1 GB 数据集完整性验收工具；不注册为日常 ctest，避免每次回归读取 1 GB |

CMake 选项：`BUILD_TESTS`、`BUILD_BENCH` 默认开启，`ENABLE_ASAN` 默认关闭；`CSVP_INPUT_BUFFER_KIB` 默认 256，`CSVP_ERROR_LOG_BUFFER_KIB` 默认 64。MSVC 下统一 `/W4 /utf-8 /permissive-`，优化级别跟随 Debug/Release 配置。

## 构建要求

- C++17 或更高
- CMake ≥ 3.16
- MSVC 2019/2022 或 GCC 8+/Clang 7+
- Google Test（已内置于 `third_party/googletest`）

## 快速开始

```bash
# 配置（MSVC）
cmake -S . -B build -G "Visual Studio 17 2022" -A x64

# 构建
cmake --build build --config Release -j

# 运行测试
ctest --test-dir build -C Release --output-on-failure

# 压力复跑（用于发现偶发缺陷）
build/Release/csvp_tests.exe --gtest_repeat=100 --gtest_shuffle

# ASan 构建
cmake -S . -B build-asan -G "Visual Studio 17 2022" -A x64 -DENABLE_ASAN=ON
cmake --build build-asan --config RelWithDebInfo
set ASAN_OPTIONS=halt_on_error=1
build-asan/RelWithDebInfo/csvp_tests.exe

# 基准测试（生成 txt 报告便于观察结果，避免 cout 刷屏）
# 用法：csvp_bench <csv文件> [重复次数] [报告路径.txt] [完整错误日志.jsonl]
build/Release/csvp_bench.exe data/sample_100k.csv 3 build/sample_100k_report.txt build/sample_100k_errors.jsonl

# 报告内容：imported/skipped/total errors、吞吐、峰值内存、日志完整性和错误样本
# 大文件压测后直接打开 txt 报告即可直观核对，无需从刷屏的 cout 里翻找。
```

## API 速览

```cpp
#include <csvp/pipeline.h>

// 定义校验规则
std::vector<csvp::FieldRule> rules = {
    {"id",    csvp::FieldType::Int,    true},
    {"name",  csvp::FieldType::String, true},
    {"age",   csvp::FieldType::Int,    true},
    {"score", csvp::FieldType::Double, false},
};

// 配置管道
csvp::ImportPipeline::Config cfg;
cfg.parser.has_header = true;
cfg.skip_bad_rows = true;
cfg.error_limit = 200;
cfg.error_log_path = "errors.jsonl";

// 执行导入
csvp::ImportPipeline pipeline(cfg, rules);
auto result = pipeline.importSync("data.csv");

std::cout << "Imported: " << result.rows_imported
          << ", Skipped: " << result.rows_skipped
          << ", Errors: " << result.total_error_count
          << ", Time: " << result.elapsed_ms << " ms\n";
```

## 已知限制

以下限制来自 `fast-cpp-csv-parser` 原项目的设计边界：

- 单行最大长度 16 MB
- 引号转义仅支持 `""` 形式
- 不支持字段内嵌换行
- 编码支持限定为 UTF-8/UTF8/GBK；不会自动探测编码

## 缺陷定位指引

> 本仓库最初有意保留了若干缺陷。Part A 的修复过程、回归用例和验证结果记录在 [docs/PART_A_EVIDENCE.md](docs/PART_A_EVIDENCE.md)。
>
> Part A/B/C 的回归用例均已启用；实现、测试、性能原始数据和提交证据分别记录在 [Part A](docs/PART_A_EVIDENCE.md)、[Part B](docs/PART_B_EVIDENCE.md) 和 [Part C](docs/PART_C_EVIDENCE.md)。

## 目录结构

```
csv-import-pipeline/
├── CMakeLists.txt
├── build_msvc.bat             # 一键配置 + 构建（MSVC）
├── README.md
├── docs/
│   ├── PART_A_EVIDENCE.md      # Part A 修复与验证证据
│   ├── PART_B_EVIDENCE.md      # Part B 线程、停止与验证证据
│   ├── PART_C_EVIDENCE.md      # Part C 1 GB、内存与性能证据
│   ├── evidence/               # 原始报告、JSON 汇总、测试附件
│   └── REPORT_TEMPLATE.md      # 交付报告模板
├── include/csvp/
│   ├── io_config.h       # 有界 I/O 缓冲配置
│   ├── error_sink.h      # 完整错误 sidecar
│   ├── parser.h          # CSV 解析器
│   ├── row.h             # 行数据容器
│   ├── validator.h       # 字段校验器
│   ├── pipeline.h        # 导入管道
│   └── types.h           # 基础类型
├── src/
│   ├── error_sink.cpp
│   ├── parser.cpp
│   ├── validator.cpp
│   └── pipeline.cpp
├── tests/
│   ├── test_parser.cpp
│   ├── test_validator.cpp
│   ├── test_pipeline.cpp
│   └── test_concurrent.cpp
├── bench/
│   ├── bench_import.cpp
│   ├── process_memory.cpp
│   └── verify_large_import.cpp
└── data/
    ├── ReadMe.md              # ⭐ 样本一览与运行方法（含 VS 调试配置）
    ├── sample_utf8.csv       # 正常数据
    ├── sample_noeol.csv      # 无尾换行
    ├── sample_gbk.csv        # GBK 编码
    ├── sample_dirty.csv      # 脏数据
    ├── sample_quote.csv      # 引号转义
    ├── sample_crlf.csv       # CRLF/LF 混用
    ├── sample_100k.csv       # 10 万行压测
    ├── sample_1gb.txt        # Part C：1 GB 数据集的本机路径说明
    └── demo_err.csv          # 带校验错误样本（演示报告错误明细）
```

## 许可证

本项目基于 BSD-3-Clause 许可发布，继承自 fast-cpp-csv-parser。
