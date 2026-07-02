# RVX-NG · Feature/Animation 详细设计（Tier-1）

**日期**：2026-06-26  
**层级**：Tier-1 · 派生自总纲 §12 Animation  
**里程碑**：M9  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：提供面向角色、载具、怪物、人群与过场的动画运行时：骨骼动画、混合树、状态机、IK、retarget、root motion、动画压缩、morph/blendshape、motion matching、GPU skinning，以及与物理布料/头发/ragdoll 的协作。

**范围内**：动画资产格式、骨架/pose 数据、动画图、状态机、采样与混合、IK、retarget、root motion、同步标记、动画事件、GPU skinning、morph、动画压缩、人群 LOD。  
**范围外**：可视化动画编辑器（Tools/Editor）、物理求解器内部（Physics）、过场轨道 UI（Tools/Sequencer）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 数据导向 | 热路径以 SoA pose/clip 数据批量采样，避免 per-bone 虚调用 |
| 可组合 | 动画图节点可组合、可缓存、可调试 |
| 多角色规模 | 大量角色按 LOD 降低采样、IK、skinning 频率 |
| 物理协作 | ragdoll、布料、头发与动画有明确前后顺序和混合策略 |
| 跨平台 | 压缩格式、SIMD、GPU skinning 后端可跨 RHI |

## 3. 公共接口面（设计契约）

```cpp
struct SkeletonHandle { u32 id; u32 gen; };
struct AnimationClipHandle { u32 id; u32 gen; };

struct PoseBuffer {
    Span<Transform> localPose;
    Span<Transform> modelPose;
    Span<float> curveValues;
};

class AnimationGraph {
    void SetParameter(Name name, float value);
    void SetParameter(Name name, bool value);
    void Evaluate(const AnimationContext& ctx, PoseBuffer& outPose);
};

class AnimationSystem {
    void RegisterAnimator(Entity entity, AnimationGraphHandle graph);
    void Update(float dt);
    void BuildSkinningJobs(RenderSnapshot& snapshot);
};
```

**不变量**：动画图不直接写渲染资源；root motion 经 ECS/Authoring 边界提交；物理后处理只在定义好的 TickGroup 生效；GPU skinning 输入来自稳定的 pose/mesh 缓冲。

## 4. 数据结构与内存布局

- **Skeleton**：骨骼父索引、bind pose、retarget metadata、socket/marker 表。
- **Clip**：压缩轨道，按 bone/curve 分离；关键帧块可 mmap；支持随机访问。
- **PoseBuffer**：局部姿态、模型姿态、曲线值分离，批量 SIMD 友好。
- **AnimationGraph 实例**：参数块、节点状态、同步组状态、事件队列。
- **Skinning 数据**：骨矩阵 palette、morph target 权重、GPU skinning dispatch 参数。

## 5. 核心算法

- **采样**：clip 以块为单位解压，按轨道采样 TRS；静态轨道常量折叠。
- **混合**：blend tree、additive、masked blend、sync group 对齐步态相位。
- **状态机**：transition 条件、blend window、interrupt policy、entry/exit event。
- **IK/约束**：Two-bone IK、look-at、FABRIK/CCD 可选；高成本 IK 受 LOD 控制。
- **Retarget**：基于骨架语义映射 + 姿态空间校正。
- **Motion matching**：高端档，查询特征向量库并做短窗口 inertialization。
- **GPU skinning**：compute 生成 skinned vertex 或 meshlet skinning 数据，供 Geometry pass 消费。

## 6. 线程与内存模型

动画采样、混合、IK 作为 JobSystem batch 运行；同一实体动画图实例单线程更新，不同实体并行。PoseBuffer 从帧竞技场分配，跨帧仅保留必要历史姿态用于 motion vector、inertialization 与同步。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| clip/skeleton 不匹配 | retarget 失败则返回 bind/参考 pose，并记录错误 |
| graph 参数缺失 | 使用默认值并 WARN；严格模式下验证失败 |
| GPU skinning 不支持 | 降级 CPU skinning 或低骨骼数路径 |
| 动画事件堆积 | 帧边界限额派发，超额计数并延迟 |

**禁止**：动画图直接访问 RHI；跨帧持有帧竞技场 pose 指针；LOD 后仍运行全量 IK/cloth。

## 8. 性能目标

- 1000 个中等复杂度角色动画采样与混合在多核预算内完成。
- 角色 LOD 降档后，采样频率、IK、morph、skinning 成本按档可测下降。
- GPU skinning 支持批量 dispatch，避免 per-character 小提交。

## 9. 测试计划

- clip 采样、混合、状态机 transition、root motion、retarget golden。
- 压缩误差测试：位置/旋转/曲线误差低于资产配置阈值。
- GPU/CPU skinning 结果容差一致。
- LOD 压力测试：大量角色帧时与事件派发稳定。

## 10. 开放问题 / Spike

- 动画压缩格式：ACL 风格库接入 vs 自研轨道压缩。
- Motion matching 数据库规模、查询结构与内存预算。
- 布料/头发在动画图内还是 Physics 后处理内统一调度。
- GPU skinning 与 meshlet/virtual geometry 的数据交界。

## 11. 依赖与被依赖

- **依赖**：Asset、ECS、JobSystem、Physics、Render/GpuScene。
- **被依赖**：Gameplay、Editor、Sequencer、Physics ragdoll、Render motion vector。


