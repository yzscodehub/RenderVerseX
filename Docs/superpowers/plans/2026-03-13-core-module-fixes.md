# Core 模块修复实施计划

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 Core 模块的关键问题：RVX_NEW 内存泄漏、日志系统改进、作业系统优先级调度

**Architecture:** 分三个独立任务实施，每个任务可独立完成测试。优先修复严重 BUG，再改进功能。

**Tech Stack:** C++20, spdlog, std::priority_queue

---

## 任务总览

| 任务 | 严重程度 | 工作量 | 依赖 |
|------|----------|--------|------|
| 1. 修复 RVX_NEW 宏 | 🔴 严重 BUG | 1-2h | 无 |
| 2. 日志系统改进 | 🟡 改进 | 3-4h | 无 |
| 3. 作业优先级调度 | 🟡 改进 | 3-4h | 无 |

---

## Chunk 1: 修复 RVX_NEW 宏内存泄漏

### 任务 1.1: 修改 MemoryTracker 接口

**Files:**
- Modify: `Core/Include/Core/Memory/Allocators.h:290-329`
- Modify: `Core/Private/Memory/Allocators.cpp` (新建或追加)

- [ ] **Step 1: 读取现有 MemoryTracker 实现**

读取 `Core/Include/Core/Memory/Allocators.h` 第 290-344 行，理解当前 TrackAllocation 接口

- [ ] **Step 2: 修改 TrackAllocation 返回类型**

在 `Allocators.h` 中将 `TrackAllocation(void* ptr, size_t size, ...)` 改为返回 `void*`:

```cpp
// 修改后的接口
void* TrackAllocation(size_t size, const char* file, int line);
void TrackDeallocation(void* ptr);
```

- [ ] **Step 3: 更新 Allocators.cpp 实现**

在 `Core/Private/Memory/Allocators.cpp` 中实现:

```cpp
void* MemoryTracker::TrackAllocation(size_t size, const char* file, int line)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    void* ptr = malloc(size);
    if (!ptr) return nullptr;

    AllocationInfo info{size, file, line};
    m_allocations[ptr] = info;
    m_totalAllocated += size;
    m_allocationCount++;

    if (m_totalAllocated > m_peakAllocated)
        m_peakAllocated = m_totalAllocated;

    return ptr;
}

void MemoryTracker::TrackDeallocation(void* ptr)
{
    if (!ptr) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_allocations.find(ptr);
    if (it != m_allocations.end())
    {
        m_totalAllocated -= it->second.size;
        m_allocations.erase(it);
    }

    free(ptr);
}
```

- [ ] **Step 4: 修改 RVX_NEW 宏**

修改 `Allocators.h` 第 335-344 行:

```cpp
#ifdef RVX_TRACK_MEMORY
    #define RVX_NEW(Type, ...) \
        new (::RVX::MemoryTracker::Get().TrackAllocation( \
            sizeof(Type), __FILE__, __LINE__)) Type(__VA_ARGS__)
    #define RVX_DELETE(ptr) \
        (::RVX::MemoryTracker::Get().TrackDeallocation(ptr), delete ptr)
#else
    #define RVX_NEW(Type, ...) new Type(__VA_ARGS__)
    #define RVX_DELETE(ptr) delete ptr
#endif
```

- [ ] **Step 5: 编译验证**

```bash
# 尝试编译 Core 模块
cd E:/WorkSpace/RenderVerseX
cmake --build build --target RVX_Core --config Release 2>&1 | head -50
```

预期: 编译成功，无错误

- [ ] **Step 6: 提交**

```bash
git add Core/Include/Core/Memory/Allocators.h Core/Private/Memory/Allocators.cpp
git commit -m "fix: resolve RVX_NEW macro double allocation memory leak

- Change TrackAllocation to return void* for placement new
- Update RVX_NEW macro to use single allocation with placement new
- Fix critical memory leak bug

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Chunk 2: 日志系统改进

### 任务 2.1: 添加日志轮转功能

**Files:**
- Modify: `Core/Include/Core/Log.h`
- Modify: `Core/Private/Log.cpp`

- [ ] **Step 1: 读取现有 Log 实现**

读取 `Core/Include/Core/Log.h` 和 `Core/Private/Log.cpp`

- [ ] **Step 2: 修改 Log.h 添加新接口**

在 `Log.h` 第 14-33 行 `Log` 类中添加:

```cpp
class Log
{
public:
    // 新增: 带轮转的初始化
    struct Config
    {
        bool enableRotation = true;
        size_t maxFileSizeMB = 10;
        size_t maxFiles = 5;
        std::string logFileName = "RenderVerseX.log";
    };

    static void Initialize(const Config& config = Config{});
    static void Shutdown();

    // 新增: 运行时级别控制
    static void SetLevel(spdlog::level::level_enum level);
    static void SetModuleLevel(const std::string& module, spdlog::level::level_enum level);
    static spdlog::level::level_enum GetModuleLevel(const std::string& module);

    // ... existing methods remain unchanged
};
```

- [ ] **Step 3: 修改 Log.cpp 实现轮转**

修改 `Log.cpp`:

```cpp
#include <spdlog/sinks/rotating_file_sink.h>

void Log::Initialize(const Config& config)
{
    std::vector<spdlog::sink_ptr> sinks;

    // 控制台输出
    sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    // 文件输出 - 根据配置选择
    if (config.enableRotation)
    {
        sinks.emplace_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config.logFileName,
            config.maxFileSizeMB * 1024 * 1024,  // MB to bytes
            config.maxFiles));
    }
    else
    {
        sinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            config.logFileName, true));
    }

    // 设置 pattern
    sinks[0]->set_pattern("%^[%T] [%n] [%l]%$ %v");
    sinks[1]->set_pattern("[%T] [%n] [%l] %v");

    // 创建模块日志器
    s_coreLogger    = CreateLogger("CORE",    sinks);
    s_rhiLogger     = CreateLogger("RHI",     sinks);
    s_sceneLogger   = CreateLogger("SCENE",   sinks);
    s_assetLogger   = CreateLogger("ASSET",   sinks);
    s_spatialLogger = CreateLogger("SPATIAL", sinks);
    s_renderLogger  = CreateLogger("RENDER",  sinks);

    RVX_CORE_INFO("RenderVerseX Logger Initialized (rotation: {})",
                  config.enableRotation ? "enabled" : "disabled");
}
```

- [ ] **Step 4: 实现运行时级别控制**

在 `Log.cpp` 中添加:

```cpp
void Log::SetLevel(spdlog::level::level_enum level)
{
    std::array loggers = { s_coreLogger, s_rhiLogger, s_sceneLogger,
                           s_assetLogger, s_spatialLogger, s_renderLogger };
    for (auto& logger : loggers)
    {
        if (logger) logger->set_level(level);
    }
}

void Log::SetModuleLevel(const std::string& module, spdlog::level::level_enum level)
{
    std::shared_ptr<spdlog::logger> logger = nullptr;

    if (module == "CORE")    logger = s_coreLogger;
    else if (module == "RHI")     logger = s_rhiLogger;
    else if (module == "SCENE")   logger = s_sceneLogger;
    else if (module == "ASSET")   logger = s_assetLogger;
    else if (module == "SPATIAL") logger = s_spatialLogger;
    else if (module == "RENDER")  logger = s_renderLogger;

    if (logger) logger->set_level(level);
}

spdlog::level::level_enum Log::GetModuleLevel(const std::string& module)
{
    std::shared_ptr<spdlog::logger> logger = nullptr;

    if (module == "CORE")    logger = s_coreLogger;
    else if (module == "RHI")     logger = s_rhiLogger;
    else if (module == "SCENE")   logger = s_sceneLogger;
    else if (module == "ASSET")   logger = s_assetLogger;
    else if (module == "SPATIAL") logger = s_spatialLogger;
    else if (module == "RENDER")  logger = s_renderLogger;

    return logger ? logger->level() : spdlog::level::info;
}
```

- [ ] **Step 5: 编译验证**

```bash
cmake --build build --target RVX_Core --config Release 2>&1 | head -50
```

预期: 编译成功

- [ ] **Step 6: 提交**

```bash
git add Core/Include/Core/Log.h Core/Private/Log.cpp
git commit -m "feat: add log rotation and runtime level control

- Add rotating_file_sink for log rotation (max size, max files)
- Add SetLevel/SetModuleLevel/GetModuleLevel APIs
- Configurable via Log::Config struct

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Chunk 3: 作业优先级调度实现

### 任务 3.1: 修改 ThreadPool 支持优先级

**Files:**
- Modify: `Core/Include/Core/Job/ThreadPool.h`
- Modify: `Core/Private/Job/ThreadPool.cpp`

- [ ] **Step 1: 读取现有 ThreadPool 实现**

读取 `Core/Include/Core/Job/ThreadPool.h` 和 `Core/Private/Job/ThreadPool.cpp`

- [ ] **Step 2: 修改 ThreadPool.h 添加优先级支持**

在 `ThreadPool.h` 中:

```cpp
// 添加头文件
#include <queue>
#include <mutex>

// 在文件顶部添加 JobPriority 声明（如果尚未包含）
namespace RVX { enum class JobPriority; }

// 修改 Task 结构体
struct Task
{
    std::function<void()> func;
    int32_t priority = 0;  // 数值越大优先级越高

    // 比较运算符用于优先级队列
    bool operator<(const Task& other) const
    {
        return priority < other.priority;  // priority_queue 是最大堆，需要反序
    }
};

// 修改 ThreadPool 类 - 添加优先级 Submit 方法
class ThreadPool
{
public:
    // 新增: 带优先级的提交
    template<typename F, typename... Args>
    auto Submit(F&& func, int32_t priority = 0)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using ReturnType = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(func), std::forward<Args>(args)...)
        );

        std::future<ReturnType> future = task->get_future();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping)
            {
                throw std::runtime_error("Submit called on stopped ThreadPool");
            }
            m_tasks.emplace(Task{[task]() { (*task)(); }, priority});
        }

        m_condition.notify_one();
        return future;
    }

    // 保留原有 Submit 兼容性
    template<typename F, typename... Args>
    auto Submit(F&& func, JobPriority priority) -> std::future<...>
    {
        return Submit(std::forward<F>(func), static_cast<int32_t>(priority));
    }

private:
    std::priority_queue<Task> m_tasks;  // 改为优先级队列
};
```

- [ ] **Step 3: 修改 ThreadPool.cpp**

修改 `ThreadPool.cpp`:

```cpp
// 修改构造函数中队列初始化 - priority_queue 不需要显式初始化

// 修改 GetPendingCount
size_t ThreadPool::GetPendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tasks.size() + m_activeTasks;
}
```

- [ ] **Step 4: 编译验证**

```bash
cmake --build build --target RVX_Core --config Release 2>&1 | head -50
```

预期: 编译成功

- [ ] **Step 5: 提交**

```bash
git add Core/Include/Core/Job/ThreadPool.h Core/Private/Job/ThreadPool.cpp
git commit -m "feat: add priority support to ThreadPool

- Convert m_tasks from std::queue to std::priority_queue
- Add Submit with priority parameter (int32_t)
- Add Submit overload for JobPriority enum

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

### 任务 3.2: 修改 JobSystem 使用优先级

**Files:**
- Modify: `Core/Include/Core/Job/JobSystem.h`
- Modify: `Core/Private/Job/JobSystem.cpp`

- [ ] **Step 1: 读取现有 JobSystem 实现**

读取 `Core/Include/Core/Job/JobSystem.h`

- [ ] **Step 2: 修改 JobSystem.h 添加优先级支持**

在 `JobSystem.h` 第 110-131 行修改 Submit 方法:

```cpp
// 修改 Submit 方法签名，添加优先级参数
template<typename F>
JobHandle Submit(F&& func, JobPriority priority = JobPriority::Normal)
{
    if (!m_threadPool)
    {
        // Execute synchronously if not initialized
        func();
        return JobHandle{};
    }

    auto completed = std::make_shared<std::atomic<bool>>(false);

    auto future = m_threadPool->Submit([func = std::forward<F>(func), completed]() {
        func();
        completed->store(true);
    }, priority);

    JobHandle handle;
    handle.m_future = future.share();
    handle.m_completed = completed;
    return handle;
}
```

- [ ] **Step 3: 修改 JobSystem.cpp**

在 `JobSystem.cpp` 中确保包含 JobPriority 定义（如果需要）

- [ ] **Step 4: 编译验证**

```bash
cmake --build build --target RVX_Core --config Release 2>&1 | head -50
```

预期: 编译成功

- [ ] **Step 5: 提交**

```bash
git add Core/Include/Core/Job/JobSystem.h Core/Private/Job/JobSystem.cpp
git commit -m "feat: add priority support to JobSystem

- Add JobPriority parameter to Submit method
- Pass priority through to ThreadPool

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## 验证步骤

所有任务完成后，运行以下验证:

```bash
# 1. 完整编译
cmake --build build --config Release 2>&1 | tail -20

# 2. 运行现有测试（如果有）
./build/Tests/Release/SystemIntegrationTest.exe 2>&1 | head -30
```

---

## 预期产出

1. **RVX_NEW 修复**: 内存泄漏问题解决
2. **日志改进**: 日志轮转 + 运行时级别控制
3. **作业优先级**: JobPriority 真正生效

---

## 关键文件路径

| 文件 | 绝对路径 |
|------|----------|
| Allocators.h | `E:\WorkSpace\RenderVerseX\Core\Include\Core\Memory\Allocators.h` |
| Log.h | `E:\WorkSpace\RenderVerseX\Core\Include\Core\Log.h` |
| Log.cpp | `E:\WorkSpace\RenderVerseX\Core\Private\Log.cpp` |
| ThreadPool.h | `E:\WorkSpace\RenderVerseX\Core\Include\Core\Job\ThreadPool.h` |
| ThreadPool.cpp | `E:\WorkSpace\RenderVerseX\Core\Private\Job\ThreadPool.cpp` |
| JobSystem.h | `E:\WorkSpace\RenderVerseX\Core\Include\Core\Job\JobSystem.h` |
| JobSystem.cpp | `E:\WorkSpace\RenderVerseX\Core\Private\Job\JobSystem.cpp` |

---

*Plan created: 2026-03-13*
