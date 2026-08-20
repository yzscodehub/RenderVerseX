#pragma once

/** @file ResourceTypes.h @brief Runtime asset identity and load-domain contracts */

#include "Core/Types.h"

namespace RVX::Resource
{
    using ResourceId = uint64;
    inline constexpr ResourceId InvalidResourceId = 0;

    enum class ResourceType : uint32
    {
        Unknown = 0,
        Mesh,
        Texture,
        Material,
        Shader,
        Skeleton,
        Animation,
        Audio,
        Scene,
        Model,
        Prefab,
        Script,
        Environment,
        Custom = 1000,
    };

    enum class ResourceLoadDomain : uint8
    {
        Unknown = 0,
        Source,
        Cooked,
        Package,

        // Transitional spellings map exactly to the public domain contract.
        SourceAsset = Source,
        CookedArtifact = Cooked,
        RuntimePackage = Package,
    };

    [[nodiscard]] const char* GetResourceTypeName(ResourceType type);
} // namespace RVX::Resource

namespace RVX
{
    using Resource::GetResourceTypeName;
    using Resource::InvalidResourceId;
    using Resource::ResourceId;
    using Resource::ResourceLoadDomain;
    using Resource::ResourceType;
} // namespace RVX
