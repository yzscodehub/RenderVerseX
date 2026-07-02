# RVX-NG · 全局依赖 DAG

**日期**：2026-06-26  
**状态**：北极星依赖模型 · v1.0 冻结

本文定义 RVX-NG 的目标模块依赖方向。它用于约束公共头、构建图、运行时初始化顺序和测试分层。v1.0 冻结的是依赖方向和跨层边界，不冻结具体库、类名或进程拆分。

## 1. 总原则

- 下层不认识上层。
- Feature 不直接依赖 RHI。
- Render 不引用 gameplay 类型。
- Asset 可被 World/Render/Feature 消费，但 Asset runtime 不反向调用业务系统。
- Tools 可以装配 Runtime 模块，但 Runtime 不依赖 Tools。
- Shipping/Cook 可以消费构建与资产元数据，但运行时不依赖编辑器。

## 2. 主依赖图

```text
Foundation
  ├─ Core Utilities
  ├─ Memory
  ├─ Jobs
  ├─ Reflection
  ├─ Platform/VFS
  ├─ Input
  └─ Time/Config/Log

Foundation
  ├─ Data Layer
  │   ├─ World
  │   │   └─ Feature
  │   │       └─ App
  │   │           └─ Tools
  │   ├─ Render Extract Boundary
  │   └─ Networking / Replay
  ├─ RHI
  │   ├─ ShaderCompiler
  │   └─ Render
  │       ├─ FrameRenderer
  │       └─ Debug/Profiler inputs
  ├─ Asset
  │   ├─ Render
  │   ├─ World
  │   ├─ Feature
  │   └─ Tools
  └─ Build/Cook
      ├─ Project/Package/Plugin
      ├─ Compliance
      └─ Shipping
```

## 3. 模块边界规则

| 边界 | 允许 | 禁止 |
|---|---|---|
| Feature → Render | 声明组件、参数、VFX/audio/UI 数据 | 直接调用 RHI command/context |
| Render → World | 只读 Extract 快照 | 读取 gameplay 对象、写 World |
| RHI → Render | RHI 提供资源/队列/管线原语 | RHI 知道 pass、material、scene |
| Asset → Render | Asset 产 cooked GPU-ready 数据 | Asset 直接驱动渲染 pass |
| Tools → Runtime | Tools 装配 runtime、编辑真实数据 | Runtime 依赖 Editor UI |
| Shipping → Build/Cook | 消费 cook/package manifest | Shipping 依赖源资产 |
| Project/Package → Build/Cook | 提供 manifest、lock、plugin descriptor | 运行时执行包管理器 |
| Online Services → Platform | 使用平台 provider 私有实现 | gameplay 直接调用平台 SDK |
| Compliance → Build/Shipping | 扫描 third-party closure、SBOM、license | Runtime 热路径依赖合规工具 |

## 4. 初始化顺序

```text
Platform
Log/Config
Memory
Jobs
Reflection/Type Registry
VFS
Asset Registry
RHI
ShaderCompiler / PSO Cache
Data Layer / World
Render
Feature Systems
Tools or App Shell
Shipping services / Telemetry hooks
```

Shutdown 逆序进行。缺依赖、初始化环、跨层访问在 Development/Test profile 中 fail-fast。

## 5. 公共头约束

- Public headers 只能包含 public dependency。
- 第三方类型不得泄露到跨模块 public API，除非该第三方库被定义为公开 ABI 依赖。
- pImpl 用于隔离平台 SDK、音频中间件、物理中间件、脚本 VM、主机 SDK。
- 句柄和 POD descriptor 是跨模块 API 的默认载体。

## 6. 测试约束

- Foundation 测试不得启动 RHI。
- RHI 测试不得启动 Render。
- RenderGraph 测试可用 mock/headless RHI。
- Feature 测试使用 TestWorldFixture，不启动 Editor。
- Tools 测试可以装配 runtime，但必须显式声明依赖。
- Shipping 测试使用 cooked/package fixture，不读取 source asset。

## 7. 已冻结的边界决策

| 决策 | v1.0 规则 |
|---|---|
| 插件 ABI | 跨 DLL/动态库边界只允许 C ABI、POD descriptor、opaque handle 或版本化接口；禁止 STL、第三方 SDK 类型和 allocator ownership 跨 ABI 外泄 |
| ShaderCompiler 归属 | ShaderCompiler 是 Build/Cook 与 Development runtime 共用的工具服务；它不依赖 Render pass/material 业务类型，Shipping 只消费 cooked shader/PSO artifact |
| Networking transport | Platform 提供 socket、平台网络 API 和低层 transport primitive；Networking Feature 拥有 session、replication、prediction、rollback 和 gameplay protocol |
| Editor graph compiler | Editor graph 只产版本化 IR/source asset；Build/Cook 负责把 IR 编译成 runtime artifact；Runtime 不依赖 Editor graph UI 或编辑器模块 |

这些规则属于框架冻结项；若后续 Spike 或平台约束要求改变，必须新增 ADR。


