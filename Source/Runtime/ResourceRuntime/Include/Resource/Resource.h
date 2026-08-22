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
 * - Injected source-change monitoring for development composition
 * 
 * Runtime cooked formats:
 * - Models: .rva Model Artifact V3 plus contained mesh/material products
 * - Materials: .rvmat CookedMaterial Artifact V1
 * - Textures: .rva cooked texture products
 * - Audio: cooked audio products
 *
 * Source formats (glTF, FBX, PNG, WAV and similar) belong to Developer
 * import/cook composition and are never a Runtime loading fallback.
 * 
 * @usage
 * @code
 * #include "Resource/Resource.h"
 * 
 * // Initialize via subsystem
 * auto& resourceSys = engine->GetSubsystem<ResourceSubsystem>();
 * 
 * // Load resources
 * auto model = resourceSys.Load<ModelResource>("models/player.rva");
 * auto material = resourceSys.Load<MaterialResource>("materials/player.rvmat");
 * auto texture = resourceSys.Load<TextureResource>("textures/diffuse.rva");
 * auto audio = resourceSys.Load<AudioResource>("audio/music.rva");
 * 
 * // Or use ResourceManager directly
 * resourceSubsystem.Load<MeshResource>("models/enemy.rva");
 * 
 * // Development hosts inject source loaders and a change monitor through
 * // ResourceManagerDependencies. ResourceManager owns reload transactions.
 * @endcode
 */

#include "Resource/IResource.h"
#include "Resource/AssetResidencyLease.h"
#include "Resource/PreparedResourceBundle.h"
#include "Resource/ResourceDiagnostics.h"
#include "Resource/ResourceDiagnosticsView.h"
#include "Resource/ResourceContentIdentity.h"
#include "Resource/ResourceLoadOperation.h"
#include "Resource/ResourceHandle.h"
#include "Resource/ResourcePublicationView.h"
#include "Resource/ResourceCache.h"
#include "Resource/ResourceRegistry.h"
#include "Resource/DependencyGraph.h"
#include "Resource/RuntimeResourcePolicy.h"
#include "Resource/ResourceManager.h"
#include "Resource/ResourceSubsystem.h"

// Concrete resource types
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/TextureResource.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MaterialInstanceResource.h"
#include "Resource/Types/AudioResource.h"
#include "Resource/Types/AnimationResource.h"
#include "Resource/Types/EnvironmentResource.h"

// Loaders
#include "Resource/Loader/AnimationLoader.h"
#include "Resource/Loader/CookedAudioLoader.h"
#include "Resource/Loader/CookedEnvironmentLoader.h"
#include "Resource/Loader/CookedMaterialLoader.h"
#include "Resource/Loader/CookedModelLoader.h"
#include "Resource/Loader/CookedTextureLoader.h"
