---
name: RenderVerseX Modules Completion
overview: 为RenderVerseX游戏引擎的各个模块制定分阶段完善计划，按优先级顺序组织，目标是构建完整的游戏引擎。
todos:
  - id: geometry-collision
    content: "Geometry: 实现 SAT/EPA/ContactManifold 碰撞完善"
    status: pending
  - id: geometry-batch
    content: "Geometry: 实现 SIMD 批量几何查询"
    status: pending
  - id: physics-jolt
    content: "Physics: 集成 Jolt Physics 后端"
    status: pending
  - id: physics-shapes
    content: "Physics: 扩展碰撞形状 (Box/Sphere/Capsule/ConvexHull)"
    status: pending
  - id: physics-joints
    content: "Physics: 实现约束系统 (Fixed/Hinge/Slider/Spring)"
    status: pending
  - id: physics-character
    content: "Physics: 实现 CharacterController"
    status: pending
  - id: animation-data
    content: "Animation: 补全 Core 层实现 (Keyframe/Interpolation)"
    status: pending
  - id: animation-loader
    content: "Animation: 实现动画加载器集成 Resource 模块"
    status: pending
  - id: particle-emitters
    content: "Particle: 实现 7 种 Emitter (Point/Box/Sphere/Cone/Circle/Edge/Mesh)"
    status: pending
  - id: particle-modules
    content: "Particle: 实现 11 种 Module (Force/Color/Size/Velocity/Rotation/Noise/Collision/TextureSheet/Trail/Lights/SubEmitter)"
    status: pending
  - id: particle-curves
    content: "Particle: 实现 Curves 和 Events 系统"
    status: pending
  - id: editor-activate
    content: "Editor: 激活编辑器构建，集成 ImGui"
    status: pending
  - id: editor-panels
    content: "Editor: 完善 Viewport/Inspector/AssetBrowser"
    status: pending
  - id: rhi-dx11
    content: "RHI_DX11: 补充 Compute/Query/UAV 支持"
    status: pending
  - id: rhi-metal
    content: "RHI_Metal: 完善 Descriptor 绑定和平台测试"
    status: pending
  - id: networking-new
    content: "Networking: 创建网络模块 (ENet/ASIO)"
    status: pending
  - id: scripting-new
    content: "Scripting: 创建脚本模块 (Lua)"
    status: pending
  - id: ai-navigation
    content: "AI: 创建 NavMesh/PathFinding 系统"
    status: pending
  - id: ai-behaviortree
    content: "AI: 创建行为树系统"
    status: pending
isProject: false
---

# RenderVerseX 引擎模块完善计划

本计划按优先级分为三个阶段，每个模块都有独立的详细实施方案。

---

## 阶段一：高优先级模块（阻塞核心功能）

### 1. Geometry 模块 - 完善碰撞和网格算法

**当前状态**: 85% 完成（大部分为 header-only 实现）

**已完成**:

- GJK/EPA 碰撞检测 ([Geometry/Collision/GJK.h](Geometry/Include/Geometry/Collision/GJK.h))
- BSP树 CSG 操作 ([Geometry/CSG/BSPTree.h](Geometry/Include/Geometry/CSG/BSPTree.h))
- 半边网格结构 ([Geometry/Mesh/HalfEdgeMesh.h](Geometry/Include/Geometry/Mesh/HalfEdgeMesh.h))
- 曲线库 Bezier/BSpline/CatmullRom ([Geometry/Curves/](Geometry/Include/Geometry/Curves/))
- 网格细分 Loop/Catmull-Clark ([Geometry/Mesh/Subdivision.h](Geometry/Include/Geometry/Mesh/Subdivision.h))
- QEM 网格简化 ([Geometry/Mesh/Simplification.h](Geometry/Include/Geometry/Mesh/Simplification.h))

**待实现**:

1. SAT (Separating Axis Theorem) - 完善凸多面体碰撞
2. EPA 穿透深度 - 补充 [EPA.h](Geometry/Include/Geometry/Collision/EPA.h) 实现
3. ContactManifold - 碰撞接触点管理
4. Batch SIMD 操作 - 批量几何查询优化
5. 三角化算法 - Ear clipping / Delaunay

**依赖关系**:

```
Physics 模块 ──依赖──> Geometry (碰撞检测)
Picking 模块 ──依赖──> Geometry (射线/形状相交)
```

---

### 2. Physics 模块 - 完善物理模拟系统

**当前状态**: 40% 完成

**已有接口**:

- PhysicsWorld 世界管理 ([Physics/PhysicsWorld.h](Physics/Include/Physics/PhysicsWorld.h))
- RigidBody 刚体 ([Physics/RigidBody.h](Physics/Include/Physics/RigidBody.h))
- CollisionShape 基础形状 ([Physics/Shapes/CollisionShape.h](Physics/Include/Physics/Shapes/CollisionShape.h))
- 射线查询

**待实现**:

阶段 2a - 核心物理:

1. 集成 Jolt Physics 后端（已在 CMake 中注释）
2. 碰撞形状扩展 - Box, Sphere, Capsule, ConvexHull, TriangleMesh
3. 碰撞响应 - 分离/反弹/摩擦

阶段 2b - 约束系统:

1. FixedJoint - 固定约束
2. HingeJoint - 铰链约束
3. SliderJoint - 滑动约束
4. SpringJoint - 弹簧约束
5. DistanceJoint - 距离约束

阶段 2c - 高级功能:

1. CharacterController - 角色控制器
2. 连续碰撞检测 (CCD)
3. 物理材质系统
4. 触发器体积

**vcpkg 依赖**: 添加 `jolt-physics`

---

### 3. Animation 模块 - 完善动画数据层

**当前状态**: 75% 完成

**已完成**:

- Runtime: AnimationPlayer, Evaluator, SkeletonPose ([Animation/Private/Runtime/](Animation/Private/Runtime/))
- Blend: BlendTree, Blend1D, Blend2D ([Animation/Private/Blend/](Animation/Private/Blend/))
- State: StateMachine, Transitions ([Animation/Private/State/](Animation/Private/State/))
- IK: TwoBoneIK, FABRIK ([Animation/Private/IK/](Animation/Private/IK/))
- Data 结构体 (header-only): AnimationClip, Skeleton, Track

**待实现**:

阶段 3a - 数据层实现:

1. Core/Types.cpp - 时间/插值工具函数
2. Core/Keyframe.cpp - 关键帧序列化/压缩
3. Core/Interpolation.cpp - 高级插值（Hermite, Cubic）

阶段 3b - 加载器:

1. AnimationLoader - glTF/FBX 动画导入
2. AnimationImporter - 与 Resource 模块集成
3. SkeletonBuilder - 骨骼构建工具

阶段 3c - 高级功能:

1. 动画事件系统
2. Root Motion 支持
3. 动画压缩

---

## 阶段二：中优先级模块（增强功能）

### 4. Particle 模块 - 补全 Emitters 和 Modules

**当前状态**: 55% 完成

**已实现**:

- 核心系统: ParticleSystem, Instance, Pool, Subsystem
- GPU 模拟器: GPUParticleSimulator, CPUParticleSimulator, ParticleSorter
- 渲染: ParticleRenderer, TrailRenderer, ParticlePass
- Shader 完整

**待实现**:

阶段 4a - Emitters 实现 (各约 50 行):

```
Private/Emitters/
├── PointEmitter.cpp
├── BoxEmitter.cpp
├── SphereEmitter.cpp
├── ConeEmitter.cpp
├── CircleEmitter.cpp
├── EdgeEmitter.cpp
└── MeshEmitter.cpp
```

阶段 4b - Modules 实现 (各约 80 行):

```
Private/Modules/
├── ForceModule.cpp          # 力场(重力/风/吸引)
├── ColorOverLifetimeModule.cpp
├── SizeOverLifetimeModule.cpp
├── VelocityOverLifetimeModule.cpp
├── RotationOverLifetimeModule.cpp
├── NoiseModule.cpp          # Perlin/Simplex 噪声
├── CollisionModule.cpp      # 依赖 Physics
├── TextureSheetModule.cpp
├── TrailModule.cpp
├── LightsModule.cpp
└── SubEmitterModule.cpp
```

阶段 4c - 工具:

```
Private/Curves/
├── AnimationCurve.cpp
└── GradientCurve.cpp

Private/Events/
├── ParticleEvent.cpp
└── ParticleEventHandler.cpp
```

---

### 5. Editor 模块 - 激活编辑器

**当前状态**: 50% 完成（未激活）

**现有面板**:

- SceneHierarchy, Inspector, AssetBrowser, Viewport, Console, ParticleEditor

**待实施**:

阶段 5a - 基础激活:

1. 取消 [Editor/CMakeLists.txt](Editor/CMakeLists.txt) 中的编辑器可执行文件注释
2. 集成 Dear ImGui (添加到 vcpkg.json)
3. 实现 Main.cpp 入口点

阶段 5b - 面板完善:

1. Viewport 渲染循环
2. Inspector 属性编辑（反射系统）
3. SceneHierarchy 拖放
4. AssetBrowser 资产预览

阶段 5c - 专业工具:

1. ParticleEditor 粒子编辑器完善
2. AnimationEditor 动画预览
3. MaterialEditor 材质编辑器

---

### 6. RHI 后端完善

**DX11 后端** (当前 70%):

- 补充 Compute Shader 支持
- 补充 Query 系统完善
- 补充 UAV 绑定

**Metal 后端** (当前 60%):

- 完善 Descriptor 绑定
- 测试 macOS/iOS 平台
- 补充 Tessellation 支持

**OpenGL 后端** (当前 75%):

- 补充 SSBO 支持
- 完善 Framebuffer 管理

---

## 阶段三：低优先级模块（扩展功能）

### 7. Networking 模块 - 新建

**架构设计**:

```
Networking/
├── Include/Networking/
│   ├── Networking.h
│   ├── NetworkManager.h
│   ├── Connection.h
│   ├── Packet.h
│   ├── Serialization/
│   │   ├── BitStream.h
│   │   └── NetworkSerializer.h
│   ├── Replication/
│   │   ├── ReplicatedObject.h
│   │   └── PropertyReplication.h
│   └── Transport/
│       ├── ITransport.h
│       ├── UDPTransport.h
│       └── ReliableUDP.h
└── Private/
    └── (实现文件)
```

**vcpkg 依赖**: `enet` 或 `asio`

---

### 8. Scripting 模块 - 新建

**方案选择**:

- Lua (轻量, luabridge3) 
- C# (Mono/CoreCLR)

**基础架构**:

```
Scripting/
├── Include/Scripting/
│   ├── ScriptEngine.h
│   ├── ScriptComponent.h
│   ├── Bindings/
│   │   ├── CoreBindings.h
│   │   ├── MathBindings.h
│   │   └── SceneBindings.h
│   └── LuaState.h
└── Private/
    └── (实现文件)
```

---

### 9. AI 模块 - 新建

**核心功能**:

```
AI/
├── Include/AI/
│   ├── AI.h
│   ├── Navigation/
│   │   ├── NavMesh.h
│   │   ├── NavMeshBuilder.h
│   │   ├── PathFinder.h
│   │   └── NavigationAgent.h
│   ├── BehaviorTree/
│   │   ├── BehaviorTree.h
│   │   ├── BTNode.h
│   │   ├── BTTask.h
│   │   ├── BTDecorator.h
│   │   └── BTService.h
│   └── Perception/
│       ├── AIPerception.h
│       ├── SightSense.h
│       └── HearingSense.h
└── Private/
```

**vcpkg 依赖**: `recastnavigation`

---

## 模块依赖图

```mermaid

graph TD

subgraph Phase1 [Phase 1 - High Priority]

Geometry[Geometry]

Physics[Physics]

Animation[Animation]

end

subgraph Phase2 [Phase 2 - Medium Priority]

Particle[Particle]

Editor[Editor]

RHIBackends[RHI Backends]

end

subgraph Phase3 [Phase 3 - Low Priority]

Networking[Networking]

Scripting[Scripting]

AI[AI]

end

Geometry --> Physics

Physics --> Particle

Physics --> AI

Animation --> Editor

Particle --> Editor

Geometry --> AI

Core --> Geometry

Core --> Physics

Core --> Animation

RHI --> RHIBackends

Scene --> Editor

Render --> Editor