#pragma once

/**
 * @file Resource.h
 * @brief Unified include for Resource module
 * 
 * The Resource module provides a complete resource management system:
 * - IResource: Base class for all resource types
 * - ResourceHandle<T>: Smart handle with reference counting
 * - ResourceManager: Central facade for loading/unloading
 * - ResourceCache: LRU cache with memory limits
 * - ResourceRegistry: Metadata database
 * - DependencyGraph: Dependency tracking and resolution
 * - ResourceSubsystem: Engine subsystem integration
 * 
 * @usage
 * @code
 * #include "Resource/Resource.h"
 * 
 * // Initialize via subsystem
 * auto& resourceSys = engine->GetSubsystem<ResourceSubsystem>();
 * 
 * // Load resources
 * auto mesh = resourceSys.Load<MeshResource>("models/player.gltf");
 * auto texture = resourceSys.Load<TextureResource>("textures/diffuse.png");
 * 
 * // Or use ResourceManager directly
 * ResourceManager::Get().Load<MeshResource>("models/enemy.gltf");
 * @endcode
 */

#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"
#include "Resource/ResourceCache.h"
#include "Resource/ResourceRegistry.h"
#include "Resource/DependencyGraph.h"
#include "Resource/ResourceManager.h"
#include "Resource/ResourceSubsystem.h"

// Concrete resource types
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/TextureResource.h"
#include "Resource/Types/MaterialResource.h"
