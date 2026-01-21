---
name: Math Library Refactor
overview: 将几何基元类（Ray, AABB, Sphere, Plane, Frustum）从 Spatial 模块迁移到 Core/Math，整合 Acceleration 模块到 Picking，建立清晰的模块边界。
todos:
  - id: phase1-create-math-dir
    content: "Phase 1: 创建 Core/Math 目录和几何基元头文件 (Ray.h, AABB.h, Sphere.h, Plane.h, Frustum.h, Intersection.h, Geometry.h)"
    status: completed
  - id: phase2-migrate-content
    content: "Phase 2: 迁移 Spatial/Bounds 和 Acceleration 代码到 Core/Math，统一命名空间"
    status: completed
  - id: phase3-update-spatial
    content: "Phase 3: 更新 Spatial 模块，移除 Bounds 目录，更新 Index/Query 的 include"
    status: completed
  - id: phase4-merge-acceleration
    content: "Phase 4: 将 Acceleration/BVH.h 合并到 Picking 模块，删除 Acceleration 目录"
    status: completed
  - id: phase5-update-deps
    content: "Phase 5: 更新所有依赖模块 (Scene, Asset, Tests) 的 include 路径"
    status: completed
  - id: phase6-cmake
    content: "Phase 6: 更新 CMake 配置，验证编译"
    status: completed
  - id: phase7-test
    content: "Phase 7: 运行测试验证功能完整性"
    status: completed
---

# Math Library Refactoring Plan

## 目标

按照游戏引擎最佳实践重构数学库，将几何基元类放入 Core/Math，保持 Spatial 专注于空间索引，合并 Acceleration 到 Picking。

---

## Phase 1: 创建 Core/Math 几何基元

在 `Core/Include/Core/Math/` 目录下创建以下文件：

### 1.1 基础几何类型

| 文件 | 类 | 说明 |

|------|-----|------|

| `Ray.h` | `Ray`, `RayHit` | 射线及射线命中信息 |

| `AABB.h` | `AABB` | 轴对齐包围盒 (别名 BoundingBox) |

| `Sphere.h` | `Sphere` | 包围球 |

| `Plane.h` | `Plane` | 3D 平面 |

| `Frustum.h` | `Frustum`, `FrustumPlane`, `IntersectionResult` | 视锥体 |

| `OBB.h` | `OBB` | 有向包围盒 (预留) |

### 1.2 交叉测试

| 文件 | 函数 |

|------|------|

| `Intersection.h` | `RayAABBIntersect`, `RaySphereIntersect`, `RayPlaneIntersect`, `RayTriangleIntersect`, `AABBOverlaps`, `FrustumContains` 等 |

### 1.3 聚合头文件

| 文件 | 说明 |

|------|------|

| `Geometry.h` | 包含所有几何类型的聚合头文件 |

### 目录结构

```
Core/
  Include/Core/
    Math/
      Ray.h
      AABB.h
      Sphere.h
      Plane.h
      Frustum.h
      OBB.h           # 预留
      Intersection.h
      Geometry.h      # 聚合头文件
    MathTypes.h       # Vec, Mat, Quat (已有)
  Private/
    Math/
      Intersection.cpp  # 复杂交叉测试实现
```

---

## Phase 2: 迁移代码内容

### 2.1 从 Spatial/Bounds 迁移

| 源文件 | 目标文件 | 迁移内容 |

|--------|----------|----------|

| `Spatial/Bounds/BoundingBox.h` | `Core/Math/AABB.h` | `BoundingBox` 类 -> `AABB` |

| `Spatial/Bounds/BoundingSphere.h` | `Core/Math/Sphere.h` | `BoundingSphere` 类 -> `Sphere` |

| `Spatial/Bounds/Frustum.h` | `Core/Math/Plane.h` | `Plane` 结构 |

| `Spatial/Bounds/Frustum.h` | `Core/Math/Frustum.h` | `Frustum`, `FrustumPlane`, `IntersectionResult` |

| `Spatial/Bounds/BoundsTests.h` | `Core/Math/Intersection.h` | `Overlaps`, `Contains`, `Distance` 函数 |

### 2.2 从 Acceleration 迁移

| 源文件 | 目标文件 | 迁移内容 |

|--------|----------|----------|

| `Acceleration/Ray.h` | `Core/Math/Ray.h` | `Ray`, `RayHit` 结构 |

| `Acceleration/Intersection.h` | `Core/Math/Intersection.h` | `RayAABBIntersect`, `RayTriangleIntersect`, `RaySphereIntersect`, `RayPlaneIntersect` |

### 2.3 命名空间

- 所有类型放在 `RVX` 命名空间（与 MathTypes.h 一致）
- 提供类型别名保持兼容：
  - `using BoundingBox = AABB;`
  - `using BoundingSphere = Sphere;`

---

## Phase 3: 更新 Spatial 模块

### 3.1 移除已迁移文件

删除以下文件：

- `Spatial/Include/Spatial/Bounds/BoundingBox.h`
- `Spatial/Include/Spatial/Bounds/BoundingSphere.h`
- `Spatial/Include/Spatial/Bounds/Frustum.h`
- `Spatial/Include/Spatial/Bounds/BoundsTests.h`
- `Spatial/Private/Bounds/*.cpp`

### 3.2 更新依赖

更新以下文件使用 `Core/Math/Geometry.h`：

- `Spatial/Include/Spatial/Index/ISpatialIndex.h`
- `Spatial/Include/Spatial/Index/ISpatialEntity.h`
- `Spatial/Include/Spatial/Index/BVHIndex.h`
- `Spatial/Include/Spatial/Query/QueryFilter.h`
- `Spatial/Include/Spatial/Query/SpatialQuery.h`

### 3.3 更新后的 Spatial 结构

```
Spatial/
  Include/Spatial/
    Index/
      ISpatialEntity.h
      ISpatialIndex.h
      BVHIndex.h
      SpatialFactory.h
    Query/
      QueryFilter.h
      SpatialQuery.h
    Spatial.h           # 更新聚合头
  Private/
    Index/
      BVHIndex.cpp
      SpatialFactory.cpp
```

---

## Phase 4: 合并 Acceleration 到 Picking

### 4.1 移动 BVH 实现

将 `Acceleration/BVH.h` 的内容移动到 Picking 模块内部：

```
Picking/
  Include/Picking/
    PickingSystem.h     # 公开 API
  Private/
    MeshBVH.h           # 从 Acceleration/BVH.h 提取
    MeshBVH.cpp
    SceneBVH.h          # 从 Acceleration/BVH.h 提取
    SceneBVH.cpp
    PickingSystem.cpp
```

### 4.2 删除 Acceleration 模块

删除 `Acceleration/` 目录，更新根 `CMakeLists.txt`。

---

## Phase 5: 更新所有依赖模块

### 5.1 需要更新的模块

| 模块 | 需要更新的 include |

|------|---------------------|

| Scene | `SceneEntity.h`, `SceneManager.h`, `Mesh.h`, `Model.h`, `Node.h` |

| Asset | `MeshAsset.h` |

| Picking | `PickingSystem.h` |

| Tests | `SystemIntegration/main.cpp` |

### 5.2 兼容性头文件

创建向后兼容头文件（标记 deprecated）：

- `Scene/Include/Scene/BoundingBox.h` -> 包含 `Core/Math/AABB.h`
- `Spatial/Include/Spatial/Bounds/BoundingBox.h` -> 包含 `Core/Math/AABB.h`

---

## Phase 6: 更新 CMake 配置

### 6.1 Core/CMakeLists.txt

添加新的源文件：

```cmake
target_sources(RVX_Core PRIVATE
    Private/Math/Intersection.cpp
)
```

### 6.2 Spatial/CMakeLists.txt

移除 Bounds 相关源文件：

```cmake
add_library(Spatial STATIC
    Private/Index/BVHIndex.cpp
    Private/Index/SpatialFactory.cpp
)
```

### 6.3 Picking/CMakeLists.txt

添加 BVH 实现文件，移除对 Acceleration 的依赖。

### 6.4 根 CMakeLists.txt

移除 `add_subdirectory(Acceleration)`。

---

## 文件变更摘要

| 操作 | 文件数 |

|------|--------|

| 新建 | 9 (Core/Math/*.h, Intersection.cpp) |

| 删除 | 8 (Spatial/Bounds/*, Acceleration/*) |

| 修改 | ~15 (更新 include) |

| 兼容头 | 2 (deprecated wrappers) |

---

## 验证步骤

1. 完成每个 Phase 后运行 CMake configure
2. Phase 1-2 完成后编译 Core 模块
3. Phase 3 完成后编译 Spatial 模块
4. Phase 4 完成后编译 Picking 模块
5. Phase 5 完成后编译所有模块
6. 运行 SystemIntegrationTest 验证功能