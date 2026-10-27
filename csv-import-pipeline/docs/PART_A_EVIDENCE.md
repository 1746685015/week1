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
