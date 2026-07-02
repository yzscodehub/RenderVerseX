# RVX-NG · App/Engine 装配层 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §14 App / Engine  
**里程碑**：M0-M11 贯穿  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义引擎应用装配层：启动、配置、模块/插件装配、子系统生命周期、主循环、应用模式（Game / Editor / PIE / Server / Cook）、GameInstance 接缝和退出流程。

**范围内**：Engine bootstrap、module registry、subsystem graph、app modes、main loop、command line/config layer、gameplay seams、shutdown、safe mode。  
**范围外**：具体 gameplay 框架实现、Editor 面板 UI、平台认证流程。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 拓扑生命周期 | 子系统按依赖 DAG 初始化，逆序关闭，环路 fail-fast |
| 多模式 | Game、Editor、PIE、Dedicated Server、Cook/Commandlet 共享装配机制 |
| 可裁剪 | Shipping profile 移除 editor/cook/debug-only 模块 |
| 可恢复 | 崩溃后可进入 safe mode 或禁用问题插件 |
| 可观测 | 启动阶段、模块耗时、失败原因进入 trace/log |

## 3. 公共接口面（设计契约）

```cpp
enum class AppMode : u8 {
    Game,
    Editor,
    PlayInEditor,
    DedicatedServer,
    Cook,
    Test
};

struct EngineDesc {
    AppMode mode;
    BuildProfile profile;
    Span<const ModuleId> requestedModules;
    ConfigStack config;
};

class Engine {
public:
    Result<void> Initialize(const EngineDesc& desc);
    void Run();
    void Tick();
    void RequestExit(ExitReason reason);
    void Shutdown();
};

class ModuleRegistry {
    void Register(ModuleDesc desc);
    Result<ModuleGraph> Resolve(AppMode mode, BuildProfile profile);
};
```

**不变量**：所有子系统由 ModuleGraph 装配；模块不得在静态初始化阶段访问其他模块；`Shutdown` 逆序释放；AppMode 决定装配差异，不允许各模块自行猜测运行模式。

## 4. 数据结构与内存布局

- **ModuleDesc**：模块 id、类型、public/private deps、profile flags、startup phase。
- **SubsystemDesc**：依赖、初始化函数、关闭函数、线程亲和、模式过滤。
- **ConfigStack**：默认配置、平台配置、项目配置、用户配置、命令行覆盖。
- **EngineState**：mode、frame index、world set、service locator、exit request。
- **Startup Trace**：每个模块 init/shutdown 的时间、结果、依赖。

## 5. 核心算法

- **Module resolve**：按 AppMode/Profile 过滤模块，构建 DAG，检测环与缺依赖。
- **Startup phases**：Platform → Log/Config → Memory → Jobs → Reflection → VFS → Asset → RHI → World/Render/Feature → App Shell。
- **Main loop**：交给 FrameScheduler 驱动 fixed sim、render submit、present、tool tick。
- **Mode dispatch**：Dedicated Server 不装 Render/RHI；Cook 不装 Runtime World；Editor 装 Tools 与多 World；PIE fork game world。
- **Safe mode**：启动失败或插件失败时禁用非核心模块，保留日志/诊断入口。

## 6. 线程与内存模型

初始化默认在主线程按拓扑执行；耗时预热（shader cache、asset scan、DDC check）可提交后台 job，但必须在声明的 barrier 前完成。主循环中 OS pump 与 present 保持平台亲和；其他阶段走 task graph。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 模块依赖环 | 初始化前 fail-fast，输出依赖环 |
| 必需模块失败 | 停止启动或进入 safe mode |
| 可选模块失败 | 禁用模块，记录诊断，继续启动 |
| 退出请求 | 标记 exit reason，在帧安全点执行 shutdown |

**禁止**：模块静态构造访问全局单例；隐式启动私有线程；绕过 ModuleGraph 手动初始化核心系统。

## 8. 性能目标

- Startup trace 可定位启动耗时。
- AppMode 裁剪后不加载无关模块。
- Shutdown 不泄露 in-flight job、GPU resource、package mount。

## 9. 测试计划

- 模块 DAG 环检测、缺依赖、profile 裁剪。
- Game/Editor/PIE/Server/Cook 模式装配 fixture。
- 初始化失败和 safe mode 注入。
- 退出流程：in-flight GPU/job/asset load 清理。

## 10. 开放问题 / Spike

- 插件 ABI：跨 DLL 是否传接口、POD descriptor 或 C ABI。
- Service locator 与显式依赖注入的边界。
- PIE world fork 与 Engine mode 是否共享同一装配图。
- Commandlet/Cook 是否作为 AppMode 还是独立工具进程。

## 11. 依赖与被依赖

- **依赖**：Foundation、Build System、Config、Frame Timing、Module manifests。
- **被依赖**：Game runtime、Editor、Dedicated Server、Cook tools、Tests、Shipping。


