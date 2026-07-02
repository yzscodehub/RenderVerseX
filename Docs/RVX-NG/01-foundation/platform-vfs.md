# RVX-NG · Foundation/Platform & VFS 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §5 Platform/VFS
**里程碑**：M0
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：把 OS/主机差异收敛到一层；提供**虚拟文件系统（VFS）**统一挂载 pak/loose/mod。

**范围内**：窗口、原始输入事件源、时间源、线程/同步原语、虚拟内存、动态库、主机 SDK 接缝、VFS 挂载与解析。
**范围外**：图形设备（RHI）、资产语义（Asset）、输入映射（Input）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 平台收敛 | 上层零 `#ifdef PLATFORM`；差异仅在本层 |
| 主机 | 抽象主机 SDK 接缝（账号/存储/成就/认证）不漏原生类型（C7） |
| 线程原语 | 线程创建/亲和/挂起-唤醒/原子/futex 抽象（供 JobSystem） |
| 虚拟内存 | reserve/commit/protect/大页（供 Memory） |
| VFS | pak + loose + mod **叠加挂载**，按优先级解析，路径无关后端 |

## 3. 公共接口面（设计契约）

```cpp
// 平台抽象（每平台一份实现，编译期选择）
namespace Platform {
    Window CreateWindow(const WindowDesc&);
    void   PumpEvents();                 // 主线程；产出输入/窗口事件
    u64    NowNanos();
    Thread SpawnThread(Affinity, Entry); void ParkThread(); void UnparkThread(Thread);
    void*  VmReserve(usize); bool VmCommit(void*, usize); void VmDecommit(void*, usize);
    Library LoadLibrary(Path);  void* Symbol(Library, Name);
}

// 主机接缝（facade，不漏原生类型）
struct IHostPlatform {
    Result<UserId>  SignIn();
    Result<void>    SaveToSlot(SlotId, Span<byte>);  Result<Buffer> LoadSlot(SlotId);
    void            UnlockAchievement(Name);
    PlatformCaps    Caps() const;        // 内存/存储/认证要求
};

// 虚拟文件系统
class IVfs {
    void   Mount(MountSpec, Priority);   // pak / loose dir / mod，叠加
    Result<Stream> Open(VfsPath);         // 按优先级解析，高优先覆盖
    Result<Buffer> ReadAll(VfsPath);
    bool   Exists(VfsPath);
    void   Watch(VfsPath, Callback);      // loose 热重载
};
```

**不变量**：上层只用 `Platform::*`/`IVfs`/`IHostPlatform`，**永不**直接调 Win32/POSIX/主机 SDK；VFS 解析按挂载优先级（mod > patch > base）。

## 4. 数据结构

- **VFS 挂载表**：有序挂载点列表（pak 索引 / loose 目录 / mod 包），每项带优先级。
- **pak 索引**：`VfsPath→(packId, offset, size, compression)`，启动期加载，mmap 友好。
- **线程/原语**：薄封装 OS 句柄。

## 5. 核心算法

- **VFS 解析**：自高优先级向下查首个命中 → 返回后端 Stream（pak slice / loose file）。mod 叠加即"高优先挂载点覆盖同路径"。
- **事件泵**：`PumpEvents` 在主线程把 OS 消息翻成引擎事件（输入/resize/close/focus），投递 Event/Input。
- **VM**：reserve 大区间 + 按需 commit（供 Memory 竞技场/堆）。

## 6. 线程与内存模型

- `PumpEvents`/窗口/主机 present **仅主线程**（C9 硬约束）。
- 线程/原语供 JobSystem；VFS Open 可在 worker（IO 经 Asset 异步层调度）。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| VFS 路径未命中 | `Result` 错误（非崩溃）；调用方降级 |
| pak 损坏/校验失败 | 拒绝挂载 + 报错；可回退 base |
| 主机 SDK 失败 | `Result` 错误 + 重试/离线降级 |
| 库加载失败 | `Result` 错误（可选特性禁用） |

## 8. 性能目标

- VFS 解析 O(1)~O(log) per 路径（哈希/有序）；命中走 mmap 零拷贝。
- `PumpEvents` < 0.05 ms 典型；不阻塞渲染（主线程预算内）。

## 9. 测试计划

- 单元：VFS 叠加优先级（mod 覆盖 base）、pak 索引解析、watch 热重载触发。
- 跨平台：同一 VFS 接口在各后端行为一致（headless 桩平台）。
- 主机接缝：mock 主机实现验证 facade 不漏类型（编译期 + 依赖检查 C7）。

## 10. 开放问题 / Spike

- 主机原生 API 的真实接缝深度（各主机 NDA 细节，占位接口）。
- VFS 异步 IO 与 Asset 流式调度的分层（谁拥有 IO 线程/优先级）。
- 大页在各平台的可用性与收益。

## 11. 依赖与被依赖

- **依赖**：Core（Result/Path/Name）。最底层之一（无上游引擎依赖）。
- **被依赖**：JobSystem（线程/原语）、Memory（VM）、Asset（VFS/IO）、Input（事件源）、Render（窗口/swapchain 宿主）、Shipping（pak/主机）。

