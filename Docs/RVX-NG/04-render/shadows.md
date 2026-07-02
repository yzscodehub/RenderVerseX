# RVX-NG · Render/Shadows 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §9 Shadows、决策 D6
**里程碑**：M5
**状态**：详细设计 · v1.0 可评审
**地位**：**虚拟阴影图（VSM, clipmap 缓存）** 为主 + RT 阴影；弃传统 CSM。

---

## 1. 目标与范围

**目标**：高分辨率、缓存式的虚拟阴影图（按需分页、复用静态页）+ 硬件 RT 阴影（高端/接触阴影），产出统一阴影掩码供延迟着色消费。

**范围内**：VSM 页表/分页/缓存失效、按页渲染（复用 GPU-driven 剔除）、RT 阴影、阴影掩码合成、PCF/接触硬化。
**范围外**：延迟着色（Lighting 消费掩码）、RT 原语（RayTracing）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 分辨率 | 虚拟超高分辨率，物理按需分页（开放世界远近统一） |
| 缓存 | 静态几何页跨帧复用，仅脏页重渲（性能命脉） |
| 多光 | 方向光 clipmap + 局部光按需页 |
| RT | 硬件可用时 RT 阴影（精确 + 接触阴影），否则 VSM PCF |
| 掩码 | 统一 shadow mask（含合成模式）供延迟着色 |

## 3. 公共接口面（设计契约）

```cpp
class VirtualShadowMap {
    void RecordMarkPages(RgBuilder&, RgTexture depth, View light, BufferHandle& pageRequests);
    void RecordAllocatePages(RgBuilder&, BufferHandle pageRequests); // 分配/复用物理页
    void RecordRenderPages(RgBuilder&, const CullOutputs& shadowCull, BufferHandle dirtyPages);
    void RecordSample(RgBuilder&, RgTexture depth, View, RgTexture& shadowMask);
};

class RtShadows {                     // caps.rayTracing 守门
    void Record(RgBuilder&, ASHandle tlas, RgTexture depth, View light, RgTexture& mask);
};

class ShadowComposite {               // VSM/RT 掩码合成（replace/min）
    void Record(RgBuilder&, RgTexture vsmMask, RgTexture rtMask, RgTexture& finalMask);
};
```

**不变量**：静态几何阴影页**缓存复用**，仅脏页（动态物体/光移动）重渲；RT 不可用时纯走 VSM（不黑/不漏）；阴影掩码语义统一（0=阴影,1=亮）。

## 4. 数据结构与内存布局

- **VSM 页表**：虚拟页 → 物理页 atlas 的间接表（稀疏，GPU 驻留）。
- **物理页 atlas**：固定大小物理阴影 atlas，LRU/缓存复用。
- **页请求/脏标记**：本帧需要的页 + 因动态变化失效的页。
- **clipmap**：方向光按距离的多级 clipmap（近密远疏）。
- **阴影掩码**：屏幕空间 R8（或打包多光）。

## 5. 核心算法

- **标记页**：从主视图 depth 反投影 → 计算每像素需要的 VSM 页 → 去重得页请求集。
- **分配/缓存**：页请求 → 命中缓存复用；未命中分配物理页；动态几何/光移动 → 失效相关页。
- **渲染脏页**：复用 GPU-driven 剔除（对阴影视锥）→ 仅向脏页 indirect 绘制 depth。
- **采样**：屏幕空间按页表查 VSM → PCF/Poisson 过滤 → 阴影掩码。
- **RT 阴影**：每像素向光投 shadow ray（TLAS）→ 二值/软（penumbra）→ 掩码；接触阴影补 VSM 偏差。
- **合成**：VSM + RT 按模式合成（如近景 RT、远景 VSM）。

## 6. 线程与内存模型

- 全 GPU；页标记/分配/采样 compute，页渲染 indirect draw。
- 物理 atlas 持久驻留；页表跨帧维护（缓存）。
- 脏页渲染可走 async compute 与主 pass 重叠。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 物理页耗尽 | LRU 逐出最久未用页 + 降分配（远景降级） |
| RT 不支持 | 纯 VSM PCF（caps 守门） |
| 缓存失效风暴（大量动态） | 限脏页预算，超额下帧续渲（渐进） |
| peter-panning/acne | 法线/接收者 bias + 接触硬化 |

**禁止**：每帧全量重渲所有阴影页（丢缓存红利）。

## 8. 性能目标

- 静态场景：阴影页**缓存命中率高**，仅脏页重渲，阴影成本远低于全 CSM 重渲。
- VSM 采样 < 0.5 ms；RT 阴影按分辨率/spp 预算。
- 开放世界远近统一高分辨率，无 CSM 级联接缝。

## 9. 测试计划

- **正确性**：阴影掩码 golden、缓存复用正确（脏页集精确）、clipmap 过渡无缝。
- **缓存**：静态场景帧间仅脏页重渲（计数门禁）。
- **降级**：RT off 走 VSM 一致；物理页耗尽优雅降级。
- **RT 阴影**：penumbra/接触阴影 golden（DX12/Vulkan 对等）。

## 10. 开放问题 / Spike

- VSM 页失效粒度与脏判定（动态物体包围 vs 精确）。
- RT/VSM 合成的近远切换策略。
- 局部光阴影的页预算分配。
- 半透明阴影（与 Transparency 协调）。

## 11. 依赖与被依赖

- **依赖**：Geometry&Culling（阴影视锥剔除复用）、RayTracing（RT 阴影）、GpuScene、RenderGraph、RHI。
- **被依赖**：Lighting（消费阴影掩码）、FrameRenderer。


