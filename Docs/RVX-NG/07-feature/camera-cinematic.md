# RVX-NG · Feature/Camera & Cinematic 详细设计（Tier-1）

**日期**：2026-06-27  
**层级**：Tier-1 · 派生自 FrameRenderer、Editor Sequencer、Gameplay 接缝  
**里程碑**：M7 / M9 / M10  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义游戏相机、视图栈、相机混合、抖动/后坐力、分屏、多视图、cinematic camera、过场轨道和 photo/replay camera 的 runtime 规则，使相机既能服务 gameplay，也能稳定驱动 FrameRenderer、Sequencer、Replay 和 Editor。

**范围内**：Camera Component、View Target、Camera Stack、blend、modifier、shake、spring arm、split-screen、cinematic track、photo mode、view flags。  
**范围外**：具体 gameplay 控制器、Editor 时间轴 UI、渲染 pass 实现。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 单一视图来源 | FrameRenderer 消费 CameraSystem 生成的 ViewSet，不私自查询 gameplay |
| 可组合 | gameplay camera、cinematic、debug、photo mode 经 stack 合成 |
| 可复现 | 确定性/Replay 模式下 camera input、seed、blend 时间可重放 |
| 多视图 | split-screen、reflection、probe、shadow、editor viewport 都有 ViewType |
| 可降级 | camera effect/shake/post profile 可按质量档关闭 |

## 3. 公共接口面（设计契约）

```cpp
enum class ViewType : uint8 {
    Main,
    SplitScreen,
    Editor,
    Cinematic,
    Reflection,
    Probe,
    Shadow,
    Photo
};

struct CameraState {
    Transform world;
    float fovY;
    float nearPlane;
    float farPlane;
    Mat4 view;
    Mat4 projection;
    PostProcessProfile post;
};

class CameraSystem {
public:
    CameraHandle CreateCamera(EntityId owner);
    void SetViewTarget(PlayerId player, EntityId target, BlendDesc blend);
    void AddModifier(CameraModifierHandle modifier);
    ViewSet BuildViews(const WorldSnapshot& snapshot, FrameTime time);
};
```

**不变量**：Render 只读取 ViewSet；camera modifier 不直接写渲染资源；cinematic 接管必须可恢复到 gameplay camera；Editor/debug camera 不污染 gameplay state。

## 4. 数据结构与内存布局

- **Camera Component**：lens、projection、clip、priority、post profile、flags。
- **View Target**：target entity、socket/bone、offset、tracking rules、blend state。
- **Camera Stack**：base camera、modifiers、shake、collision/spring arm、post override。
- **Cinematic Track**：keyframes、curve、lens、focus、look-at target、event markers。
- **ViewSet**：per-view CameraState、viewport rect、jitter、view flags、render profile。

## 5. 核心算法

- **Resolve Target**：按 player/camera priority 选择 view target。
- **Blend**：source/target CameraState 按 curve 混合，rotation 使用 shortest arc 或 squad。
- **Modifier Stack**：spring arm collision、camera lag、shake、recoil、FOV pulse、post override 顺序执行。
- **Cinematic Override**：Sequencer 轨道写入 camera state；结束时按 blend-out 恢复。
- **Multi-view Build**：main/split/editor/photo 产生 ViewSet；probe/reflection/shadow 由 FrameRenderer 根据主 View 派生。
- **Replay Camera**：回放模式可选择 recorded camera 或 free camera，不改变 replay state。

## 6. 线程与内存模型

CameraSystem 在 sim/update 阶段读取 WorldSnapshot 和输入，输出 ViewSet。ViewSet 是帧只读数据，交给 Extract/FrameRenderer。Camera curve 和 cinematic 数据来自 cooked asset，运行时不读取 editor source。调试 camera 可在 Editor/App 层注入，但必须通过同一接口。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| view target 无效 | fallback 到 last valid camera 或 default spawn camera |
| cinematic camera asset 缺失 | 跳过轨道并记录错误 |
| near/far/fov 非法 | clamp 到 profile 允许范围 |
| split-screen view 超预算 | 降低 view quality 或拒绝增加本地玩家 |
| replay camera 不匹配版本 | 使用 free camera fallback |

**禁止**：FrameRenderer 直接访问 gameplay actor；camera modifier 修改 World 物理状态；cinematic 接管后无法恢复 gameplay camera。

## 8. 性能目标

- 每帧 camera resolve 为 O(active cameras + modifiers)，不随场景实体总数增长。
- ViewSet 构建无堆分配，使用 frame arena。
- split-screen 成本显式进入 FrameStats 和 scalability profile。

## 9. 测试计划

- blend curve、modifier 顺序、spring arm collision、shake determinism。
- cinematic 接管/恢复、camera cut、focus/look-at。
- split-screen viewport rect、per-view post profile、FrameRenderer ViewSet contract。
- replay recorded/free camera、invalid target fallback。

## 10. 开放问题 / Spike

- Camera collision 使用 physics query 还是 spatial query fallback。
- Cinematic lens/focus 是否需要真实物理相机参数。
- Photo mode 是否允许暂停 sim 并单独 tick render effects。
- VR/XR stereo camera 是否共享本系统或在 platform profile 中扩展。

## 11. 依赖与被依赖

- **依赖**：World/Transform、Input、Physics query、Animation sockets、Sequencer、Frame Timing。
- **被依赖**：FrameRenderer、Editor viewport、Replay、Photo mode、Gameplay controllers、Cinematic tools。
