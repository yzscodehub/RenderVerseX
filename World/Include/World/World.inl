#pragma once

/**
 * @file World.inl
 * @brief World module unified header
 * 
 * Includes all World module headers for convenience.
 */

// Core World
#include "World/World.h"
#include "World/SpatialSubsystem.h"
#include "World/PickingService.h"

// Scene (forwarded from Scene module)
#include "Scene/Scene.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneEntity.h"
#include "Scene/Node.h"
#include "Scene/Mesh.h"
#include "Scene/Material.h"
#include "Scene/Model.h"

// Spatial (forwarded from Spatial module)
#include "Spatial/Spatial.h"
