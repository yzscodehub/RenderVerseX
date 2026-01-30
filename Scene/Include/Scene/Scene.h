#pragma once

/**
 * @file Scene.h
 * @brief Unified header for Scene module
 * 
 * Include this file to access all scene management functionality:
 * - BoundingBox (AABB for spatial queries, from Spatial module)
 * - VertexAttribute (flexible vertex data storage)
 * - Mesh (geometry with submesh support)
 * - Material (PBR material system)
 * - Node (scene graph with component system)
 * - Model (container for scene hierarchy)
 * - SceneEntity (base class for spatial entities)
 * - SceneManager (central scene management)
 * - Prefab (reusable entity templates)
 * - Components (various entity components)
 */

// Core scene types
#include "Core/Math/AABB.h"
#include "Scene/VertexAttribute.h"
#include "Scene/Mesh.h"
#include "Scene/Material.h"
#include "Scene/Node.h"
#include "Scene/Model.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "Scene/Prefab.h"

// Components
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/CameraComponent.h"
#include "Scene/Components/ColliderComponent.h"
#include "Scene/Components/RigidBodyComponent.h"
#include "Scene/Components/AnimatorComponent.h"
#include "Scene/Components/SkeletonComponent.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/Components/ReflectionProbeComponent.h"
#include "Scene/Components/LightProbeComponent.h"
#include "Scene/Components/DecalComponent.h"
#include "Scene/Components/LODComponent.h"
#include "Scene/Components/AudioComponent.h"
