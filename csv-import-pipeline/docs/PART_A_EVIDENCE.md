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
