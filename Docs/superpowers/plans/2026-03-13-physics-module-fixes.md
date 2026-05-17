# Physics 模块优化实施计划

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 完成 Physics 模块的核心功能，使物理模拟真正可用

**Architecture:** 基于现有的 PhysicsWorld API 接口，完善 Built-in 后端实现（不依赖 Jolt），提供完整的碰撞检测和物理模拟

**Tech Stack:** C++20, 内部碰撞检测算法 (Built-in backend)

---

## 问题分析

当前 Physics 模块的问题：
1. JoltBackend 是空壳，所有方法返回 nullptr/false
2. PhysicsWorld::CreateBody 未创建后端 body
3. 碰撞查询 (Raycast, SphereCast, OverlapSphere) 返回空
4. RigidBodyComponent 生命周期与 PhysicsWorld 脱节
5. Built-in 后端使用 O(n²) 暴力检测

---

## Chunk 1: 完善 PhysicsWorld 核心功能

### 任务 1.1: 实现 PhysicsWorld 模拟循环

**Files:**
- Modify: `Physics/Private/PhysicsWorld.cpp`

- [x] **Step 1: 读取现有 PhysicsWorld.cpp 实现**

读取 `Physics/Private/PhysicsWorld.cpp` 第 1-100 行

- [x] **Step 2: 实现 StepInternal 方法**

```cpp
void PhysicsWorld::StepInternal(float dt)
{
    // 1. 更新所有动态 body's 位置
    for (auto& body : m_bodies)
    {
        if (!body || body->GetType() != BodyType::Dynamic)
            continue;

        // 应用重力
        Vec3 velocity = body->GetLinearVelocity();
        velocity += m_config.gravity * dt;
        body->SetLinearVelocity(velocity);

        // 更新位置
        Vec3 position = body->GetPosition();
        position += velocity * dt;
        body->SetPosition(position);
    }

    // 2. 碰撞检测 (简化实现)
    DetectCollisions();

    // 3. 解决碰撞
    ResolveCollisions();

    // 4. 更新约束
    for (auto& constraint : m_constraints)
    {
        if (constraint)
            SolveConstraint(constraint.get(), dt);
    }
}
```

- [ ] **Step 3: 实现碰撞检测 DetectCollisions**

```cpp
void PhysicsWorld::DetectCollisions()
{
    // 简化的 AABB 检测
    for (size_t i = 0; i < m_bodies.size(); ++i)
    {
        if (!m_bodies[i]) continue;

        for (size_t j = i + 1; j < m_bodies.size(); ++j)
        {
            if (!m_bodies[j]) continue;

            if (CheckAABBOverlap(m_bodies[i].get(), m_bodies[j].get()))
            {
                // 记录碰撞对
                m_collisionPairs.push_back({i, j});
            }
        }
    }
}
```

- [ ] **Step 4: 提交**

```bash
git add Physics/Private/PhysicsWorld.cpp
git commit -m "feat: implement PhysicsWorld simulation loop

- Add StepInternal method with gravity integration
- Add basic collision detection via AABB overlap
- Add collision resolution placeholder

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### 任务 1.2: 实现碰撞查询功能

**Files:**
- Modify: `Physics/Private/PhysicsWorld.cpp`

- [x] **Step 1: 实现 Raycast**

```cpp
bool PhysicsWorld::Raycast(const Vec3& origin, const Vec3& direction,
                          float maxDistance, RaycastHit& hit, uint32 layerMask) const
{
    float closestDist = maxDistance;
    bool foundHit = false;

    for (auto& body : m_bodies)
    {
        if (!body) continue;

        // 简化的射线-AABB 检测
        AABB aabb = body->GetAABB();
        float t = 0.0f;

        if (RayAABBIntersect(origin, direction, aabb, t))
        {
            if (t < closestDist)
            {
                closestDist = t;
                foundHit = true;
                hit.body = body.get();
                hit.distance = t;
                hit.hitPoint = origin + direction * t;
                // 计算法线 (简化)
                hit.hitNormal = Vec3(0, 1, 0);
            }
        }
    }

    return foundHit;
}
```

- [x] **Step 2: 实现 SphereCast**

```cpp
bool PhysicsWorld::SphereCast(const Vec3& origin, float radius,
                            const Vec3& direction, float maxDistance,
                            ShapeCastHit& hit, uint32 layerMask) const
{
    // 类似 Raycast，使用膨胀的 AABB
    float closestDist = maxDistance;
    bool foundHit = false;

    for (auto& body : m_bodies)
    {
        if (!body) continue;

        AABB aabb = body->GetAABB();
        // 膨胀 AABB
        aabb.min -= Vec3(radius);
        aabb.max += Vec3(radius);

        float t = 0.0f;
        if (RayAABBIntersect(origin, direction, aabb, t))
        {
            if (t < closestDist)
            {
                closestDist = t;
                foundHit = true;
                hit.body = body.get();
                hit.distance = t;
            }
        }
    }

    return foundHit;
}
```

- [x] **Step 3: 实现 OverlapSphere**

```cpp
size_t PhysicsWorld::OverlapSphere(const Vec3& center, float radius,
                                  std::vector<BodyHandle>& bodies, uint32 layerMask) const
{
    size_t count = 0;

    for (size_t i = 0; i < m_bodies.size(); ++i)
    {
        if (!m_bodies[i]) continue;

        Vec3 bodyPos = m_bodies[i]->GetPosition();
        float dist = glm::length(bodyPos - center);

        if (dist < radius)
        {
            // 计算 body handle
            BodyHandle handle(m_bodyLookup.begin()->first + i);
            bodies.push_back(handle);
            count++;
        }
    }

    return count;
}
```

- [ ] **Step 4: 提交**

```bash
git add Physics/Private/PhysicsWorld.cpp
git commit -m "feat: implement collision queries

- Add Raycast with AABB intersection
- Add SphereCast with expanded AABB
- Add OverlapSphere with distance check

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Chunk 2: 完善 RigidBody 和 AABB

### 任务 2.1: 实现 RigidBody 的 AABB 计算

**Files:**
- Modify: `Physics/Include/Physics/RigidBody.h`
- Modify: `Physics/Private/RigidBody.cpp`

- [x] **Step 1: 读取 RigidBody 实现**

- [x] **Step 2: 添加 AABB 方法**

```cpp
// RigidBody.h 添加
AABB GetAABB() const;
void UpdateAABB();

// RigidBody.cpp 实现
AABB RigidBody::GetAABB() const
{
    if (m_shapes.empty())
    {
        // 无形状，返回位置点的 AABB
        Vec3 pos = GetPosition();
        return AABB{pos - Vec3(0.5f), pos + Vec3(0.5f)};
    }

    // 计算所有形状的组合 AABB
    AABB result;
    result.min = Vec3(std::numeric_limits<float>::max());
    result.max = Vec3(std::numeric_limits<float>::lowest());

    for (auto& shape : m_shapes)
    {
        AABB shapeAABB = shape->GetAABB();
        result.min = glm::min(result.min, shapeAABB.min);
        result.max = glm::max(result.max, shapeAABB.max);
    }

    // 变换到世界坐标
    Mat4 worldMat = GetWorldMatrix();
    result.min = Vec3(worldMat * Vec4(result.min, 1.0f));
    result.max = Vec3(worldMat * Vec4(result.max, 1.0f));

    return result;
}
```

- [ ] **Step 3: 实现 CollisionShape 的 GetAABB**

在 `Shapes/CollisionShape.cpp` 中为各形状实现 AABB 计算

- [ ] **Step 4: 提交**

```bash
git add Physics/Include/Physics/RigidBody.h Physics/Private/RigidBody.cpp Physics/Private/Shapes/CollisionShape.cpp
git commit -m "feat: implement RigidBody AABB calculation

- Add GetAABB method to compute world-space AABB
- Implement AABB for sphere, box, capsule shapes
- Use AABB in collision queries

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Chunk 3: 实现 AABB 射线检测算法

### 任务 3.1: 在 Geometry 模块实现 Ray-AABB 检测

**Files:**
- Modify: `Geometry/Include/Geometry/Intersection.h`
- Modify: `Geometry/Private/Intersection.cpp` (如果存在)

- [ ] **Step 1: 添加 Ray-AABB 检测函数**

```cpp
// Intersection.h 添加
bool RayAABBIntersect(const Vec3& rayOrigin, const Vec3& rayDir,
                     const AABB& aabb, float& t);

// Intersection.cpp 实现
bool RayAABBIntersect(const Vec3& rayOrigin, const Vec3& rayDir,
                     const AABB& aabb, float& t)
{
    Vec3 invDir = Vec3(1.0f) / rayDir;
    Vec3 t0s = (aabb.min - rayOrigin) * invDir;
    Vec3 t1s = (aabb.max - rayOrigin) * invDir;

    Vec3 tsmaller = glm::min(t0s, t1s);
    Vec3 tbigger = glm::max(t0s, t1s);

    float tmin = glm::max(glm::max(tsmaller.x, tsmaller.y), tsmaller.z);
    float tmax = glm::min(glm::min(tbigger.x, tbigger.y), tbigger.z);

    if (tmax < 0.0f) return false;  // AABB 在射线后方
    if (tmin > tmax) return false;  // 无交点

    t = tmin;
    return tmin >= 0.0f;
}
```

- [ ] **Step 2: 提交**

```bash
git add Geometry/Include/Geometry/Intersection.h Geometry/Private/Intersection.cpp
git commit -m "feat: implement Ray-AABB intersection test

- Add RayAABBIntersect function using slab method
- Enable collision queries in Physics module

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Chunk 4: 集成 RigidBodyComponent

### 任务 4.1: 连接 RigidBodyComponent 与 PhysicsWorld

**Files:**
- Modify: `Scene/Private/Components/RigidBodyComponent.cpp`

- [ ] **Step 1: 读取现有 RigidBodyComponent 实现**

- [ ] **Step 2: 添加 PhysicsWorld 引用和注册**

```cpp
void RigidBodyComponent::CreateBody()
{
    if (m_body) return;

    // 获取 PhysicsWorld
    auto* physicsWorld = GetPhysicsWorld();  // 需要实现
    if (!physicsWorld) return;

    // 创建 body
    Physics::RigidBodyDesc desc;
    desc.type = m_bodyType;
    desc.position = GetOwner()->GetPosition();
    desc.rotation = GetOwner()->GetRotation();

    m_bodyHandle = physicsWorld->CreateBody(desc);

    // 添加碰撞形状
    if (m_collider)
    {
        physicsWorld->AddShape(m_bodyHandle, m_collider->GetShape());
    }
}

void RigidBodyComponent::DestroyBody()
{
    if (!m_bodyHandle.IsValid()) return;

    auto* physicsWorld = GetPhysicsWorld();
    if (physicsWorld)
    {
        physicsWorld->DestroyBody(m_bodyHandle);
    }
    m_bodyHandle = Physics::BodyHandle::Invalid();
}

void RigidBodyComponent::OnTransformChanged()
{
    // 同步 Transform 到 Physics
    if (m_bodyHandle.IsValid())
    {
        auto* physicsWorld = GetPhysicsWorld();
        if (physicsWorld)
        {
            physicsWorld->SetBodyPosition(m_bodyHandle, GetOwner()->GetPosition());
            physicsWorld->SetBodyRotation(m_bodyHandle, GetOwner()->GetRotation());
        }
    }
}

void RigidBodyComponent::Tick(float deltaTime)
{
    // 同步 Physics 到 Transform
    if (m_bodyHandle.IsValid())
    {
        auto* physicsWorld = GetPhysicsWorld();
        if (physicsWorld && GetOwner())
        {
            Vec3 pos = physicsWorld->GetBodyPosition(m_bodyHandle);
            GetOwner()->SetPosition(pos);
        }
    }
}
```

- [ ] **Step 3: 提交**

```bash
git add Scene/Private/Components/RigidBodyComponent.cpp
git commit -m "feat: integrate RigidBodyComponent with PhysicsWorld

- Add CreateBody/DestroyBody that register with PhysicsWorld
- Sync Transform -> Physics on transform changes
- Sync Physics -> Transform on tick

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## 验证步骤

```bash
# 编译验证
cmake --build --preset debug 2>&1 | grep -E "(error|Physics)"

# 运行测试 (如果有)
./build/Debug/Tests/Debug/SystemIntegrationTest.exe
```

---

## 预期产出

1. PhysicsWorld 可以进行基本的物理模拟 (重力应用)
2. Raycast、SphereCast、OverlapSphere 碰撞查询可用
3. RigidBody 组件可以正确创建和销毁
4. Transform 和 Physics 状态可以同步

---

## 关键文件路径

| 文件 | 绝对路径 |
|------|----------|
| PhysicsWorld.h | `E:\WorkSpace\RenderVerseX\Physics\Include\Physics\PhysicsWorld.h` |
| PhysicsWorld.cpp | `E:\WorkSpace\RenderVerseX\Physics\Private\PhysicsWorld.cpp` |
| RigidBody.h | `E:\WorkSpace\RenderVerseX\Physics\Include\Physics\RigidBody.h` |
| RigidBodyComponent.cpp | `E:\WorkSpace\RenderVerseX\Scene\Private\Components\RigidBodyComponent.cpp` |
| Intersection.h | `E:\WorkSpace\RenderVerseX\Geometry\Include\Geometry\Intersection.h` |

---

*Plan created: 2026-03-13*
