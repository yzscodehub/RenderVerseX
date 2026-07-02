# RVX-NG · ShaderCompiler 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §8
**里程碑**：M2（PSO 异步预编译于 M3）
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：单源 HLSL → 跨后端字节码 + 反射 + permutation 管理 + **异步 PSO 预编译**（杜绝运行时着色器编译 hitch，AAA 必修）。

**范围内**：HLSL→DXC→SPIR-V→跨编译(MSL/GLSL)、反射元数据、permutation/variant、两级缓存、热重载、PSO 异步预编译与缓存、bindless 绑定模型。
**范围外**：着色器内容/算法（各渲染 pass 的 .hlsl）、RHI PSO 对象创建（RHI，本系统对接）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 单源 | 一份 HLSL 跨 D3D12(DXIL)/Vulkan(SPIR-V)/Metal(MSL) |
| 反射 | 资源绑定/常量布局元数据，喂 bindless 与材质表 |
| permutation | 特性开关组合可枚举/按需编译，避免爆炸 |
| 无 hitch | 运行时不同步编译；PSO 预编译 + 缓存 |
| 缓存 | 离线（发行预烘焙全 permutation）+ 运行时（内容寻址） |
| 热重载 | 源/include 变更增量重编 + 运行时替换 |

## 3. 公共接口面（设计契约）

```cpp
struct ShaderKey { Name source; Name entry; ShaderStage stage; PermutationMask perm; };

class ShaderCompiler {
    // 异步编译；返回 future/handle，完成经 JobSystem 回调
    ShaderHandle Request(const ShaderKey&);            // 命中缓存即同步返回
    bool         IsReady(ShaderHandle);
    Bytecode     Get(ShaderHandle, BackendTarget);     // DXIL/SPIRV/MSL
    const ShaderReflection& Reflect(ShaderHandle);

    void OnSourceChanged(Name source);                 // 热重载：失效 + 增量重编
};

// PSO 预编译（M3，对接 RHI）：避免运行时 hitch
class PsoCache {
    void   Precompile(const GfxPipelineDesc&);         // 后台 JobSystem 编译
    bool   IsReady(PsoKey);
    PipelineHandle GetOrStall(PsoKey);                 // 就绪→返回；否则用 fallback 占位
    void   LoadDisk(path);  void SaveDisk(path);       // 持久化 PSO 缓存
};

struct ShaderReflection {                              // 喂 bindless/材质
    Span<BindingInfo> bindings;  ConstantLayout constants;  ThreadGroupSize tgSize;
};
```

**不变量**：`Request` 缓存命中**同步**返回，未命中**异步**（绝不同步阻塞渲染）；`GetOrStall` 永不同步编译 PSO——未就绪用 fallback PSO（占位），后台编译完成后下帧替换；反射与字节码一致（同次编译产出）。

## 4. 编译管线与数据布局

- **前端**：DXC 编译 HLSL → DXIL（D3D12）与 SPIR-V；**SPIRV-Cross** SPIR-V→MSL（Metal）/ GLSL（如需）。
- **反射**：从 DXIL/SPIR-V 反射（spirv-reflect）→ 统一 `ShaderReflection`。
- **permutation**：特性 = 编译期 `#define` 维度；`PermutationMask` 位集；按需编译 + 缓存，发行期全枚举预烘焙。
- **缓存键**：内容寻址 = hash(源 + include 闭包 + entry + perm + target + 编译器版本)。两级：磁盘（持久）+ 内存（LRU）。
- **PSO 缓存**：`PsoKey` = hash(shader bytecodes + render state)；磁盘持久化（driver PSO blob + 自管）。

## 5. 核心算法

- **异步编译**：`Request` 未命中 → 入 JobSystem 编译 job（DXC 调用，可重；多 permutation 并行）→ 完成填缓存 + 回调。
- **include 失效**：维护 source→include 反向依赖图；`OnSourceChanged` 沿图失效所有受影响 ShaderKey。
- **PSO 预编译**：场景/材质加载时 `Precompile` 所需 PSO 集 → 后台编译队列；运行时 `GetOrStall` 命中即用，未命中走 fallback（防 hitch）。
- **发行预烘焙**：Shipping Cook（§09）遍历全 permutation + PSO 集离线编译入包。

## 6. 线程与内存模型

- 编译在 JobSystem worker（DXC/SPIRV-Cross 可并行多 permutation）。
- 缓存内存池化；字节码 mmap（磁盘缓存）。
- 渲染线程只读缓存（命中同步），不在渲染热路径触发编译。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 编译错误 | 报错（含 file:line）；渲染用 error/fallback 着色器（紫色等可视标记） |
| 缓存未命中且运行时需要 | 异步编译 + fallback 占位（不阻塞、不 hitch） |
| profile/backend 不一致 | 编译期校验（如用了后端不支持的特性）→ 报错 |
| 热重载编译失败 | 保留旧版本运行，报错，不崩溃 |

**禁止**：渲染热路径同步编译着色器/PSO（hitch）；缓存失效逻辑漏 include 闭包。

## 8. 性能目标

- 缓存命中 `Request` 同步 O(1)。
- PSO 预编译并行吞吐随 worker 扩展；运行时 `GetOrStall` 零编译。
- 发行包：100% PSO 预缓存（验收：运行时编译计数 = 0）。
- 增量热重载：仅重编受影响 permutation。

## 9. 测试计划

- **单元**：permutation 编译正确、反射准确、include 失效闭包完整、缓存键稳定。
- **跨后端**：同源产出 DXIL/SPIR-V/MSL 且反射一致。
- **无 hitch 门禁**：运行时着色器/PSO 同步编译计数 == 0（性能回归门禁）。
- **热重载**：源变更后运行时着色器更新、编译失败保留旧版。

## 10. 开放问题 / Spike

- SPIRV-Cross → MSL 的边界 case（bindless argument buffer 映射）。
- permutation 爆炸控制策略（über-shader vs 静态分支 vs 动态分支）。
- PSO 缓存的 driver blob 跨驱动版本失效处理。
- C++26/新 DXC 特性跟进。

## 11. 依赖与被依赖

- **依赖**：RHI（PSO 对象创建、能力位）、Foundation（Jobs/Memory/Hash/VFS）、Asset（着色器源/缓存存储）、构建系统（发行预烘焙）。
- **被依赖**：所有渲染 pass（取字节码/PSO）、材质（反射喂材质表）、FrameRenderer。


