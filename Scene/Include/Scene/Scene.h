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
 */

#include "Scene/BoundingBox.h"
#include "Scene/VertexAttribute.h"
#include "Scene/Mesh.h"
#include "Scene/Material.h"
#include "Scene/Node.h"
#include "Scene/Model.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
