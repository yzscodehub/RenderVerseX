# RVX-NG · Feature/VFX & Particle 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §12 VFX/Particle  
**里程碑**：M9  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：提供 GPU-first 的粒子与 VFX 框架：发射、模拟、事件、排序、碰撞、光照、贴花/mesh particle、ribbon/trail、VFX LOD 与预算控制。

**范围内**：CPU/GPU emitter、GPU particle simulation、indirect dispatch/draw、alive-count、排序、碰撞、VFX graph 数据模型、预算/LOD。  
**范围外**：VFX 编辑器 UI、离线流体模拟、电影级离线特效。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| GPU-first | 大规模粒子模拟、剔除、绘制走 GPU indirect |
| 预算 | 粒子数、发射率、排序、碰撞、光照按质量档控制 |
| 可组合 | VFX graph 节点声明资源和阶段 |
| 渲染集成 | 接入 RenderGraph、GpuScene、透明/Forward+、深度/运动信息 |
| 可降级 | 低档关闭碰撞/光照/排序或降低模拟频率 |

## 3. 公共接口面（设计契约）

```cpp
struct VfxEmitterComponent {
    VfxAssetHandle asset;
    u32 seed;
    VfxLod lod;
};

class VfxSystem {
    void Spawn(Entity owner, VfxAssetHandle asset);
    void Tick(float dt);
    void RecordSimulation(RgBuilder& graph, const VfxFrameInputs& inputs);
    void RecordRendering(RgBuilder& graph, const View& view);
};
```

**不变量**：GPU 粒子不回读到 CPU 驱动玩法；VFX 资源读写必须通过 RenderGraph 声明；粒子随机数按 seed 可复现；预算耗尽时降级而非无上限扩张。

## 4. 数据结构与内存布局

- **Particle Buffer**：SoA position/velocity/lifetime/color/size/custom data。
- **Alive/Dead Lists**：GPU append/consume buffer 管理生命周期。
- **Emitter State**：发射累积器、seed、bounds、LOD、event 队列。
- **Sort Keys**：透明粒子按 view-depth/material 分桶排序。
- **VFX Graph**：模拟节点、渲染节点、参数块、资源声明。

## 5. 核心算法

- **发射**：CPU 或 GPU 根据 emitter 参数写入 dead-list 分配的新粒子。
- **模拟**：compute 更新粒子，死亡粒子回收，alive count 驱动 indirect。
- **碰撞**：低档 depth collision，高档 SDF/physics proxy。
- **排序**：按透明需求进行 GPU sort 或 tile/bin sort，可按 LOD 关闭。
- **渲染**：billboard、mesh particle、ribbon、trail 走 indirect draw，接入 Forward+ 光照。
- **事件**：GPU 事件尽量留在 GPU；需要 gameplay 的少量事件通过限额 readback。

## 6. 线程与内存模型

CPU 负责 emitter 参数、资产、预算；模拟和绘制主要在 GPU。VFX pass 由 RenderGraph 调度，可与其他 compute pass async。持久粒子池按平台预算创建，避免每帧分配。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 粒子池耗尽 | 降发射率/拒绝低优先级 emitter |
| GPU sort 不支持/过贵 | 降级分桶或不排序 |
| emitter bounds 无效 | 使用保守 bounds 并 WARN |
| readback 事件过量 | 限额截断并计数 |

**禁止**：每粒子 CPU tick；未声明 RenderGraph 资源读写；无限制 GPU readback。

## 8. 性能目标

- 大规模粒子模拟与绘制由 alive-count indirect 驱动。
- VFX 预算可按场景/平台限制总粒子、排序、碰撞成本。
- 透明 VFX overdraw 有可视化与门禁。

## 9. 测试计划

- 发射/死亡/alive-list、seed reproducibility、LOD 降级。
- GPU simulation golden 与 CPU 小规模参考对比。
- 透明排序/碰撞/光照测试。
- 粒子池耗尽与预算门禁。

## 10. 开放问题 / Spike

- VFX graph 表达能力与 ShaderCompiler permutation 的关系。
- GPU sort 算法选择。
- VFX 与 Decal/Volumetric/Lighting 的跨模块资源边界。
- Niagara 风格事件系统是否纳入首期。

## 11. 依赖与被依赖

- **依赖**：RenderGraph、RHI compute/indirect、ShaderCompiler、Asset、JobSystem。
- **被依赖**：Gameplay effects、Editor VFX graph、Render transparency。


