# Part A 修复与验证证据

本文记录 Part A 每个缺陷的复现、根因、修复、测试结果和 Git 提交，作为可复现验收证据。

## 基线

- 平台：Windows 11 x64
- 编译器：MSVC 19.44.35229（Visual Studio 2022 17.14）
- Windows SDK：10.0.22621.0
- CMake：3.28.1
- 基线提交：`420d16f9673b49b940c98ecd4a9f3c517beef4e3`
- Release 常规测试：19 项实际执行，17 项通过，`Pipeline.SkipBadRows` 失败，`Pipeline.ErrorLimit` 跳过；6 项 disabled 未执行。
- Release 强制执行 disabled：25 项中 18 项通过、6 项失败、1 项跳过。

基线命令：

```powershell
cmake -S . -B build-review-baseline -G "Visual Studio 17 2022" -A x64
cmake --build build-review-baseline --config Release --parallel
ctest --test-dir build-review-baseline -C Release --output-on-failure
build-review-baseline/Release/csvp_tests.exe --gtest_also_run_disabled_tests
```

## A-0：MSVC ASan 测试目标无法链接

### 复现

安装 x64 AddressSanitizer 组件后，以 `-DENABLE_ASAN=ON` 构建 `csvp_tests`，链接阶段报告：

- `annotate_string` / `annotate_vector` 不匹配；
- `/fsanitize=address` 被 `link.exe` 识别为无效选项。

### 根因

项目只给 `csvp` 和 `csvp_tests` 设置了 ASan 编译选项，内嵌的 GoogleTest 仍按普通 STL 注解模式编译，导致同一可执行文件中的对象文件 ABI 检测值不一致。MSVC 的 ASan 运行库由编译器插入链接指令，不应把 `/fsanitize=address` 作为原始链接器选项传给 `link.exe`。

### 修复

- 给 `gtest`、`gtest_main`、`gmock`、`gmock_main` 使用相同的 MSVC ASan 编译选项。
- `csvp` 的 ASan 选项改为私有，测试和第三方目标分别显式配置。
- 移除 MSVC 目标上的无效 `/fsanitize=address` 链接选项。

### 验证

命令：

```powershell
cmake -S . -B build-review-asan-msvc -G "Visual Studio 17 2022" -A x64 `
    -DENABLE_ASAN=ON -DBUILD_BENCH=OFF
cmake --build build-review-asan-msvc --config Debug --target csvp_tests --parallel --clean-first
$env:PATH = "D:\vs2022application\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64;$env:PATH"
$env:ASAN_OPTIONS = "halt_on_error=1"
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Parser.UTF8Basic
```

结果：ASan Debug 构建和链接成功；`Parser.UTF8Basic` 通过，进程退出码为 0，未产生 ASan 报告。

### Git 证据

Git 标签：`part-a-a0-asan-build`。

## A-1：文件末尾无换行时最后一行丢失

### 复现

修复前强制运行 `Parser.DISABLED_NoTrailingNewline`，实际只读取 2 行，预期为 3 行。

### 根因

`CsvParser::readLineFromFile()` 在读取块中找不到 `\n` 时总是返回 `false`。当该块已经到达文件末尾时，缓冲区中的最后一行有效数据因此未交付给调用方。

### 修复

- 找不到 `\n` 且已到达文件末尾时，将当前缓冲区作为最后一行返回。
- 找不到 `\n` 且尚未到文件末尾时，明确报告单行超过 16 MB 限制，避免把超长行误报为正常 EOF。
- 启用 `Parser.NoTrailingNewline`，并精确断言最后一行字段为 `3,gamma,300`。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Parser.NoTrailingNewline
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Parser.NoTrailingNewline
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Parser.NoTrailingNewline
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：

- Release 定向测试：1/1 通过。
- Debug 定向测试：1/1 通过。
- ASan Debug 定向测试：1/1 通过，零 ASan 报告。
- Release/Debug 全量回归：A-1 已通过；当前各剩余 1 项已知失败 `Pipeline.SkipBadRows`，`Pipeline.ErrorLimit` 仍因该缺陷跳过，其他已启用测试通过。该失败属于后续 A-3，不是 A-1 回归。

### Git 证据

Git 标签：`part-a-a1-no-trailing-newline`。

## A-2：解析器重复使用时状态残留和缓冲区泄漏

### 复现

修复前强制运行 `Parser.DISABLED_ReentryState`：第一次解析结束时行号为 11，第二次解析另一个文件后行号累计到 14；每次 `parse()` 又分配新的 16 MB 缓冲区，却没有先释放旧缓冲区。

### 根因

`parse()` 只关闭旧文件并清空表头/偏移，没有重置 `line_number_`、`bytes_read_`、`last_error_`，也没有释放已有的 `line_buffer_`。缓冲区分配失败或表头读取失败时，已打开文件也未立即关闭。

### 修复

- 每次解析前关闭旧文件并释放旧缓冲区。
- 在打开新文件前统一清空文件大小、偏移、表头、行号、字节数和错误状态。
- 缓冲区分配失败或表头读取失败时立即关闭文件。
- 启用 `Parser.ReentryState`，精确检查两个文件各自的行数、行号和读取字节数。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Parser.ReentryState
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Parser.ReentryState
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Parser.ReentryState
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release、Debug、ASan Debug 定向测试均为 1/1 通过，ASan 零报告。两种普通配置的全量回归均执行 21 项：20 项通过，仅后续 A-3 的 `Pipeline.SkipBadRows` 失败；`Pipeline.ErrorLimit` 因同一缺陷跳过。

### Git 证据

Git 标签：`part-a-a2-reentry-state`。

## A-3：跳过坏行模式仍因列数错误中断

### 复现

修复前 `Pipeline.SkipBadRows` 在第 4 行列数为 6、表头列数为 5 时抛出 `runtime_error`。`Pipeline.ErrorLimit` 捕获该异常后调用 `GTEST_SKIP()`，没有真正验证错误上限。

### 根因

`importSync()` 的列数检查无条件抛异常，没有读取 `skip_bad_rows`。该路径也没有生成 `RowError`，所以即使调用方捕获异常，也拿不到错误行证据。

### 修复

- 非跳过模式保持立即失败的既有行为。
- 跳过模式为列数不匹配创建 `ColumnCountMismatch` 错误，遵守 `error_limit` 后继续读取下一行。
- 加强 `Pipeline.SkipBadRows`：精确断言 2 行导入、5 行跳过、5 条错误及首条错误行号/错误码。
- 删除 `Pipeline.ErrorLimit` 的异常捕获和 `GTEST_SKIP()`，精确断言只保存 3 条错误但仍统计 5 行跳过。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Pipeline.SkipBadRows:Pipeline.ErrorLimit
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Pipeline.SkipBadRows:Pipeline.ErrorLimit
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Pipeline.SkipBadRows:Pipeline.ErrorLimit
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release、Debug、ASan Debug 定向测试均为 2/2 通过，ASan 零报告。Release 与 Debug 全量回归均执行 21 项，21 项全部通过；剩余 4 项因尚未进入对应 Part A/Part B 修复而保持 disabled。

### Git 证据

Git 标签：`part-a-a3-skip-bad-rows`。

## A-4：GBK/UTF-8 多字节边界与测试样本错误

### 复现与样本核对

原 `Parser.DISABLED_ChineseGBKEncoding` 期望 `sample_gbk.csv` 第二条数据解析为 4 列。十六进制检查显示该行含 `D4 2C`：GBK 尾字节合法范围是 `0x40-0xFE`（排除 `0x7F`），所以 `0x2C` 不是合法尾字节，该样本本身是非法 GBK，不能把逗号强行视作汉字的一部分。

同时，原解析器完全没有使用 `Config::encoding` 或 `isTrailingByte()`，任何字节都逐个与分隔符比较。

### 修复

- `parse()` 只接受 UTF-8/UTF8/GBK 配置，未知编码立即报错。
- 切分普通字段和引号字段时验证 UTF-8 或 GBK 字节序列，并按完整字符推进。
- GBK 双字节字符的合法尾字节即使等于自定义分隔符，也不会被误切。
- 修正原测试：有效的第一行应解析为 4 列；含 `D4 2C` 的第二行必须明确返回 `Invalid GBK byte sequence`。
- 新增合法字节级用例：使用反斜杠 `0x5C` 作分隔符，同时构造合法 GBK 双字节 `81 5C`，断言字符内部的 `0x5C` 不被误切。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Parser.ChineseGBKEncoding:Parser.GbkTrailingByteMatchesDelimiter
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Parser.ChineseGBKEncoding:Parser.GbkTrailingByteMatchesDelimiter
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Parser.ChineseGBKEncoding:Parser.GbkTrailingByteMatchesDelimiter
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release、Debug、ASan Debug 定向测试均为 2/2 通过，ASan 零报告。Release 与 Debug 全量回归均执行 23 项，23 项全部通过；仅 A-8、A-9 和 Part B 的停止测试仍保持 disabled。

### Git 证据

Git 标签：`part-a-a4-gbk-boundaries`。

## A-5：类型转换无法区分真实零值与失败

### 复现

原 `Row::toInt()` 对空值、非法字符串、越界索引和真实 `0` 都返回 0；`toDouble()` 使用 `atof()`，同样无法报告失败，也不检查溢出或 `nan/inf`。`toBool()` 把合法 `false` 和任意非法文本都返回为 `false`。

### 根因

原接口只返回数值，没有错误通道；整数异常被全部捕获并静默转为 0，浮点使用无法报告转换状态的 `atof()`。

### 修复

- 新增 `tryToInt(index, value)`：使用 `from_chars`，要求完整消费字符串并检查溢出。
- 新增 `tryToDouble(index, value)`：检查完整消费、`ERANGE`、有限值及首尾空白。
- 新增 `tryToBool(index, value)`：合法真值与假值都返回成功，非法文本返回失败。
- 保留原 `toInt/toDouble/toBool` 的兼容回退值，避免破坏已有调用方；需要区分失败的新代码必须使用 `tryTo*`。
- 头文件同步改为可移植 include guard，并按 WPS C++ 规范整理本次涉及代码。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Row.TypedConversions
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Row.TypedConversions
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Row.TypedConversions
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release、Debug、ASan Debug 定向测试均为 1/1 通过，ASan 零报告。Release 与 Debug 全量回归均执行 24 项，24 项全部通过。

### Git 证据

Git 标签：`part-a-a5-safe-conversions`。

## A-6：固定大缓冲与重复读取导致内存和性能异常

### 复现

旧实现为每个解析器固定 `malloc` 16 MiB。每读取一行，都从当前偏移重新 `fseek`，再最多读取约 16 MiB，但只消费其中第一行；短行文件因此反复读取和复制相同后续数据。

修复前对 `sample_100k.csv` 重复 3 次的 Release 基线：

- 平均耗时：28,530.1 ms
- 吞吐：3,434 行/秒
- 每次结果：98,000 行导入、2,000 行跳过、2,000 条错误

### 修复

- 使用固定 64 KiB 顺序读取块，保留块中尚未消费的数据，不再逐行 seek/重复读取。
- 当前行使用 `std::string` 按需增长，不再为每个解析器预分配 16 MiB 裸缓冲。
- 检查 `fread` 的错误状态；单行超过 16 MiB 时返回明确错误，不静默截断。
- 删除手动 `malloc/free`，禁止解析器复制，避免文件句柄或缓冲区产生双重所有权。
- 新增跨越 64 KiB 块边界的 100,000 字节字段测试，逐字节核对字段内容。
- `parser.h`/`parser.cpp` 按 WPS C++ 规范整理为 include guard、Tab、独占行花括号、成员 `m_` 前缀及英文注释。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Parser.LineCrossesReadBufferBoundary
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Parser.LineCrossesReadBufferBoundary
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Parser.LineCrossesReadBufferBoundary
build-review-baseline/Release/csvp_bench.exe data/sample_100k.csv 3 data/a6_optimized_report.txt
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：

- Release、Debug、ASan Debug 跨块测试均为 1/1 通过，ASan 零报告。
- Release 与 Debug 全量回归均执行 25 项，25 项全部通过。
- 优化后 3 次平均 107.16 ms，吞吐 914,516 行/秒；结果仍为 98,000 行导入、2,000 行跳过、2,000 条错误。
- 同一设备、同一文件、同一 Release 命令下，相对旧实现加速约 266.2 倍。
- 基准进程观测到的峰值工作集约 5.37 MiB。此数据证明 10 万行样本的内存行为；1 GB 文件小于 50 MB 的最终证明仍属于 Part C，不能用本结果替代。

### Git 证据

Git 标签：`part-a-a6-buffered-io`。

## A-7：行号类型可能在超大文件中溢出

### 复现与根因

解析器内部行号及 `lineNumber()` 返回值原为 32 位 `int`，而 `RowError::line_number` 和管道校验接口使用平台相关的 `size_t`。超过 `INT_MAX` 行的输入会使解析器计数溢出，并且接口之间的类型不一致会隐藏截断风险。

### 修复

- 将解析器行号成员和 `lineNumber()` 返回值统一为固定宽度的 `std::uint64_t`。
- 将 `RowError::line_number` 与管道内部传递行号的参数同步改为 `std::uint64_t`。
- 删除从解析器行号到 `size_t` 的多余强制转换。
- 新增编译期类型断言，并在运行时验证能够保存 `UINT32_MAX + 1`（4,294,967,296）。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Parser.LineNumberUsesFixedWidthType
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Parser.LineNumberUsesFixedWidthType
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Parser.LineNumberUsesFixedWidthType
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release、Debug、ASan Debug 定向测试均为 1/1 通过，ASan 零报告；Release 与 Debug 全量回归均执行 26 项，26 项全部通过。仅 A-8、A-9 和 Part B 的关闭测试仍保持 disabled。

### Git 证据

Git 标签：`part-a-a7-wide-line-number`。

## A-8：CSV 转义双引号及字段边界解析错误

### 复现与根因

旧 `handleQuotedField()` 遇到第一个 `"` 就结束字段，无法按 CSV 规则把 `""` 还原为单个字面双引号。因此 `"he said ""hi"""` 会被提前截断。原字段循环还允许双引号出现在未加引号字段中，并在引号字段位于行末或行尾有分隔符时存在重复追加/漏加空字段的边界风险。

### 修复

- 引号字段中的连续 `""` 解析为一个字面 `"`，只有非转义的单个 `"` 才结束字段。
- 按“每次完整解析一个字段”的方式重写切分循环，明确处理引号字段、普通字段、分隔符和行尾。
- 支持空引号字段、引号内分隔符、位于行末的引号字段及末尾空字段。
- 未加引号字段中出现 `"`、引号未闭合、结束引号后存在非法字符时返回明确错误。
- 启用原 `QuotedEscapedQuote` 用例，并新增字段边界与非法引号测试。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Parser.QuotedEscapedQuote:Parser.QuotedFieldBoundaries:Parser.RejectsMalformedQuotes
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Parser.QuotedEscapedQuote:Parser.QuotedFieldBoundaries:Parser.RejectsMalformedQuotes
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Parser.QuotedEscapedQuote:Parser.QuotedFieldBoundaries:Parser.RejectsMalformedQuotes
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release、Debug、ASan Debug 定向测试均为 3/3 通过，ASan 零报告；Release 与 Debug 全量回归均执行 29 项，29 项全部通过。仅 A-9 和 Part B 的关闭测试仍保持 disabled。

### Git 证据

Git 标签：`part-a-a8-quoted-fields`。

## A-9：CRLF/LF 混用时错误删除字段中的回车符

### 复现与样本审计

旧 `readRow()` 只要发现行缓冲最后一个字节为 `\r` 就无条件删除，无法区分它是 CRLF 行结束符的一部分，还是无末尾换行时字段本身的最后一个字节。

同时，仓库原 `sample_crlf.csv` 经十六进制检查只有 `0A`，没有任何 `0D` 字节；原关闭测试声称第二行包含 `has_cr\r`，但样本实际只有 `has_cr\n`。因此该样本不能作为 CRLF 验收证据，测试预期也与真实字节不一致。

### 修复

- 读取一行时记录是否确实遇到了 `\n`。
- 只有“本行由 `\n` 结束且 `\n` 前一字节是 `\r`”时才删除该 `\r`。
- EOF 前没有 `\n` 的独立末尾 `\r` 保留为字段内容；引号字段内部的 `\r` 同样原样保留。
- 将原测试替换为运行时按二进制写入的精确样本，覆盖 CRLF 表头、纯 LF 数据行、CRLF 数据行、引号字段内部 CR，以及无末尾换行且以 CR 结束的最后一行。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Parser.CRLFLFMixedCR
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Parser.CRLFLFMixedCR
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Parser.CRLFLFMixedCR
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
build-review-asan-msvc/Debug/csvp_tests.exe
```

结果：Release、Debug、ASan Debug 定向测试均为 1/1 通过；Release 与 Debug 全量回归均为 30/30，ASan Debug 全量回归为 30/30 且零报告。Part A 相关关闭测试已全部启用，当前仅 Part B 的 `Concurrent.DISABLED_ShutdownNoDeadlock` 保持关闭。

### Git 证据

Git 标签：`part-a-a9-line-endings`。

## A-10：无表头模式把正常数据行误判为列数错误

### 复现与根因

`importSync()` 原来始终用 `parser.header().size()` 校验数据行列数。配置 `has_header=false` 时表头必然为空，因此任何非空数据行都会与期望的 0 列不一致，无法正常导入。

### 修复

- 有表头时继续以表头列数作为期望列数。
- 无表头时以构造管道时传入的字段规则数量作为显式 schema 列数。
- 新增无表头测试：三列规则下 3 条合法行成功导入，1 条两列坏行被跳过并记录为第 3 行的 `ColumnCountMismatch`。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Pipeline.NoHeaderUsesRuleCount
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Pipeline.NoHeaderUsesRuleCount
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Pipeline.NoHeaderUsesRuleCount
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release、Debug、ASan Debug 定向测试均为 1/1 通过，ASan 零报告；Release 与 Debug 全量回归均为 31/31。

### Git 证据

Git 标签：`part-a-a10-no-header-schema`。

## A-11：解析失败被当作正常 EOF，导致后续数据静默丢失

### 复现与根因

旧 `readRow()` 对正常 EOF、未打开文件、I/O 错误和 CSV 格式错误都返回 `false`。`importSync()` 使用 `while (parser.readRow(row))`，因此文件中间出现未闭合引号等解析错误时会直接结束循环，并把错误后的合法行静默丢弃。

### 修复

- 新增 `ReadStatus` 三态读取结果，明确区分成功行、EOF 和错误。
- 保留原 `readRow()` 作为兼容接口，管道改用三态 `readNext()`。
- 跳过坏行模式下，解析错误记录为 `ParseError`，遵守 `error_limit`，然后继续读取下一物理行。
- 非跳过模式下，解析错误立即抛出包含行号和具体解析原因的异常。
- 表头读取失败时保留底层解析原因，不再统一覆盖成模糊信息。
- 新增中间坏行测试，验证坏行后的合法行仍被导入，并验证严格模式明确失败。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Pipeline.ParseErrorDoesNotBecomeEof
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Pipeline.ParseErrorDoesNotBecomeEof
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Pipeline.ParseErrorDoesNotBecomeEof
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release、Debug、ASan Debug 定向测试均为 1/1 通过，ASan 零报告；Release 与 Debug 全量回归均为 32/32。测试中的第 3 行解析错误被记录，最后一行仍成功导入。

### Git 证据

Git 标签：`part-a-a11-read-status`。

## A-12：错误记录中的 `raw_line` 不是原始 CSV 行

### 复现与根因

管道原来把解析后的字段固定用逗号重新拼接为 `raw_line`。该过程会丢失自定义分隔符、字段引号、转义双引号、引号内分隔符和被 trim 的空白，解析错误路径甚至只能写入“原始行不可用”的占位文本。

### 修复

- 解析器新增输出参数接口，复制当前物理行的原始字节（不包含 CRLF/LF 行结束符）。
- 解析错误、列数错误和字段校验错误统一从解析器取得原始行，不再重新序列化字段。
- 使用输出参数而不是暴露内部字符串引用，保持解析器缓冲区所有权明确。
- 新增分号分隔测试，逐字节检查包含引号、引号内分号、转义双引号和尾随空格的错误行。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Pipeline.ErrorPreservesRawCsvLine:Pipeline.ParseErrorDoesNotBecomeEof
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Pipeline.ErrorPreservesRawCsvLine:Pipeline.ParseErrorDoesNotBecomeEof
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Pipeline.ErrorPreservesRawCsvLine:Pipeline.ParseErrorDoesNotBecomeEof
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release、Debug、ASan Debug 定向测试均为 2/2 通过，ASan 零报告；Release 与 Debug 全量回归均为 33/33。测试对 `raw_line` 的字节长度和内容进行了精确相等比较。

### Git 证据

Git 标签：`part-a-a12-raw-line`。

## A-13：校验器静默忽略 schema 与数据列数差异

### 复现与根因

`FieldValidator::validate()` 原来只遍历 `min(row.size(), rules.size())`。数据列多于规则时，多余字段完全不校验；规则多于数据列时，缺失字段也不产生错误。管道有表头时只检查数据行与表头列数，未验证表头与规则数量是否一致。

### 修复

- 管道读取表头后立即检查表头列数与规则数；不一致时不读取数据行，返回包含两侧数量的 schema 错误。
- 校验器对多余数据列产生 `ColumnCountMismatch`，记录列索引和原始值。
- 校验器对缺失规则列产生 `ColumnCountMismatch`，记录列索引和规则名称。
- 新增规则少于表头、规则多于表头，以及独立 Row 多列/少列测试。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Validator.ReportsSchemaColumnMismatch:Pipeline.RejectsHeaderRuleCountMismatch
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Validator.ReportsSchemaColumnMismatch:Pipeline.RejectsHeaderRuleCountMismatch
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Validator.ReportsSchemaColumnMismatch:Pipeline.RejectsHeaderRuleCountMismatch
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release、Debug、ASan Debug 定向测试均为 2/2 通过，ASan 零报告；Release 与 Debug 全量回归均为 35/35。

### Git 证据

Git 标签：`part-a-a13-schema-contract`。

## A-14：数值溢出和非有限浮点值绕过校验

### 复现与根因

旧整数校验使用 `strtol()` 但不检查 `errno`，浮点校验使用 `strtod()` 但同样不检查上溢/下溢，并允许 `nan`、`inf`。NaN 与上下界比较均为 false，可绕过范围限制。整数范围又把 `double` 上下界强制转为平台宽度不一致的 `long`，存在截断和跨平台差异。

### 修复

- 整数使用 `std::from_chars` 完整转换为固定宽度 `std::int64_t`，检查全部字符和溢出错误。
- 浮点使用完整转换并检查 `ERANGE` 与 `std::isfinite()`，拒绝 `nan`、`±inf` 和 `1e9999`。
- 数值文本不接受前导空白，避免 C 转换函数静默跳过字符。
- 整数与浮点范围统一转换为 `long double` 后比较，不再把配置边界截断成 `long`。
- 测试覆盖 64 位整数上下限及正负溢出、负零、非有限值和范围精确边界。
- 本次涉及的 `validator.h/.cpp` 按 WPS 规范整理为 include guard、Tab、独占行花括号、成员 `m_` 前缀和英文注释/错误文本。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Validator.RejectsNumericOverflowAndNonFiniteValues:Validator.NumericRangeIncludesExactBoundaries
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Validator.RejectsNumericOverflowAndNonFiniteValues:Validator.NumericRangeIncludesExactBoundaries
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Validator.RejectsNumericOverflowAndNonFiniteValues:Validator.NumericRangeIncludesExactBoundaries
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release、Debug、ASan Debug 定向测试均为 2/2 通过，ASan 零报告；Release 与 Debug 全量回归均为 37/37。

### Git 证据

Git 标签：`part-a-a14-numeric-validation`。

## A-15：异常分支跳过坏行但不保存错误

### 复现与根因

旧 `validateAndCollect()` 用 `catch (const std::runtime_error &)` 包住整个校验流程。跳过模式进入该分支后只创建一个局部 `RowError`，既不写入 `result.errors`，也不保存异常原因，并用“原始行不可用”的占位文本覆盖事实。

源码进一步核对发现，当前真实可预期的正则异常已由 `FieldValidator` 转换成 `RegexMismatch`；旧 catch 分支没有正常业务触发源，却可能在未来静默吞掉意外异常。

### 修复

- 删除吞掉 `std::runtime_error` 的分支。
- 可预期校验失败统一由 `CellError` 返回并保存；严格模式保存错误后明确失败，跳过模式遵守 `error_limit` 并继续。
- 不可预期异常不再伪装成成功跳过，避免丢失真实原因。
- 新增非法正则测试，核对跳过数、错误数、行号、原始行、`RegexMismatch` 和具体错误消息。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Pipeline.InvalidRegexErrorIsTraceable
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Pipeline.InvalidRegexErrorIsTraceable
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Pipeline.InvalidRegexErrorIsTraceable
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release、Debug、ASan Debug 定向测试均为 1/1 通过，ASan 零报告；Release 与 Debug 全量回归均为 38/38。

### Git 证据

Git 标签：`part-a-a15-traceable-errors`。

## A-16：同步导入统计错误且 `stats()` 存在数据竞争

### 复现与根因

`importSync()` 原来更新普通成员 `stats_`，但 `stats()` 随后用从未由同步导入更新的 `hot_*` 原子计数覆盖行数，导致同步导入成功后查询仍返回 0。同时，一个线程写普通 `stats_`、另一个线程复制它属于无同步读写，存在数据竞争。

### 修复

- 删除共享的普通 `Stats` 对象，行数、字节数、导入数、跳过数和耗时分别使用原子成员保存。
- 同步导入开始时重置全部统计；每处理一行实时更新行数、字节数及结果计数。
- 文件打开失败、schema 失败和正常完成路径都更新耗时及适用统计。
- `stats()` 只读取原子成员，不再复制正在被另一线程修改的普通对象。
- 新增同步结果一致性测试，以及导入 10 万行期间并发轮询统计的测试。

### 验证

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Concurrent.SyncImportStatsMatchResult:Concurrent.ReadsStatsDuringSyncImport
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Concurrent.SyncImportStatsMatchResult:Concurrent.ReadsStatsDuringSyncImport
build-review-asan-msvc/Debug/csvp_tests.exe --gtest_filter=Concurrent.SyncImportStatsMatchResult:Concurrent.ReadsStatsDuringSyncImport
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release、Debug、ASan Debug 定向测试均为 2/2 通过，ASan 零报告；Release 与 Debug 全量回归均为 40/40。10 万行测试最终统计为 100,000 行、98,000 行导入、2,000 行跳过，并发读取期间总行数保持单调。

限制：Windows/MSVC 当前构建链不提供 ThreadSanitizer，本项使用原子实现审查、并发压力测试和 ASan 内存检查验证，未声称执行过 TSan。

### Git 证据

Git 标签：`part-a-a16-atomic-stats`。
