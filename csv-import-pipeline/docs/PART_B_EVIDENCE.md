# Part B 实现与验收证据

## 原始材料与提交物保护

- 周作业原件：`D:\c++development\week01\周作业：CSV 数据导入：缺陷修复与特性实现作业.pdf`
- 文件大小：1,420,624 字节。
- SHA-256：`D1A2084ADAF2CB09C22A677D403D951DAEFD94CEAEE9CCB1495CB755D69AAA63`。
- 核对范围：PDF 共 6 页；Part B 位于第 4–5 页，提交要求位于第 6 页。
- 原件只读保留，实验过程不覆盖、不移动、不删除该文件。

老师要求最终提交：

1. 修复和实现后的完整工程，Git 历史能够体现定位、验证和修复过程。
2. 按 `docs/REPORT_TEMPLATE.md` 填写的报告。
3. 关键测试输出、ASan 报告、`csvp_bench` 前后对比，以及约 1 GB 文件的内存实测证据。
4. Part B 报告需说明流式导入的线程模型、关键代码、停止机制、超时行为和未完成任务数。

当前已归档材料：

- `docs/PART_A_EVIDENCE.md`：Part A 逐项修复与最终验收。
- `data/a6_baseline_report.txt`：A-6 优化前基准。
- `data/a6_optimized_report.txt`：A-6 优化后基准。
- `data/sample_1gb.txt`：本机约 1 GB 数据文件的路径说明；大文件本体不提交 GitHub。
- `third_party/googletest`：已内嵌测试框架和许可证，保证新克隆可构建。
- `docs/PART_B_EVIDENCE.md`：本文件，持续记录 Part B 的真实命令和结果。

## Part B 清单审查结论

清单对老师的四项 B-1 要求和六项 B-2 要求转述准确，B0–B8 的测试优先、逐项提交和证据留存方法合理。实施时作以下修正：

- 清单中的“GitHub 连接 `push=false`”已过时；本机 `origin` 具有写权限，每项完成后直接推送 `part-b` 分支并核对远端 SHA。
- 不直接让后台线程调用公开 `importSync()`；同步与流式路径应复用私有导入核心，才能统一逐行分类、停止检查、统计和错误语义。
- `pendingCount()` 应随任务生命周期实时表示未完成任务数，而不是只在 `shutdown()` 超时时临时写入。
- C++17 的 `std::thread` 没有超时 join。`shutdown(timeout)` 只有限等待完成条件；超时后保留受管 worker，不 `detach()`，任务完成后再安全 join。
- 不持有状态锁或生命周期锁执行用户 callback、文件 I/O 或 `join()`。
- 同一 `ImportPipeline` 实例的同步与流式导入互斥；并发启动应通过明确异常拒绝，不能让两条路径重置同一组统计。
- callback 取消属于正常流程并返回部分 `Result`；callback 抛异常则通过 `future` 传播，但所有退出路径都必须清理 active/pending 并通知等待者。
- 现有 `Concurrent.ImportAndStats` 使用 `skip_bad_rows=false`，不能证明 100,000 行完整进度；Part B 将改为确定性测试并核对 98,000/2,000 分布。

## B0：基线与分支

### 环境与起点

- 日期：2026-10-27。
- 分支：`part-b`，从 `main` 创建。
- 基线提交：`3ce2ac3fe960f1a8d358c3bd79faafc970b9db55`。
- 生成器：Visual Studio 17 2022，x64。
- MSBuild：17.14.60；MSVC 工具集：14.44.35207。
- C++ 标准：C++17；项目启用 MSVC `/W4`。

### 基线命令

```powershell
cmake --build build-review-baseline --config Debug --parallel
ctest --test-dir build-review-baseline -C Debug --output-on-failure
cmake --build build-review-baseline --config Release --parallel
ctest --test-dir build-review-baseline -C Release --output-on-failure
build-review-baseline/Release/csvp_tests.exe --gtest_list_tests
```

### 基线结果

- Debug：47/47 个启用测试通过，零失败。
- Release：47/47 个启用测试通过，零失败。
- 共发现 48 个注册测试；唯一禁用项为 `Concurrent.DISABLED_ShutdownNoDeadlock`。
- `Pipeline.StreamingImportNotImplemented` 与 `Pipeline.ShutdownNotImplemented` 只是骨架检查，当前通过不能证明 Part B 已实现。
- Part A 全量回归保持通过。

### Git 证据

提交信息：`chore: establish Part B baseline`。

标签：`part-b-b0-baseline`。

## B1：任务状态和线程生命周期基础

### 设计

- `m_lifecycleMutex` 只保护 worker 的创建、移动和 join，不用于文件 I/O、校验或 callback。
- `m_stateMutex` 保护 streaming active 状态与 pending 数；`pendingCount()` 返回锁保护的一致快照。
- `m_stateChanged` 为后续限时等待提供完成通知，不用忙等或固定 sleep 猜测状态。
- `m_worker` 由 `ImportPipeline` 独占；不使用 `detach()`。析构时先请求停止，再 join，避免线程访问已销毁对象。
- 显式禁用复制，防止互斥量、线程和统计状态被意外复制。
- 头文件由 `#pragma once` 改为可移植 include guard，并按 WPS C++ 规范统一本次涉及的声明。

### 验证命令

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Concurrent.PendingCountInitiallyZero:Concurrent.ShutdownWhenIdle
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Concurrent.PendingCountInitiallyZero:Concurrent.ShutdownWhenIdle
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
clang-tidy src/pipeline.cpp -checks="-*,clang-analyzer-*,bugprone-*" -- -std=c++17 -Iinclude -DWIN32 -D_WINDOWS
```

结果：Release/Debug 定向测试均为 2/2 通过；两种配置全量回归均为 49/49 通过、零失败；clang-tidy 退出码 0，显示告警 0、错误 0。空闲 shutdown 实测未等待完整的 25 ms 超时，pending 始终为 0。

### Git 证据

提交信息：`feat: add streaming task lifecycle state`。

标签：`part-b-b1-lifecycle-state`。

## B2：真实后台导入与 future 结果

### 实现

- 将同步路径的完整导入流程抽为私有 `runImport()`，同步与后台线程复用同一解析、列数检查、校验、错误收集和统计语义。
- `importStreaming()` 在调用线程只复制文件路径、预约任务状态并创建 worker；解析器和全部导入工作在 worker 内执行。
- 使用 `std::packaged_task<Result()>`/`std::future<Result>` 交付正常结果；异常通过 future 传播，不从线程入口逃逸。
- 文件路径在启动线程前复制为拥有所有权的 `std::string`，不捕获调用者可能失效的 `const char *`。
- 启动时 pending 为 1；正常、文件失败、异常和线程创建失败路径都调用统一清理，使 pending 最终归零并通知等待者。
- 启动新任务前 join 已完成的旧 worker；活动任务重复启动会被明确拒绝，不覆盖 joinable thread。

### 验证命令

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Pipeline.StreamingImportMatchesSyncImport:Pipeline.StreamingMissingFile
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Pipeline.StreamingImportMatchesSyncImport:Pipeline.StreamingMissingFile
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
clang-tidy src/pipeline.cpp -checks="-*,clang-analyzer-*,bugprone-*" -- -std=c++17 -Iinclude -DWIN32 -D_WINDOWS
```

结果：Release/Debug 定向测试均为 2/2 通过；两种配置全量回归均为 49/49 通过、零失败；clang-tidy 退出码 0，显示告警 0、错误 0。静态检查最初对 packaged task 的有意异常传播报告 `bugprone-exception-escape`；保留“异常进入 future”的正确语义，并在该 lambda 上用带英文理由的精确 `NOLINT`，没有全局关闭规则。

### Git 证据

提交信息：`feat: implement asynchronous CSV import`。

标签：`part-b-b2-async-import`。

## B3：每 1000 行报告进度

### 语义与实现

- `processed` 定义为已经完成分类的数据记录数，即 `rows_imported + rows_skipped`；不包含表头。
- 解析错误、列数错误和字段校验错误在按配置归入 skipped 后才计数，合法行在归入 imported 后计数。
- 仅当 `processed` 是 1000 的整数倍时调用 callback；不足 1000 的末尾批次不额外回调。
- 空 callback 被安全忽略；callback 在 worker 线程、所有内部锁之外执行。
- B3 只报告进度，返回值取消语义在 B4 单独实现和提交。

### 验证命令

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Concurrent.StreamingProgressEveryThousand
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Concurrent.StreamingProgressEveryThousand
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
clang-tidy src/pipeline.cpp -checks="-*,clang-analyzer-*,bugprone-*" -- -std=c++17 -Iinclude -DWIN32 -D_WINDOWS
```

结果：Release/Debug 定向测试均为 1/1 通过；两种配置全量回归均为 50/50 通过、零失败；clang-tidy 退出码 0，显示告警 0、错误 0。`sample_100k.csv` 的回调值严格为 1000、2000……100000，共 100 次；结果为 imported=98000、skipped=2000，回调线程与调用线程不同，结束后 pending=0。

### Git 证据

提交信息：`feat: report streaming progress every 1000 rows`。

标签：`part-b-b3-progress`。

## B4：回调取消并返回部分结果

### 语义与实现

- 进度 callback 返回 `false` 时，worker 在已完成当前 1000 行批次后停止读取；本次 callback 对应的行已计入结果和实时统计。
- 取消属于正常控制流：future 返回部分 `Result`，不抛出取消异常。
- worker 的统一退出路径仍会将 pending 清零并通知等待者；取消后没有后台任务遗留。

### 验证命令

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Concurrent.StreamingProgressCanCancel
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Concurrent.StreamingProgressCanCancel
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
clang-tidy src/pipeline.cpp -checks="-*,clang-analyzer-*,bugprone-*" -- -std=c++17 -Iinclude -DWIN32 -D_WINDOWS
```

结果：Release/Debug 定向测试均为 1/1 通过；两种配置全量回归均为 51/51 通过、零失败；clang-tidy 退出码 0，显示告警 0、错误 0。callback 仅收到 1000、2000 两次进度，在 2000 返回 `false` 后 future 正常返回共 2000 行的部分结果，pending=0。

### Git 证据

提交信息：`feat: support progress callback cancellation`。

标签：`part-b-b4-progress-cancel`。

## B5：协作停止、超时等待与 pending

### 语义与实现

- `shutdown(timeout)` 先发布原子停止信号，再通过条件变量至多等待指定时长；不强制终止线程。
- worker 在每次读取前、每个 1000 行 callback 返回后检查停止信号，因此正常解析和回调阻塞解除后都能及时收尾。
- `pendingCount()` 是锁保护的实时生命周期状态：活动 worker 为 1，完成为 0；超时返回不会伪造完成状态。
- shutdown 若由 worker callback 自己调用，只发布停止信号并立即返回，避免线程等待自身。
- worker 始终保存在成员 `std::thread` 中；没有 detach，析构或下一次启动会 join，不泄漏线程资源。

### 确定性验证设计

测试让 worker 在第 1000 行 callback 内等待测试门闩。主线程调用 `shutdown(50ms)`，验证其在 500 ms 安全上限内返回且 pending=1；随后打开门闩，验证 future 在 2 秒内就绪、部分结果为 1000 行、pending 最终为 0，并再次调用空闲 shutdown 验证幂等性。

### 验证命令

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Concurrent.ShutdownNoDeadlock
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Concurrent.ShutdownNoDeadlock
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
clang-tidy src/pipeline.cpp -checks="-*,clang-analyzer-*,bugprone-*" -- -std=c++17 -Iinclude -DWIN32 -D_WINDOWS
```

结果：Release/Debug 定向测试均为 1/1 通过；两种配置全量回归均为 52/52 通过、零失败，原先禁用的 shutdown 验收测试已经启用；clang-tidy 退出码 0，显示告警 0、错误 0。受控 callback 下 `shutdown(50ms)` 约 57 ms 完成整项测试，超时点 pending=1；门闩释放后 future 在期限内返回 1000 行部分结果，pending=0。

### Git 证据

提交信息：`feat: implement timed shutdown and pending count`。

标签：`part-b-b5-timed-shutdown`。

## B6：并发统计、互斥与异常清理

### 实现

- 新增统一的 `m_importActive` 状态，同一实例的同步与流式导入共享互斥契约；冲突调用抛出 `std::logic_error`，避免同时重置或写入同一组统计。
- 同步导入也使用异常安全的进入/退出路径，严格模式抛出后会清除活动状态；后续调用可以继续使用同一实例。
- callback 和严格模式异常由 `std::packaged_task` 放入 future；worker 内部先完成状态清理，再把异常交付调用方。
- `stats()` 继续只读原子计数器，callback 不在生命周期锁或状态锁内执行。
- `shutdown()` 只通知流式 worker；空闲或仅同步导入时不会遗留停止标志影响后续同步任务。

### 验证命令

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Concurrent.StatsDuringStreamingImport:Concurrent.StreamingCallbackThrows:Concurrent.StrictModeBadRowMatchesSyncImport:Concurrent.RejectsSyncImportWhileStreaming
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Concurrent.StatsDuringStreamingImport:Concurrent.StreamingCallbackThrows:Concurrent.StrictModeBadRowMatchesSyncImport:Concurrent.RejectsSyncImportWhileStreaming
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
clang-tidy src/pipeline.cpp -checks="-*,clang-analyzer-*,bugprone-*" -- -std=c++17 -Iinclude -DWIN32 -D_WINDOWS
```

结果：Release/Debug 定向测试均为 4/4 通过；两种配置全量回归均为 56/56 通过、零失败，MSVC 构建无新增警告；clang-tidy 退出码 0，显示告警 0、错误 0。流式统计单调且最终与 Result 一致；callback 异常和严格模式坏行均可由 future 观察，pending 最终为 0；活动流式任务期间同步调用被明确拒绝。

### Git 证据

提交信息：`fix: make streaming cleanup exception-safe`。

标签：`part-b-b6-exception-safety`。

## B7：worker 生命周期与重复调用

### 覆盖范围

- 正常完成后再次启动：旧的 joinable worker 先被 join，再创建新 worker。
- 活动任务期间第二次流式启动被拒绝；活动流式任务期间同步启动在 B6 已覆盖。
- callback 可读取 pending 并调用 shutdown，证明 callback 不在内部锁内执行；worker 自调用 shutdown 不发生 self-join。
- callback 异常完成清理后，同一实例可再次成功启动。
- 析构活动对象时发布停止信号并等待 worker；受控 callback 门闩证明析构不会让线程悬空访问对象。
- B5 超时后再次 shutdown 仍按时返回，门闩释放后任务正常清理。
- 线程创建失败路径无法在不增加生产代码故障注入接口的情况下稳定触发；代码审查确认赋值 `std::thread` 抛出时调用统一清理，future/pending 不悬挂。

### 验证命令

```powershell
build-review-baseline/Release/csvp_tests.exe --gtest_filter=Concurrent.CompletedWorkerCanRestart:Concurrent.RejectsSecondStreamingImport:Concurrent.CallbackCanRequestShutdown:Concurrent.DestructorJoinsActiveWorker:Concurrent.RestartsAfterCallbackFailure:Concurrent.ShutdownNoDeadlock
build-review-baseline/Debug/csvp_tests.exe --gtest_filter=Concurrent.CompletedWorkerCanRestart:Concurrent.RejectsSecondStreamingImport:Concurrent.CallbackCanRequestShutdown:Concurrent.DestructorJoinsActiveWorker:Concurrent.RestartsAfterCallbackFailure:Concurrent.ShutdownNoDeadlock
ctest --test-dir build-review-baseline -C Release --output-on-failure
ctest --test-dir build-review-baseline -C Debug --output-on-failure
```

结果：Release/Debug 生命周期定向测试均为 6/6 通过；两种配置全量回归均为 61/61 通过、零失败，MSVC 构建无新增警告。已验证正常重启、异常后重启、活动任务重复启动拒绝、callback 内 pending/shutdown 无死锁、超时后再次 shutdown、活动对象析构等待并安全 join。

### Git 证据

提交信息：`test: cover streaming worker lifecycle`。

标签：`part-b-b7-worker-lifecycle`。
