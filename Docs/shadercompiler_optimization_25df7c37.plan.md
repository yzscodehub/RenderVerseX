---
name: ShaderCompiler Optimization
overview: Comprehensive optimization of the ShaderCompiler module focusing on async compilation, shader permutation system, improved caching, and hot reload support to meet game engine production standards.
todos:
  - id: phase1-compile-service
    content: "Phase 1: 实现 ShaderCompileService 异步编译服务，集成 Job System"
    status: completed
  - id: phase2-cache-format
    content: "Phase 2: 设计并实现新的缓存格式，包含版本控制和反射数据序列化"
    status: completed
  - id: phase2-cache-manager
    content: "Phase 2: 实现 ShaderCacheManager，支持 Include 依赖追踪"
    status: completed
  - id: phase3-permutation
    content: "Phase 3: 实现 ShaderPermutationSystem 变体管理系统"
    status: completed
  - id: phase4-hot-reload
    content: "Phase 4: 实现 ShaderHotReloader 热重载系统"
    status: completed
  - id: phase5-refactor
    content: "Phase 5: 重构 ShaderManager，整合所有新服务"
    status: completed
  - id: dx11-slot-mapper
    content: "补充: 完善 DX11SlotMapper 实现"
    status: completed
  - id: dx12-upgrade
    content: "补充: DX12 升级到 DXC 支持 SM6.x"
    status: completed
isProject: false
---

# ShaderCompiler 完整优化设计方案

## 一、现状分析

### 1.1 当前架构

```
ShaderCompiler/
├── Include/ShaderCompiler/
│   ├── ShaderCompiler.h      # IShaderCompiler 接口, ShaderCompileOptions/Result
│   ├── ShaderManager.h       # 高层加载管理
│   ├── ShaderReflection.h    # 反射数据结构
│   └── ShaderLayout.h        # Pipeline Layout 生成
├── Private/
│   ├── ShaderCompiler.cpp    # 工厂函数
│   ├── ShaderManager.cpp     # 加载、缓存逻辑
│   ├── DXCCompiler.cpp       # Windows: FXC/DXC 编译
│   ├── DXCCompiler_Apple.cpp # Apple: glslang 编译
│   ├── SPIRVCrossTranslator.cpp # SPIR-V 转 MSL/GLSL
│   ├── ShaderReflection.cpp  # 反射提取
│   ├── ShaderLayout.cpp      # Layout 构建
│   ├── ShaderCache.cpp       # 空占位符
│   ├── ShaderPermutationCache.cpp # 空占位符
│   └── DX11SlotMapper.cpp    # 空占位符
```

### 1.2 当前编译流程

```
HLSL 源码
    │
    ├─[DX11/DX12]──► FXC (D3DCompile) ──► DXBC 字节码
    │
    ├─[Vulkan]─────► DXC (-spirv) ──────► SPIR-V 字节码
    │
    ├─[Metal]──────► glslang ──► SPIR-V ──► SPIRV-Cross ──► MSL 源码
    │
    └─[OpenGL]─────► DXC ──► SPIR-V ──► SPIRV-Cross ──► GLSL 源码
```

### 1.3 存在问题


| 问题                 | 影响                    | 严重程度 |
| ------------------ | --------------------- | ---- |
| 同步阻塞编译             | 加载时主线程卡顿              | 高    |
| DX12 使用 FXC        | 无法使用 SM6.x 特性         | 中    |
| 缓存只存字节码            | Metal/GL 无磁盘缓存，每次重新反射 | 中    |
| Include 未追踪        | 修改头文件缓存不失效            | 高    |
| 无 Permutation 系统   | 材质变体管理困难              | 高    |
| 无 Hot Reload       | 开发迭代效率低               | 中    |
| DX11SlotMapper 未实现 | DX11 资源绑定可能有问题        | 低    |


---

## 二、优化目标

### 2.1 核心目标

1. **异步编译**：着色器编译不阻塞主线程
2. **智能缓存**：完整缓存格式，支持 Include 依赖
3. **变体系统**：Shader Permutation 管理
4. **热重载**：开发时实时更新着色器
5. **DX12 升级**：支持 SM6.x 特性

### 2.2 性能指标


| 指标       | 当前    | 目标     |
| -------- | ----- | ------ |
| 首次加载卡顿   | 明显卡顿  | 异步无卡顿  |
| 缓存命中加载时间 | ~10ms | <1ms   |
| 变体编译     | 无支持   | 后台预热   |
| 热重载延迟    | 无支持   | <500ms |


---

## 三、整体架构设计

### 3.1 新架构

```
┌─────────────────────────────────────────────────────────────────┐
│                     ShaderManager (Facade)                       │
│  - LoadFromFile() / LoadFromFileAsync()                          │
│  - GetShaderVariant()                                            │
│  - EnableHotReload()                                             │
│  - Update()                                                      │
└─────────────────────────────────────────────────────────────────┘
         │              │               │              │
         ▼              ▼               ▼              ▼
┌─────────────┐ ┌─────────────┐ ┌─────────────┐ ┌─────────────┐
│ Compile     │ │ Cache       │ │ Permutation │ │ HotReload   │
│ Service     │ │ Manager     │ │ System      │ │ System      │
│             │ │             │ │             │ │             │
│ - Sync      │ │ - Memory    │ │ - Register  │ │ - Watch     │
│ - Async     │ │ - Disk      │ │ - GetVar    │ │ - Update    │
│ - Batch     │ │ - Validate  │ │ - Prewarm   │ │ - Callback  │
└──────┬──────┘ └──────┬──────┘ └──────┬──────┘ └──────┬──────┘
       │               │               │               │
       ▼               ▼               ▼               ▼
┌─────────────────────────────────────────────────────────────────┐
│                    IShaderCompiler (Backend)                     │
│  - DXCShaderCompiler (Windows: FXC + DXC)                       │
│  - AppleShaderCompiler (macOS/iOS: glslang)                     │
│  - SPIRVCrossTranslator (SPIR-V → MSL/GLSL)                     │
└─────────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────────┐
│                    Infrastructure                                │
│  - Job System (Core/Job/JobGraph.h)                             │
│  - File Watcher (Platform-specific)                             │
│  - Serialization (Core/Serialization/)                          │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 新增文件清单

```
ShaderCompiler/
├── Include/ShaderCompiler/
│   ├── ShaderCompiler.h          # 现有，小改
│   ├── ShaderManager.h           # 重构
│   ├── ShaderReflection.h        # 现有，不变
│   ├── ShaderLayout.h            # 现有，不变
│   ├── ShaderCompileService.h    # 新增：异步编译服务
│   ├── ShaderCacheManager.h      # 新增：缓存管理
│   ├── ShaderCacheFormat.h       # 新增：缓存文件格式
│   ├── ShaderSourceInfo.h        # 新增：源文件依赖信息
│   ├── ShaderPermutation.h       # 新增：变体系统
│   ├── ShaderHotReloader.h       # 新增：热重载
│   ├── ShaderCompileStats.h      # 新增：统计信息
│   └── ShaderTypes.h             # 新增：公共类型定义
├── Private/
│   ├── ShaderCompiler.cpp        # 现有，不变
│   ├── ShaderManager.cpp         # 重构
│   ├── DXCCompiler.cpp           # 修改：添加 SM6 支持、Include 追踪
│   ├── DXCCompiler_Apple.cpp     # 现有，小改
│   ├── SPIRVCrossTranslator.cpp  # 现有，不变
│   ├── SPIRVCrossTranslator.h    # 现有，不变
│   ├── ShaderReflection.cpp      # 现有，不变
│   ├── ShaderLayout.cpp          # 现有，不变
│   ├── ShaderCompileService.cpp  # 新增
│   ├── ShaderCacheManager.cpp    # 新增
│   ├── ShaderSourceInfo.cpp      # 新增
│   ├── ShaderPermutation.cpp     # 新增
│   ├── ShaderHotReloader.cpp     # 新增
│   ├── ShaderFileWatcher.cpp     # 新增：平台文件监视
│   ├── ShaderFileWatcher.h       # 新增
│   ├── TrackingIncludeHandler.cpp # 新增：DXC Include 追踪
│   ├── TrackingIncludeHandler.h  # 新增
│   ├── ShaderCache.cpp           # 重写（原空占位符）
│   ├── ShaderPermutationCache.cpp # 重写（原空占位符）
│   └── DX11SlotMapper.cpp        # 实现（原空占位符）
└── CMakeLists.txt                # 更新
```

---

## 四、Phase 1：异步编译服务

### 4.1 ShaderTypes.h - 公共类型

```cpp
#pragma once

#include "Core/Types.h"
#include <functional>
#include <future>

namespace RVX
{
    // 编译句柄
    using CompileHandle = uint64;
    constexpr CompileHandle RVX_INVALID_COMPILE_HANDLE = 0;

    // 回调类型
    using CompileCallback = std::function<void(const struct ShaderCompileResult&)>;
    using LoadCallback = std::function<void(const struct ShaderLoadResult&)>;
    using ReloadCallback = std::function<void(class RHIShaderRef)>;

    // 编译优先级
    enum class CompilePriority : uint8
    {
        Low = 0,      // 后台预热
        Normal = 1,   // 常规编译
        High = 2,     // 高优先级
        Immediate = 3 // 立即执行（同步）
    };

    // 编译状态
    enum class CompileStatus : uint8
    {
        Pending,      // 等待中
        Compiling,    // 编译中
        Completed,    // 已完成
        Failed,       // 失败
        Cancelled     // 已取消
    };

} // namespace RVX
```

### 4.2 ShaderCompileService.h

```cpp
#pragma once

#include "ShaderCompiler/ShaderCompiler.h"
#include "ShaderCompiler/ShaderTypes.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>

namespace RVX
{
    class JobSystem;

    // =========================================================================
    // 编译请求
    // =========================================================================
    struct CompileRequest
    {
        ShaderCompileOptions options;
        CompileCallback callback;
        CompilePriority priority = CompilePriority::Normal;
        CompileHandle handle = RVX_INVALID_COMPILE_HANDLE;
    };

    // =========================================================================
    // 编译任务状态
    // =========================================================================
    struct CompileTask
    {
        CompileHandle handle;
        CompileStatus status = CompileStatus::Pending;
        ShaderCompileResult result;
        CompileCallback callback;
        std::chrono::steady_clock::time_point submitTime;
        std::chrono::steady_clock::time_point completeTime;
    };

    // =========================================================================
    // 异步编译服务
    // =========================================================================
    class ShaderCompileService
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================
        struct Config
        {
            uint32 maxConcurrentCompiles = 4;
            bool enableStatistics = true;
        };

        explicit ShaderCompileService(const Config& config = {});
        ~ShaderCompileService();

        // 禁止拷贝
        ShaderCompileService(const ShaderCompileService&) = delete;
        ShaderCompileService& operator=(const ShaderCompileService&) = delete;

        // =====================================================================
        // 编译接口
        // =====================================================================

        /** @brief 同步编译（阻塞当前线程） */
        ShaderCompileResult CompileSync(const ShaderCompileOptions& options);

        /** @brief 异步编译（立即返回，后台执行） */
        CompileHandle CompileAsync(
            const ShaderCompileOptions& options,
            CompileCallback onComplete = nullptr,
            CompilePriority priority = CompilePriority::Normal);

        /** @brief 批量异步编译 */
        std::vector<CompileHandle> CompileBatch(
            const std::vector<ShaderCompileOptions>& batch,
            CompilePriority priority = CompilePriority::Normal);

        // =====================================================================
        // 任务管理
        // =====================================================================

        /** @brief 等待编译完成并获取结果 */
        ShaderCompileResult Wait(CompileHandle handle);

        /** @brief 等待多个编译完成 */
        std::vector<ShaderCompileResult> WaitAll(const std::vector<CompileHandle>& handles);

        /** @brief 检查编译是否完成 */
        bool IsComplete(CompileHandle handle) const;

        /** @brief 获取编译状态 */
        CompileStatus GetStatus(CompileHandle handle) const;

        /** @brief 取消编译任务（仅限等待中的任务） */
        bool Cancel(CompileHandle handle);

        /** @brief 取消所有等待中的任务 */
        void CancelAll();

        /** @brief 等待所有任务完成 */
        void Flush();

        // =====================================================================
        // 配置
        // =====================================================================

        /** @brief 设置最大并发编译数 */
        void SetMaxConcurrentCompiles(uint32 count);

        /** @brief 获取当前等待中的任务数 */
        uint32 GetPendingCount() const;

        /** @brief 获取当前正在编译的任务数 */
        uint32 GetActiveCount() const;

        // =====================================================================
        // 统计
        // =====================================================================
        struct Statistics
        {
            uint64 totalCompiles = 0;
            uint64 totalCompileTimeMs = 0;
            uint64 averageCompileTimeMs = 0;
            uint32 successCount = 0;
            uint32 failureCount = 0;
            uint32 cancelledCount = 0;

            void Reset();
        };

        const Statistics& GetStatistics() const { return m_stats; }
        void ResetStatistics() { m_stats.Reset(); }

    private:
        // =====================================================================
        // 内部实现
        // =====================================================================
        CompileHandle GenerateHandle();
        void WorkerThread();
        void ProcessTask(CompileTask& task);

        Config m_config;
        std::unique_ptr<IShaderCompiler> m_compiler;

        // 任务队列
        mutable std::mutex m_queueMutex;
        std::condition_variable m_queueCV;
        std::vector<CompileRequest> m_pendingQueue;

        // 活动任务
        mutable std::mutex m_tasksMutex;
        std::unordered_map<CompileHandle, CompileTask> m_tasks;

        // 工作线程
        std::vector<std::thread> m_workers;
        std::atomic<bool> m_shutdown{false};
        std::atomic<uint32> m_activeCount{0};

        // 句柄生成
        std::atomic<uint64> m_nextHandle{1};

        // 统计
        Statistics m_stats;
    };

} // namespace RVX
```

### 4.3 ShaderCompileService.cpp 实现要点

```cpp
#include "ShaderCompiler/ShaderCompileService.h"
#include "Core/Log.h"
#include <algorithm>

namespace RVX
{
    ShaderCompileService::ShaderCompileService(const Config& config)
        : m_config(config)
        , m_compiler(CreateShaderCompiler())
    {
        // 启动工作线程
        uint32 workerCount = std::max(1u, config.maxConcurrentCompiles);
        m_workers.reserve(workerCount);
        for (uint32 i = 0; i < workerCount; ++i)
        {
            m_workers.emplace_back(&ShaderCompileService::WorkerThread, this);
        }

        RVX_CORE_INFO("ShaderCompileService: Started with {} worker threads", workerCount);
    }

    ShaderCompileService::~ShaderCompileService()
    {
        // 关闭
        m_shutdown.store(true);
        m_queueCV.notify_all();

        for (auto& worker : m_workers)
        {
            if (worker.joinable())
                worker.join();
        }
    }

    ShaderCompileResult ShaderCompileService::CompileSync(const ShaderCompileOptions& options)
    {
        // 直接在当前线程编译
        auto start = std::chrono::steady_clock::now();
        ShaderCompileResult result = m_compiler->Compile(options);
        auto end = std::chrono::steady_clock::now();

        // 更新统计
        if (m_config.enableStatistics)
        {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            m_stats.totalCompiles++;
            m_stats.totalCompileTimeMs += duration.count();
            m_stats.averageCompileTimeMs = m_stats.totalCompileTimeMs / m_stats.totalCompiles;
            if (result.success)
                m_stats.successCount++;
            else
                m_stats.failureCount++;
        }

        return result;
    }

    CompileHandle ShaderCompileService::CompileAsync(
        const ShaderCompileOptions& options,
        CompileCallback onComplete,
        CompilePriority priority)
    {
        CompileHandle handle = GenerateHandle();

        // 创建任务
        {
            std::lock_guard lock(m_tasksMutex);
            CompileTask task;
            task.handle = handle;
            task.status = CompileStatus::Pending;
            task.callback = onComplete;
            task.submitTime = std::chrono::steady_clock::now();
            m_tasks.emplace(handle, std::move(task));
        }

        // 加入队列
        {
            std::lock_guard lock(m_queueMutex);
            CompileRequest request;
            request.options = options;
            request.callback = onComplete;
            request.priority = priority;
            request.handle = handle;

            // 按优先级插入
            auto it = std::lower_bound(m_pendingQueue.begin(), m_pendingQueue.end(), request,
                [](const CompileRequest& a, const CompileRequest& b) {
                    return static_cast<uint8>(a.priority) > static_cast<uint8>(b.priority);
                });
            m_pendingQueue.insert(it, std::move(request));
        }

        m_queueCV.notify_one();
        return handle;
    }

    void ShaderCompileService::WorkerThread()
    {
        while (!m_shutdown.load())
        {
            CompileRequest request;

            // 获取任务
            {
                std::unique_lock lock(m_queueMutex);
                m_queueCV.wait(lock, [this] {
                    return m_shutdown.load() || !m_pendingQueue.empty();
                });

                if (m_shutdown.load() && m_pendingQueue.empty())
                    break;

                if (m_pendingQueue.empty())
                    continue;

                request = std::move(m_pendingQueue.back());
                m_pendingQueue.pop_back();
            }

            m_activeCount++;

            // 更新状态为编译中
            {
                std::lock_guard lock(m_tasksMutex);
                auto it = m_tasks.find(request.handle);
                if (it != m_tasks.end())
                {
                    it->second.status = CompileStatus::Compiling;
                }
            }

            // 执行编译
            auto start = std::chrono::steady_clock::now();
            ShaderCompileResult result = m_compiler->Compile(request.options);
            auto end = std::chrono::steady_clock::now();

            // 更新任务状态
            {
                std::lock_guard lock(m_tasksMutex);
                auto it = m_tasks.find(request.handle);
                if (it != m_tasks.end())
                {
                    it->second.result = std::move(result);
                    it->second.status = it->second.result.success ? 
                        CompileStatus::Completed : CompileStatus::Failed;
                    it->second.completeTime = end;
                }
            }

            // 回调
            if (request.callback)
            {
                std::lock_guard lock(m_tasksMutex);
                auto it = m_tasks.find(request.handle);
                if (it != m_tasks.end())
                {
                    request.callback(it->second.result);
                }
            }

            // 更新统计
            if (m_config.enableStatistics)
            {
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                m_stats.totalCompiles++;
                m_stats.totalCompileTimeMs += duration.count();
                m_stats.averageCompileTimeMs = m_stats.totalCompileTimeMs / m_stats.totalCompiles;

                std::lock_guard lock(m_tasksMutex);
                auto it = m_tasks.find(request.handle);
                if (it != m_tasks.end())
                {
                    if (it->second.result.success)
                        m_stats.successCount++;
                    else
                        m_stats.failureCount++;
                }
            }

            m_activeCount--;
        }
    }

    ShaderCompileResult ShaderCompileService::Wait(CompileHandle handle)
    {
        while (true)
        {
            {
                std::lock_guard lock(m_tasksMutex);
                auto it = m_tasks.find(handle);
                if (it != m_tasks.end())
                {
                    if (it->second.status == CompileStatus::Completed ||
                        it->second.status == CompileStatus::Failed ||
                        it->second.status == CompileStatus::Cancelled)
                    {
                        return it->second.result;
                    }
                }
                else
                {
                    // 任务不存在
                    ShaderCompileResult result;
                    result.errorMessage = "Invalid compile handle";
                    return result;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool ShaderCompileService::IsComplete(CompileHandle handle) const
    {
        std::lock_guard lock(m_tasksMutex);
        auto it = m_tasks.find(handle);
        if (it != m_tasks.end())
        {
            return it->second.status == CompileStatus::Completed ||
                   it->second.status == CompileStatus::Failed ||
                   it->second.status == CompileStatus::Cancelled;
        }
        return true; // 不存在的任务视为已完成
    }

    CompileHandle ShaderCompileService::GenerateHandle()
    {
        return m_nextHandle.fetch_add(1);
    }

} // namespace RVX
```

---

## 五、Phase 2：缓存系统改进

### 5.1 ShaderCacheFormat.h - 缓存格式

```cpp
#pragma once

#include "Core/Types.h"
#include "RHI/RHIDefinitions.h"

namespace RVX
{
    // =========================================================================
    // 缓存文件魔数和版本
    // =========================================================================
    constexpr uint32 RVX_SHADER_CACHE_MAGIC = 0x52565853; // "RVXS"
    constexpr uint32 RVX_SHADER_CACHE_VERSION = 1;

    // =========================================================================
    // 缓存标志
    // =========================================================================
    enum class ShaderCacheFlags : uint32
    {
        None = 0,
        DebugInfo = 1 << 0,
        Optimized = 1 << 1,
        HasReflection = 1 << 2,
        HasSourceInfo = 1 << 3,
        HasMSLSource = 1 << 4,    // Metal
        HasGLSLSource = 1 << 5,   // OpenGL
    };

    inline ShaderCacheFlags operator|(ShaderCacheFlags a, ShaderCacheFlags b)
    {
        return static_cast<ShaderCacheFlags>(static_cast<uint32>(a) | static_cast<uint32>(b));
    }

    inline bool HasFlag(ShaderCacheFlags flags, ShaderCacheFlags flag)
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    // =========================================================================
    // 缓存文件头
    // =========================================================================
    #pragma pack(push, 1)
    struct ShaderCacheHeader
    {
        uint32 magic = RVX_SHADER_CACHE_MAGIC;
        uint32 version = RVX_SHADER_CACHE_VERSION;
        uint64 contentHash;            // 整个内容的 CRC64 校验
        uint64 compilerVersion;        // 编译器版本标识
        uint64 timestamp;              // 编译时间戳
        
        RHIBackendType backend;        // 目标后端
        RHIShaderStage stage;          // 着色器阶段
        uint16 padding1;
        
        ShaderCacheFlags flags;        // 缓存标志
        
        // 数据段偏移和大小
        uint32 bytecodeOffset;
        uint32 bytecodeSize;
        uint32 reflectionOffset;
        uint32 reflectionSize;
        uint32 sourceInfoOffset;
        uint32 sourceInfoSize;
        uint32 mslSourceOffset;        // Metal MSL 源码
        uint32 mslSourceSize;
        uint32 glslSourceOffset;       // OpenGL GLSL 源码
        uint32 glslSourceSize;
        
        uint32 reserved[8];            // 保留扩展
    };
    #pragma pack(pop)

    static_assert(sizeof(ShaderCacheHeader) == 128, "ShaderCacheHeader size mismatch");

    // =========================================================================
    // 文件布局
    // =========================================================================
    // [ShaderCacheHeader - 128 bytes]
    // [Bytecode - variable]
    // [Reflection - variable, serialized]
    // [SourceInfo - variable, serialized]
    // [MSL Source - variable, optional]
    // [GLSL Source - variable, optional]

} // namespace RVX
```

### 5.2 ShaderSourceInfo.h - 源文件依赖

```cpp
#pragma once

#include "Core/Types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>

namespace RVX
{
    // =========================================================================
    // 着色器源文件依赖信息
    // =========================================================================
    struct ShaderSourceInfo
    {
        std::string mainFile;                              // 主文件路径
        std::vector<std::string> includeFiles;             // 包含的文件列表
        std::unordered_map<std::string, uint64> fileHashes; // 文件哈希映射
        uint64 combinedHash = 0;                           // 组合哈希

        // =====================================================================
        // 方法
        // =====================================================================

        /** @brief 计算组合哈希（包含所有文件） */
        uint64 ComputeCombinedHash() const;

        /** @brief 检查是否有文件发生变化 */
        bool HasChanged(const std::filesystem::path& baseDir = {}) const;

        /** @brief 添加 include 文件 */
        void AddInclude(const std::string& path, uint64 hash);

        /** @brief 清空 */
        void Clear();

        /** @brief 序列化 */
        void Serialize(std::vector<uint8>& out) const;

        /** @brief 反序列化 */
        static ShaderSourceInfo Deserialize(const uint8* data, size_t size);

        /** @brief 计算文件哈希 */
        static uint64 ComputeFileHash(const std::filesystem::path& path);
    };

} // namespace RVX
```

### 5.3 ShaderCacheManager.h

```cpp
#pragma once

#include "ShaderCompiler/ShaderCacheFormat.h"
#include "ShaderCompiler/ShaderSourceInfo.h"
#include "ShaderCompiler/ShaderReflection.h"
#include "ShaderCompiler/ShaderCompiler.h"
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace RVX
{
    // =========================================================================
    // 缓存条目
    // =========================================================================
    struct ShaderCacheEntry
    {
        std::vector<uint8> bytecode;
        ShaderReflection reflection;
        ShaderSourceInfo sourceInfo;
        uint64 timestamp = 0;

        // 后端特定数据
        std::string mslSource;         // Metal
        std::string mslEntryPoint;
        std::string glslSource;        // OpenGL
        uint32 glslVersion = 450;
    };

    // =========================================================================
    // 缓存管理器
    // =========================================================================
    class ShaderCacheManager
    {
    public:
        // =====================================================================
        // Configuration
        // =====================================================================
        struct Config
        {
            std::filesystem::path cacheDirectory;
            uint64 maxCacheSizeBytes = 512 * 1024 * 1024; // 512 MB
            bool enableMemoryCache = true;
            bool enableDiskCache = true;
            bool validateOnLoad = true;  // 加载时验证依赖
        };

        explicit ShaderCacheManager(const Config& config = {});
        ~ShaderCacheManager() = default;

        // 禁止拷贝
        ShaderCacheManager(const ShaderCacheManager&) = delete;
        ShaderCacheManager& operator=(const ShaderCacheManager&) = delete;

        // =====================================================================
        // 缓存操作
        // =====================================================================

        /** @brief 加载缓存条目 */
        std::optional<ShaderCacheEntry> Load(uint64 key);

        /** @brief 保存缓存条目 */
        void Save(uint64 key, const ShaderCacheEntry& entry);

        /** @brief 检查缓存是否有效（依赖是否变化） */
        bool IsValid(uint64 key, const ShaderSourceInfo& currentInfo);

        /** @brief 使指定缓存失效 */
        void Invalidate(uint64 key);

        /** @brief 使所有缓存失效 */
        void InvalidateAll();

        /** @brief 清理内存缓存 */
        void ClearMemoryCache();

        // =====================================================================
        // 缓存目录管理
        // =====================================================================

        /** @brief 设置缓存目录 */
        void SetCacheDirectory(const std::filesystem::path& dir);

        /** @brief 获取缓存目录 */
        const std::filesystem::path& GetCacheDirectory() const { return m_config.cacheDirectory; }

        /** @brief 清理过期缓存 */
        void PruneCache(uint64 maxAgeSeconds = 7 * 24 * 3600); // 默认 7 天

        /** @brief 限制缓存大小 */
        void EnforceSizeLimit();

        // =====================================================================
        // 统计
        // =====================================================================
        struct Statistics
        {
            uint64 memoryHits = 0;
            uint64 diskHits = 0;
            uint64 misses = 0;
            uint64 invalidations = 0;
            uint64 memoryCacheSize = 0;
            uint64 diskCacheSize = 0;

            float GetHitRate() const
            {
                uint64 total = memoryHits + diskHits + misses;
                return total > 0 ? static_cast<float>(memoryHits + diskHits) / total : 0.0f;
            }
        };

        const Statistics& GetStatistics() const { return m_stats; }

    private:
        // =====================================================================
        // 内部实现
        // =====================================================================
        std::filesystem::path GetCachePath(uint64 key) const;
        bool LoadFromDisk(uint64 key, ShaderCacheEntry& entry);
        void SaveToDisk(uint64 key, const ShaderCacheEntry& entry);
        bool ValidateHeader(const ShaderCacheHeader& header) const;

        Config m_config;

        // 内存缓存
        mutable std::shared_mutex m_cacheMutex;
        std::unordered_map<uint64, ShaderCacheEntry> m_memoryCache;

        // 统计
        Statistics m_stats;
    };

} // namespace RVX
```

### 5.4 TrackingIncludeHandler.h - DXC Include 追踪

```cpp
#pragma once

#ifdef _WIN32

#include "ShaderCompiler/ShaderSourceInfo.h"
#include <Windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>

namespace RVX
{
    using Microsoft::WRL::ComPtr;

    // =========================================================================
    // 追踪 Include 的 DXC Handler
    // =========================================================================
    class TrackingIncludeHandler : public IDxcIncludeHandler
    {
    public:
        TrackingIncludeHandler(
            ComPtr<IDxcUtils> utils,
            const std::filesystem::path& baseDir);

        // IUnknown
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;

        // IDxcIncludeHandler
        HRESULT STDMETHODCALLTYPE LoadSource(
            LPCWSTR pFilename,
            IDxcBlob** ppIncludeSource) override;

        // 获取追踪的 include 信息
        const ShaderSourceInfo& GetSourceInfo() const { return m_sourceInfo; }

        // 设置主文件信息
        void SetMainFile(const std::string& path, uint64 hash);

        // 添加额外的 include 目录
        void AddIncludeDirectory(const std::filesystem::path& dir);

    private:
        std::filesystem::path ResolveInclude(const std::wstring& filename) const;

        ComPtr<IDxcUtils> m_utils;
        std::filesystem::path m_baseDir;
        std::vector<std::filesystem::path> m_includeDirs;
        ShaderSourceInfo m_sourceInfo;
        ULONG m_refCount = 1;
    };

} // namespace RVX

#endif // _WIN32
```

---

## 六、Phase 3：Shader Permutation 系统

### 6.1 ShaderPermutation.h

```cpp
#pragma once

#include "ShaderCompiler/ShaderCompiler.h"
#include "ShaderCompiler/ShaderTypes.h"
#include "RHI/RHIShader.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace RVX
{
    class ShaderCompileService;
    class ShaderCacheManager;
    class IRHIDevice;

    // =========================================================================
    // 变体维度定义
    // =========================================================================
    struct ShaderPermutationDimension
    {
        std::string name;                     // 宏名称 e.g. "LIGHTING_MODEL"
        std::vector<std::string> values;      // 可选值 e.g. {"PHONG", "PBR", "UNLIT"}
        bool optional = false;                // 是否可选（可以完全不定义）
        std::string defaultValue;             // 默认值
    };

    // =========================================================================
    // 变体空间（所有可能的变体组合）
    // =========================================================================
    struct ShaderPermutationSpace
    {
        std::vector<ShaderPermutationDimension> dimensions;

        /** @brief 获取所有变体数量 */
        uint64 GetTotalVariantCount() const;

        /** @brief 枚举所有变体组合 */
        std::vector<std::vector<ShaderMacro>> EnumerateAll() const;

        /** @brief 计算特定变体的哈希 */
        uint64 ComputePermutationHash(const std::vector<ShaderMacro>& defines) const;

        /** @brief 规范化定义（按名称排序，填充默认值） */
        std::vector<ShaderMacro> Normalize(const std::vector<ShaderMacro>& defines) const;

        /** @brief 验证定义是否在空间内 */
        bool IsValid(const std::vector<ShaderMacro>& defines) const;
    };

    // =========================================================================
    // 变体优先级（用于预热顺序）
    // =========================================================================
    enum class VariantPriority : uint8
    {
        Critical = 0,   // 启动必需
        High = 1,       // 常用
        Medium = 2,     // 偶尔使用
        Low = 3         // 罕见
    };

    // =========================================================================
    // Shader Permutation 系统
    // =========================================================================
    class ShaderPermutationSystem
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================
        ShaderPermutationSystem(
            ShaderCompileService* compileService,
            ShaderCacheManager* cacheManager);

        ~ShaderPermutationSystem() = default;

        // =====================================================================
        // 着色器注册
        // =====================================================================

        /** @brief 注册着色器及其变体空间 */
        void RegisterShader(
            const std::string& shaderPath,
            const ShaderPermutationSpace& space,
            const ShaderLoadDesc& baseDesc);

        /** @brief 取消注册 */
        void UnregisterShader(const std::string& shaderPath);

        /** @brief 检查是否已注册 */
        bool IsRegistered(const std::string& shaderPath) const;

        // =====================================================================
        // 变体获取
        // =====================================================================

        /** @brief 获取或创建变体（可能触发编译） */
        RHIShaderRef GetVariant(
            IRHIDevice* device,
            const std::string& shaderPath,
            const std::vector<ShaderMacro>& defines);

        /** @brief 异步获取变体 */
        CompileHandle GetVariantAsync(
            IRHIDevice* device,
            const std::string& shaderPath,
            const std::vector<ShaderMacro>& defines,
            std::function<void(RHIShaderRef)> callback);

        /** @brief 检查变体是否已编译 */
        bool HasVariant(
            const std::string& shaderPath,
            const std::vector<ShaderMacro>& defines) const;

        // =====================================================================
        // 预热
        // =====================================================================

        /** @brief 预热指定变体（后台编译） */
        void PrewarmVariants(
            IRHIDevice* device,
            const std::string& shaderPath,
            const std::vector<std::vector<ShaderMacro>>& variants,
            VariantPriority priority = VariantPriority::Medium);

        /** @brief 预热所有变体 */
        void PrewarmAllVariants(
            IRHIDevice* device,
            const std::string& shaderPath,
            VariantPriority priority = VariantPriority::Low);

        /** @brief 获取预热进度 */
        float GetPrewarmProgress(const std::string& shaderPath) const;

        // =====================================================================
        // 统计
        // =====================================================================

        /** @brief 获取已编译变体数量 */
        uint32 GetCompiledVariantCount(const std::string& shaderPath) const;

        /** @brief 获取总变体数量 */
        uint32 GetTotalVariantCount(const std::string& shaderPath) const;

        /** @brief 获取等待编译的变体数量 */
        uint32 GetPendingCompileCount() const;

        /** @brief 清除所有变体 */
        void ClearVariants(const std::string& shaderPath);

        /** @brief 清除所有着色器的变体 */
        void ClearAllVariants();

    private:
        // =====================================================================
        // 内部结构
        // =====================================================================
        struct ShaderEntry
        {
            std::string source;
            ShaderPermutationSpace space;
            ShaderLoadDesc baseDesc;
            std::unordered_map<uint64, RHIShaderRef> variants;
            std::unordered_map<uint64, CompileHandle> pendingCompiles;
            mutable std::mutex mutex;
        };

        uint64 ComputeVariantKey(
            const std::string& shaderPath,
            const std::vector<ShaderMacro>& defines) const;

        ShaderCompileService* m_compileService;
        ShaderCacheManager* m_cacheManager;

        mutable std::shared_mutex m_shadersMutex;
        std::unordered_map<std::string, std::unique_ptr<ShaderEntry>> m_shaders;
    };

} // namespace RVX
```

---

## 七、Phase 4：Hot Reload 系统

### 7.1 ShaderHotReloader.h

```cpp
#pragma once

#include "ShaderCompiler/ShaderTypes.h"
#include "RHI/RHIShader.h"
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

namespace RVX
{
    class ShaderCompileService;
    class ShaderCacheManager;
    class IRHIDevice;

    // =========================================================================
    // 文件变化事件
    // =========================================================================
    struct ShaderFileChange
    {
        std::string path;
        enum class Type : uint8 { Modified, Created, Deleted } type;
        uint64 timestamp;
    };

    // =========================================================================
    // 热重载回调
    // =========================================================================
    struct ShaderReloadInfo
    {
        std::string shaderPath;
        RHIShaderRef oldShader;
        RHIShaderRef newShader;
        bool success;
        std::string errorMessage;
    };

    using ShaderReloadCallback = std::function<void(const ShaderReloadInfo&)>;

    // =========================================================================
    // Shader Hot Reloader
    // =========================================================================
    class ShaderHotReloader
    {
    public:
        // =====================================================================
        // Configuration
        // =====================================================================
        struct Config
        {
            std::vector<std::filesystem::path> watchDirectories;
            std::vector<std::string> watchExtensions = {".hlsl", ".hlsli", ".ush", ".usf"};
            uint32 pollIntervalMs = 100;      // 轮询间隔
            uint32 debounceMs = 200;          // 防抖延迟
            bool enabled = true;
        };

        ShaderHotReloader(
            ShaderCompileService* compileService,
            ShaderCacheManager* cacheManager,
            const Config& config = {});

        ~ShaderHotReloader();

        // 禁止拷贝
        ShaderHotReloader(const ShaderHotReloader&) = delete;
        ShaderHotReloader& operator=(const ShaderHotReloader&) = delete;

        // =====================================================================
        // 启用/禁用
        // =====================================================================

        void Enable();
        void Disable();
        bool IsEnabled() const { return m_enabled.load(); }

        // =====================================================================
        // 注册着色器
        // =====================================================================

        /** @brief 注册需要热重载的着色器 */
        void RegisterShader(
            IRHIDevice* device,
            const std::string& shaderPath,
            RHIShaderRef shader,
            const ShaderLoadDesc& loadDesc,
            ShaderReloadCallback callback = nullptr);

        /** @brief 取消注册 */
        void UnregisterShader(const std::string& shaderPath);

        /** @brief 取消注册特定着色器实例 */
        void UnregisterShaderInstance(RHIShaderRef shader);

        // =====================================================================
        // 更新
        // =====================================================================

        /** @brief 每帧调用，处理文件变化 */
        void Update();

        /** @brief 强制重载指定着色器 */
        void ForceReload(const std::string& shaderPath);

        /** @brief 强制重载所有着色器 */
        void ForceReloadAll();

        // =====================================================================
        // 监视目录
        // =====================================================================

        void AddWatchDirectory(const std::filesystem::path& dir);
        void RemoveWatchDirectory(const std::filesystem::path& dir);
        void ClearWatchDirectories();

        // =====================================================================
        // 全局回调
        // =====================================================================

        void SetGlobalReloadCallback(ShaderReloadCallback callback);

    private:
        // =====================================================================
        // 内部结构
        // =====================================================================
        struct WatchedShader
        {
            std::string path;
            ShaderLoadDesc loadDesc;
            std::vector<RHIShaderRef> instances;
            std::vector<ShaderReloadCallback> callbacks;
            std::unordered_set<std::string> dependencies; // include 文件
            uint64 lastModifiedTime = 0;
            IRHIDevice* device = nullptr;
        };

        struct PendingChange
        {
            std::string path;
            uint64 timestamp;
        };

        void WatcherThread();
        void ProcessFileChange(const ShaderFileChange& change);
        void ReloadShader(WatchedShader& shader);
        std::vector<std::string> GetAffectedShaders(const std::string& changedFile);

        Config m_config;
        ShaderCompileService* m_compileService;
        ShaderCacheManager* m_cacheManager;

        // 监视状态
        std::atomic<bool> m_enabled{false};
        std::atomic<bool> m_shutdown{false};
        std::thread m_watcherThread;

        // 已注册的着色器
        mutable std::mutex m_shadersMutex;
        std::unordered_map<std::string, WatchedShader> m_watchedShaders;

        // 待处理的变化（防抖）
        std::mutex m_pendingMutex;
        std::unordered_map<std::string, PendingChange> m_pendingChanges;

        // 全局回调
        ShaderReloadCallback m_globalCallback;

        // 平台特定的文件监视器
        class FileWatcherImpl;
        std::unique_ptr<FileWatcherImpl> m_fileWatcher;
    };

} // namespace RVX
```

---

## 八、Phase 5：重构 ShaderManager

### 8.1 新 ShaderManager.h

```cpp
#pragma once

#include "ShaderCompiler/ShaderCompiler.h"
#include "ShaderCompiler/ShaderTypes.h"
#include "ShaderCompiler/ShaderCompileService.h"
#include "ShaderCompiler/ShaderCacheManager.h"
#include "ShaderCompiler/ShaderPermutation.h"
#include "ShaderCompiler/ShaderHotReloader.h"
#include "RHI/RHIShader.h"
#include <memory>

namespace RVX
{
    class IRHIDevice;

    // =========================================================================
    // ShaderManager 配置
    // =========================================================================
    struct ShaderManagerConfig
    {
        // 缓存配置
        std::filesystem::path cacheDirectory;
        uint64 maxCacheSizeBytes = 512 * 1024 * 1024;
        bool enableMemoryCache = true;
        bool enableDiskCache = true;

        // 编译配置
        uint32 maxConcurrentCompiles = 4;
        bool enableAsyncCompile = true;

        // 热重载配置
        bool enableHotReload = false;  // 默认关闭，编辑器/开发模式开启
        std::vector<std::filesystem::path> shaderDirectories;

        // 统计
        bool enableStatistics = true;
    };

    // =========================================================================
    // ShaderManager 统计信息
    // =========================================================================
    struct ShaderManagerStats
    {
        // 编译统计
        ShaderCompileService::Statistics compileStats;
        
        // 缓存统计
        ShaderCacheManager::Statistics cacheStats;
        
        // 变体统计
        uint32 totalRegisteredShaders = 0;
        uint32 totalCompiledVariants = 0;
        uint32 pendingCompiles = 0;

        // 热重载统计
        uint32 reloadCount = 0;
        uint32 reloadSuccessCount = 0;
        uint32 reloadFailureCount = 0;
    };

    // =========================================================================
    // 加载描述
    // =========================================================================
    struct ShaderLoadDesc
    {
        std::string path;
        std::string entryPoint;
        RHIShaderStage stage = RHIShaderStage::None;
        RHIBackendType backend = RHIBackendType::DX12;
        std::string targetProfile;
        std::vector<ShaderMacro> defines;
        bool enableDebugInfo = false;
        bool enableOptimization = true;
    };

    // =========================================================================
    // 加载结果
    // =========================================================================
    struct ShaderLoadResult
    {
        RHIShaderRef shader;
        ShaderCompileResult compileResult;
    };

    // =========================================================================
    // ShaderManager - 统一的着色器管理门面
    // =========================================================================
    class ShaderManager
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================
        explicit ShaderManager(const ShaderManagerConfig& config = {});
        ~ShaderManager();

        // 禁止拷贝
        ShaderManager(const ShaderManager&) = delete;
        ShaderManager& operator=(const ShaderManager&) = delete;

        // =====================================================================
        // 同步加载（兼容旧接口）
        // =====================================================================

        /** @brief 从文件加载着色器（同步，可能阻塞） */
        ShaderLoadResult LoadFromFile(IRHIDevice* device, const ShaderLoadDesc& desc);

        /** @brief 从源码加载着色器（同步） */
        ShaderLoadResult LoadFromSource(
            IRHIDevice* device,
            const ShaderLoadDesc& desc,
            const std::string& source);

        // =====================================================================
        // 异步加载
        // =====================================================================

        /** @brief 从文件异步加载着色器 */
        CompileHandle LoadFromFileAsync(
            IRHIDevice* device,
            const ShaderLoadDesc& desc,
            LoadCallback onComplete);

        /** @brief 从源码异步加载着色器 */
        CompileHandle LoadFromSourceAsync(
            IRHIDevice* device,
            const ShaderLoadDesc& desc,
            const std::string& source,
            LoadCallback onComplete);

        /** @brief 等待异步加载完成 */
        ShaderLoadResult WaitForLoad(CompileHandle handle);

        /** @brief 检查异步加载是否完成 */
        bool IsLoadComplete(CompileHandle handle) const;

        // =====================================================================
        // 变体系统
        // =====================================================================

        /** @brief 注册着色器变体空间 */
        void RegisterShaderVariants(
            const std::string& shaderPath,
            const ShaderPermutationSpace& space,
            const ShaderLoadDesc& baseDesc);

        /** @brief 获取着色器变体 */
        RHIShaderRef GetShaderVariant(
            IRHIDevice* device,
            const std::string& shaderPath,
            const std::vector<ShaderMacro>& defines);

        /** @brief 预热变体 */
        void PrewarmVariants(
            IRHIDevice* device,
            const std::string& shaderPath,
            const std::vector<std::vector<ShaderMacro>>& variants);

        // =====================================================================
        // 热重载
        // =====================================================================

        /** @brief 启用热重载（通常在编辑器模式下） */
        void EnableHotReload();

        /** @brief 禁用热重载 */
        void DisableHotReload();

        /** @brief 注册热重载回调 */
        void SetHotReloadCallback(ShaderReloadCallback callback);

        /** @brief 添加着色器监视目录 */
        void AddShaderWatchDirectory(const std::filesystem::path& dir);

        // =====================================================================
        // 更新（每帧调用）
        // =====================================================================

        /** @brief 每帧更新，处理热重载等 */
        void Update();

        // =====================================================================
        // 缓存管理
        // =====================================================================

        /** @brief 清除内存缓存 */
        void ClearMemoryCache();

        /** @brief 清除磁盘缓存 */
        void ClearDiskCache();

        /** @brief 使指定着色器缓存失效 */
        void InvalidateShader(const std::string& shaderPath);

        // =====================================================================
        // 统计信息
        // =====================================================================

        /** @brief 获取统计信息 */
        ShaderManagerStats GetStats() const;

        /** @brief 重置统计信息 */
        void ResetStats();

        // =====================================================================
        // 访问内部服务（高级用法）
        // =====================================================================

        ShaderCompileService* GetCompileService() { return m_compileService.get(); }
        ShaderCacheManager* GetCacheManager() { return m_cacheManager.get(); }
        ShaderPermutationSystem* GetPermutationSystem() { return m_permutationSystem.get(); }
        ShaderHotReloader* GetHotReloader() { return m_hotReloader.get(); }

    private:
        // =====================================================================
        // 内部实现
        // =====================================================================
        uint64 BuildCacheKey(const ShaderLoadDesc& desc, uint64 sourceHash) const;
        bool LoadFile(const std::string& path, std::string& outSource) const;

        ShaderManagerConfig m_config;

        std::unique_ptr<ShaderCompileService> m_compileService;
        std::unique_ptr<ShaderCacheManager> m_cacheManager;
        std::unique_ptr<ShaderPermutationSystem> m_permutationSystem;
        std::unique_ptr<ShaderHotReloader> m_hotReloader;

        // 异步加载任务映射
        mutable std::mutex m_loadTasksMutex;
        std::unordered_map<CompileHandle, ShaderLoadDesc> m_loadTasks;
    };

} // namespace RVX
```

---

## 九、DX12 升级到 DXC

### 9.1 修改 DXCCompiler.cpp

```cpp
// 在 DXCShaderCompiler::Compile 中修改 DX12 编译路径：

ShaderCompileResult Compile(const ShaderCompileOptions& options) override
{
    // ...

    // DX11: 继续使用 FXC 保证 SM5 兼容性
    if (options.targetBackend == RHIBackendType::DX11)
    {
        return CompileWithFXC(options);
    }
    
    // DX12: 使用 DXC 支持 SM6.x
    if (options.targetBackend == RHIBackendType::DX12)
    {
        return CompileWithDXC_DX12(options);
    }

    // Vulkan/OpenGL: 使用 DXC -spirv
    // ...
}

ShaderCompileResult CompileWithDXC_DX12(const ShaderCompileOptions& options)
{
    ShaderCompileResult result;

    // 构建参数
    std::vector<LPCWSTR> args;
    std::wstring entryW = ToWide(options.entryPoint);
    std::wstring profileW;
    
    // 使用 SM6.0+ profile
    if (options.targetProfile && options.targetProfile[0])
    {
        profileW = ToWide(options.targetProfile);
    }
    else
    {
        profileW = GetSM6Profile(options.stage);
    }

    args.push_back(L"-E"); args.push_back(entryW.c_str());
    args.push_back(L"-T"); args.push_back(profileW.c_str());

    // 优化和调试标志
    if (options.enableDebugInfo)
    {
        args.push_back(L"-Zi");
        args.push_back(L"-Qembed_debug");
        args.push_back(L"-Od");
    }
    else if (options.enableOptimization)
    {
        args.push_back(L"-O3");
    }

    // DX12 特定：启用 root signature auto
    args.push_back(L"-rootsig-define");
    args.push_back(L"RS");

    // 使用追踪 Include Handler
    auto includeHandler = std::make_unique<TrackingIncludeHandler>(
        m_utils, GetSourceDirectory(options.sourcePath));

    // 编译
    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = options.sourceCode;
    sourceBuffer.Size = strlen(options.sourceCode);
    sourceBuffer.Encoding = DXC_CP_UTF8;

    ComPtr<IDxcResult> dxcResult;
    HRESULT hr = m_compiler->Compile(
        &sourceBuffer,
        args.data(),
        static_cast<uint32_t>(args.size()),
        includeHandler.get(),
        IID_PPV_ARGS(&dxcResult));

    // ... 处理结果 ...

    // 保存 Include 信息
    result.sourceInfo = includeHandler->GetSourceInfo();

    return result;
}

const wchar_t* GetSM6Profile(RHIShaderStage stage)
{
    switch (stage)
    {
        case RHIShaderStage::Vertex:   return L"vs_6_0";
        case RHIShaderStage::Pixel:    return L"ps_6_0";
        case RHIShaderStage::Compute:  return L"cs_6_0";
        case RHIShaderStage::Geometry: return L"gs_6_0";
        case RHIShaderStage::Hull:     return L"hs_6_0";
        case RHIShaderStage::Domain:   return L"ds_6_0";
        case RHIShaderStage::Mesh:     return L"ms_6_5";  // 新增
        case RHIShaderStage::Amplification: return L"as_6_5";  // 新增
        default: return L"vs_6_0";
    }
}
```

---

## 十、CMakeLists.txt 更新

```cmake
# ShaderCompiler/CMakeLists.txt

set(SHADER_COMPILER_SOURCES
    # 公共头文件
    Include/ShaderCompiler/ShaderCompiler.h
    Include/ShaderCompiler/ShaderManager.h
    Include/ShaderCompiler/ShaderReflection.h
    Include/ShaderCompiler/ShaderLayout.h
    Include/ShaderCompiler/ShaderTypes.h
    Include/ShaderCompiler/ShaderCompileService.h
    Include/ShaderCompiler/ShaderCacheManager.h
    Include/ShaderCompiler/ShaderCacheFormat.h
    Include/ShaderCompiler/ShaderSourceInfo.h
    Include/ShaderCompiler/ShaderPermutation.h
    Include/ShaderCompiler/ShaderHotReloader.h
    Include/ShaderCompiler/ShaderCompileStats.h

    # 实现文件
    Private/ShaderCompiler.cpp
    Private/ShaderManager.cpp
    Private/ShaderReflection.cpp
    Private/ShaderLayout.cpp
    Private/ShaderCompileService.cpp
    Private/ShaderCacheManager.cpp
    Private/ShaderSourceInfo.cpp
    Private/ShaderPermutation.cpp
    Private/ShaderHotReloader.cpp
    Private/ShaderFileWatcher.cpp
    Private/ShaderFileWatcher.h
    Private/SPIRVCrossTranslator.cpp
    Private/SPIRVCrossTranslator.h
)

# 平台特定文件
if(WIN32)
    list(APPEND SHADER_COMPILER_SOURCES
        Private/DXCCompiler.cpp
        Private/TrackingIncludeHandler.cpp
        Private/TrackingIncludeHandler.h
        Private/DX11SlotMapper.cpp
    )
elseif(APPLE)
    list(APPEND SHADER_COMPILER_SOURCES
        Private/DXCCompiler_Apple.cpp
    )
endif()

add_library(ShaderCompiler STATIC ${SHADER_COMPILER_SOURCES})

target_include_directories(ShaderCompiler
    PUBLIC Include
    PRIVATE Private
)

target_link_libraries(ShaderCompiler
    PUBLIC Core RHI
    PRIVATE spirv-cross-core spirv-cross-glsl
)

if(WIN32)
    target_link_libraries(ShaderCompiler
        PRIVATE d3dcompiler dxcompiler
    )
endif()

if(APPLE)
    target_link_libraries(ShaderCompiler
        PRIVATE glslang::glslang glslang::SPIRV
        spirv-cross-msl spirv-cross-hlsl
    )
endif()
```

---

## 十一、实施路线图


| 阶段          | 任务               | 预期产出                              |
| ----------- | ---------------- | --------------------------------- |
| **Phase 1** | 异步编译服务           | ShaderCompileService 完整实现         |
| **Phase 2** | 缓存系统改进           | 新缓存格式 + Include 追踪 + CacheManager |
| **Phase 3** | 变体系统             | ShaderPermutationSystem 完整实现      |
| **Phase 4** | 热重载              | ShaderHotReloader + FileWatcher   |
| **Phase 5** | 整合重构             | 新 ShaderManager + 向后兼容            |
| **补充**      | DX12 升级          | SM6.x 支持 + DXC 编译                 |
| **补充**      | DX11 Slot Mapper | 完整的 DX11 资源槽位映射                   |


---

## 十二、向后兼容性保证

1. **保留原有接口**：`ShaderManager::LoadFromFile` 签名不变
2. **渐进式迁移**：旧代码无需修改即可继续工作
3. **功能开关**：异步编译、热重载等新功能通过配置开启
4. **旧缓存兼容**：能够读取旧格式缓存（版本检测后重新编译）

