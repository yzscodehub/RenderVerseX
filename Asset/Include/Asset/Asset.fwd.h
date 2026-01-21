#pragma once

/**
 * @file Asset.fwd.h
 * @brief Forward declarations for Asset module
 */

namespace RVX::Asset
{
    // Core
    class Asset;
    template<typename T> class AssetHandle;
    class AssetRegistry;
    class AssetCache;
    class DependencyGraph;
    class ResourceManager;
    class IAssetLoader;

    // Types
    class MeshAsset;
    class TextureAsset;
    class MaterialAsset;
    class ShaderAsset;
    class SkeletonAsset;
    class AnimationAsset;
    class ModelAsset;
    class SceneAsset;
    class PrefabAsset;

    // Type aliases
    using AssetId = uint64_t;
}
