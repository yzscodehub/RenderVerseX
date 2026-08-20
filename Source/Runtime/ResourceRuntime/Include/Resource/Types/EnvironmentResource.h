#pragma once

/**
 * @file EnvironmentResource.h
 * @brief Formal HDR/IBL environment resource composed of texture resources.
 */

#include "Core/Types.h"
#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"
#include "Resource/Types/TextureResource.h"

#include <filesystem>

namespace RVX::Resource
{
    struct EnvironmentResourceData
    {
        std::filesystem::path sourcePath;
        TextureHandle environment;
        TextureHandle irradiance;
        TextureHandle prefiltered;
        TextureHandle brdfLUT;
        uint32 environmentResolution = 0;
        uint32 irradianceResolution = 0;
        uint32 prefilteredResolution = 0;
        uint32 prefilteredMipLevels = 0;
        uint32 brdfLUTResolution = 0;
        float32 intensity = 1.0f;

        /** @brief Validate immutable dependency identity without requiring residency. */
        [[nodiscard]] bool IsStructurallyValid() const noexcept;
        [[nodiscard]] bool IsValid() const noexcept;
    };

    class EnvironmentResource final : public IResource
    {
    public:
        static constexpr ResourceType StaticResourceType = ResourceType::Environment;

        ResourceType GetType() const override
        {
            return ResourceType::Environment;
        }
        const char* GetTypeName() const override { return "Environment"; }
        size_t GetMemoryUsage() const override;
        size_t GetGPUMemoryUsage() const override;
        std::vector<ResourceId> GetRequiredDependencies() const override;

        [[nodiscard]] bool SetData(EnvironmentResourceData data);
        /**
         * @brief Bind prepared dependencies without publishing the resource.
         *
         * ResourceManager remains the sole owner of the later Loaded transition.
         */
        [[nodiscard]] bool SetPreparedData(EnvironmentResourceData data);
        [[nodiscard]] const EnvironmentResourceData& GetData() const noexcept
        {
            return m_data;
        }

    private:
        EnvironmentResourceData m_data;
    };

    using EnvironmentHandle = ResourceHandle<EnvironmentResource>;
} // namespace RVX::Resource

namespace RVX
{
    using Resource::EnvironmentHandle;
    using Resource::EnvironmentResource;
    using Resource::EnvironmentResourceData;
} // namespace RVX
