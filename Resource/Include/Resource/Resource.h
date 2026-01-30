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
 * - Hot-reload support with file watching
 * 
 * Supported formats:
 * - Models: glTF, GLB, FBX
 * - Textures: PNG, JPG, TGA, BMP, HDR, EXR
 * - Audio: WAV, MP3, OGG, FLAC
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
 * auto audio = resourceSys.Load<AudioResource>("audio/music.mp3");
 * 
 * // Or use ResourceManager directly
 * ResourceManager::Get().Load<MeshResource>("models/enemy.fbx");
 * 
 * // Hot-reload support
 * HotReloadManager::Get().Initialize(&ResourceManager::Get());
 * HotReloadManager::Get().WatchDirectory("Assets/");
 * // Call in update loop:
 * HotReloadManager::Get().Update();
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
#include "Resource/Types/AudioResource.h"

// Loaders
#include "Resource/Loader/TextureLoader.h"
#include "Resource/Loader/ModelLoader.h"
#include "Resource/Loader/AudioLoader.h"
#include "Resource/Loader/HDRTextureLoader.h"

// Importers
#include "Resource/Importer/GLTFImporter.h"
#include "Resource/Importer/FBXImporter.h"

// Hot-reload
#include "Resource/FileWatcher.h"
#include "Resource/HotReloadManager.h"
