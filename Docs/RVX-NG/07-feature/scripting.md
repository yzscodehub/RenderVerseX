# RVX-NG · Feature/Scripting 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §12 Scripting  
**里程碑**：M9  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：提供安全、热重载、可调试、可沙箱的脚本层，用于轻量 gameplay glue、工具扩展、自动化和内容驱动逻辑，同时保持核心模拟与高性能系统在 C++/数据层中。

**范围内**：脚本 VM/语言接缝、反射绑定、沙箱、热重载、模块生命周期、调试、性能预算、脚本资产 cook。  
**范围外**：Blueprint 等价可视化脚本、核心渲染/RHI 脚本化。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 安全 | 文件、网络、内存、反射 API 白名单 |
| 热重载 | 脚本重载不破坏世界状态，失败可回退 |
| 性能 | 脚本 tick 有预算，热路径不得依赖脚本 |
| 调试 | stack trace、断点、变量查看、性能统计 |
| 绑定 | 反射驱动绑定，C++ API 显式暴露 |

## 3. 公共接口面（设计契约）

```cpp
class ScriptRuntime {
    ScriptModuleHandle LoadModule(AssetId scriptAsset);
    void ReloadModule(ScriptModuleHandle module);
    ScriptObjectHandle CreateObject(TypeId scriptType, Entity owner);
    void Call(ScriptObjectHandle object, Name function, Span<ScriptValue> args);
};

class ScriptBindingRegistry {
    void RegisterType(TypeId type, ScriptExposure exposure);
    void RegisterFunction(Name name, ScriptCallable callable);
};
```

**不变量**：脚本只能访问显式暴露 API；脚本对象持有 Entity/Asset 句柄而非裸指针；脚本异常不穿透 C++ 边界；热重载失败保留旧模块。

## 4. 数据结构与内存布局

- **Module**：字节码/源码 hash、依赖、导出类型、版本。
- **ScriptObject**：VM 对象句柄、owner Entity、持久字段快照。
- **Binding Table**：反射类型、函数 trampoline、权限标记。
- **Sandbox Policy**：API 白名单、路径白名单、内存/时间预算。
- **Hot Reload State**：旧/新模块映射、字段迁移表。

## 5. 核心算法

- **加载/cook**：脚本源编译为字节码或预验证源码；记录依赖与 hash。
- **绑定**：C++ 反射元数据生成脚本可见类型，函数调用经 trampoline 校验参数。
- **热重载**：编译新模块 → 验证 → 对象字段迁移 → 原子切换；失败回滚。
- **预算**：脚本 tick 按模块/对象计时，超预算降频或暂停低优先级对象。
- **沙箱**：每次 host call 检查 capability，不允许任意 OS/文件/网络访问。

## 6. 线程与内存模型

脚本默认在 game/sim 阶段执行；单 VM 可单线程，多个 isolated VM 可并行。脚本对象状态只在帧边界迁移，跨线程调用通过命令队列。VM 内存使用独立 allocator 并计入 Memory Budget。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 脚本异常 | 捕获、记录 stack、禁用本次调用 |
| 热重载失败 | 保留旧模块，报告编译/迁移错误 |
| API 权限不足 | 拒绝调用并记录安全事件 |
| 超预算 | 降频/暂停脚本对象，避免拖垮帧 |

**禁止**：脚本直接访问 RHI/OS 任意 API；脚本异常跨越 C++；脚本持有裸 C++ 对象指针。

## 8. 性能目标

- 脚本总 tick 时间可按平台配置硬预算。
- Host call 计数与耗时可归因到模块/函数。
- 热重载不造成长时间主线程停顿。

## 9. 测试计划

- 绑定类型/函数往返、权限拒绝、异常捕获。
- 热重载字段迁移、失败回滚。
- 脚本 fuzz/恶意 API 访问测试。
- 预算超限降级测试。

## 10. 开放问题 / Spike

- Lua/LuaJIT、WASM、自定义 VM 的取舍。
- AOT 字节码 cook 与平台安全策略。
- 脚本 debugger 协议与 Editor 集成。
- 网络复制字段是否允许脚本定义。

## 11. 依赖与被依赖

- **依赖**：Reflection、Asset、Serialization、Memory、JobSystem、Security policy。
- **被依赖**：Gameplay glue、Editor tools、Automation、UI logic。


