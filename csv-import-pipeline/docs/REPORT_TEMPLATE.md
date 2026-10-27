# 缺陷修复与特性实现报告

> 候选人：_______________  日期：_______________  用时：_______________

---

## Part A：缺陷修复

请对每个发现的缺陷，按以下四段式填写：

### 缺陷 A-1

- **复现步骤**：
- **根因分析**：
- **修复方法**：
- **验证证据**（ASan 报告、测试输出、gtest_repeat 结果等）：

---

### 缺陷 A-2

- **复现步骤**：
- **根因分析**：
- **修复方法**：
- **验证证据**：

---

### 缺陷 A-3

- **复现步骤**：
- **根因分析**：
- **修复方法**：
- **验证证据**：

---

### 缺陷 A-4

- **复现步骤**：
- **根因分析**：
- **修复方法**：
- **验证证据**：

---

### 缺陷 A-5

- **复现步骤**：
- **根因分析**：
- **修复方法**：
- **验证证据**：

---

### 缺陷 A-6

- **复现步骤**：
- **根因分析**：
- **修复方法**：
- **验证证据**（前后吞吐量对比）：

---

### 缺陷 A-7

- **复现步骤**：
- **根因分析**：
- **修复方法**：
- **验证证据**：

---

### 缺陷 A-8

- **复现步骤**：
- **根因分析**：
- **修复方法**：
- **验证证据**：

---

## Part B：特性实现

### B-1: 流式导入（importStreaming + progress 回调）

- **设计思路**：同步与流式入口复用私有 `runImport()`，保证解析、列数检查、校验、坏行和统计语义一致。流式入口复制文件路径和 callback 后立即返回 future；每完成 1000 条数据记录（imported + skipped，不含表头）在 worker 线程调用一次 callback。callback 返回 `false` 属于正常取消，future 返回已完成部分的 Result。
- **线程模型**（如何管理后台线程与主线程的交互）：每个 `ImportPipeline` 最多有一个活动导入。`m_lifecycleMutex` 只串行化启动、join 和 shutdown 生命周期操作，`m_stateMutex` 只保护 active/pending 复合状态，统计使用原子变量。用户 callback 在所有内部锁之外执行。worker 不 detach；完成的 worker 在下一次启动时 join，活动 worker 在析构时收到停止信号并被 join。
- **关键代码变更摘要**：新增成员 worker、条件变量、生命周期/状态锁、`pendingCount()`、统一任务清理函数和非复制约束；使用 `std::packaged_task<Result()>` 将正常结果及异常交付 future；同步和流式入口通过 `m_importActive` 拒绝同实例并发写统计；文件失败、严格模式异常、callback 异常和线程创建异常路径都会恢复状态。

### B-2: 优雅停止（shutdown + timeout）

- **设计思路**：采用 C++17 可安全实现的协作停止，不强杀线程。`shutdown(timeout)` 发布原子停止信号，通过条件变量等待 worker 完成，等待达到 timeout 后无条件返回。
- **取消机制**（如何通知工作线程停止、超时如何处理）：worker 在每次读取前以及每次进度 callback 返回后检查停止标志。callback 阻塞时，shutdown 仅等待到 timeout；callback 自己调用 shutdown 时检测 worker 线程 ID 并立即返回，避免 self-join。析构函数最终负责停止并 join，保证对象销毁后没有 worker 访问成员。
- **未完成任务数汇报**：`pendingCount()` 在状态锁下读取实时值。活动流式任务为 1，所有正常、取消和异常退出路径最终设为 0。受控门闩测试确认 `shutdown(50ms)` 超时返回时 pending=1，释放 callback 后 future 返回 1000 行部分结果且 pending=0。

### Part B 验证摘要

- Release/Debug 全量回归：均为 61/61 通过，MSVC 编译告警为 0。
- 进度：`sample_100k.csv` 回调严格为 1000、2000……100000，共 100 次。
- callback 取消：在 2000 返回 `false`，future 返回恰好 2000 条已分类记录。
- 异常与生命周期：callback/严格模式异常、重复启动、异常后重启、callback 内 shutdown、活动对象析构均有自动化测试。
- 压力与工具：Release 随机顺序重复 100 轮全部通过；MSVC ASan Debug 61/61 通过且未报告 AddressSanitizer 错误；clang-tidy 退出码 0、可见告警 0、错误 0。
- 逐提交命令、结果、Git SHA 和限制见 `docs/PART_B_EVIDENCE.md`。

---

## Part C：加分项

### C-1: 编译告警清理

- `/W4`（MSVC）或 `-Wall -Wextra` 告警列表及修复说明：

### C-2: 性能优化

- 发现的性能瓶颈：
- 优化方法：
- **前后吞吐量对比数据**（csvp_bench 输出）：

### C-3: 额外发现

- 发现的未在题面中提及的问题及分析：

---

## 附件

请将以下文件随报告一并提交：

1. `git log --oneline --all` 输出（展示提交历史）
2. 关键测试的通过截图或日志
3. ASan / clang-tidy 输出（如使用）
