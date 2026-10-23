/** @file ResourceTypes.cpp @brief Runtime asset type names */

#include "AssetContracts/ResourceTypes.h"

namespace RVX::Resource
{
const char* GetResourceTypeName(ResourceType type)
{
    switch (type)
    {
        case ResourceType::Unknown: return "Unknown";
        case ResourceType::Mesh: return "Mesh";
        case ResourceType::Texture: return "Texture";
        case ResourceType::Material: return "Material";
        case ResourceType::Shader: return "Shader";
        case ResourceType::Skeleton: return "Skeleton";
        case ResourceType::Animation: return "Animation";
        case ResourceType::Audio: return "Audio";
        case ResourceType::Scene: return "Scene";
        case ResourceType::Model: return "Model";
        case ResourceType::Prefab: return "Prefab";
        case ResourceType::Script: return "Script";
        case ResourceType::Environment: return "Environment";
        case ResourceType::Custom: return "Custom";
        default: return "Custom";
    }
}
} // namespace RVX::Resource
