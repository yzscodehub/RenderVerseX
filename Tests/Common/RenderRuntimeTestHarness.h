#pragma once

/** @file RenderRuntimeTestHarness.h @brief Exact-registry fixture support. */

#include "RenderContracts/IRenderResourceGateway.h"
#include "Resource/IResource.h"

#include <memory>
#include <unordered_map>

namespace RVX
{
    class IRHIDevice;
    class RHITexture;
    class RenderResourceGateway;
    class RenderResourceRegistry;
    class RenderRetirementQueue;
    class RenderSubmissionTracker;
    class RenderUploadProcessor;
    struct MeshGPUBuffers;

    /** @brief Test-only owner of the same exact upload/registry path as Render. */
    class RenderRuntimeTestHarness final
    {
    public:
        RenderRuntimeTestHarness();
        ~RenderRuntimeTestHarness();

        RenderRuntimeTestHarness(const RenderRuntimeTestHarness&) = delete;
        RenderRuntimeTestHarness& operator=(
            const RenderRuntimeTestHarness&) = delete;

        [[nodiscard]] bool Initialize(IRHIDevice* device);
        void Shutdown();

        [[nodiscard]] RenderResourceHandle Reserve(
            Resource::IResource* resource);
        [[nodiscard]] bool UploadImmediate(Resource::IResource* resource);
        [[nodiscard]] RenderResourceHandle ResolveOrUpload(
            Resource::IResource* resource);
        [[nodiscard]] RenderResourceHandle GetHandle(uint64 resourceId) const;
        [[nodiscard]] RenderResourcePublicState GetResourceState(
            uint64 resourceId) const;
        [[nodiscard]] bool IsGPUReady(uint64 resourceId) const;
        [[nodiscard]] RHITexture* GetTexture(uint64 resourceId) const;
        [[nodiscard]] MeshGPUBuffers GetMeshBuffers(uint64 resourceId) const;
        [[nodiscard]] RenderResourceRegistry& GetRegistry();
        [[nodiscard]] const RenderResourceRegistry& GetRegistry() const;

    private:
        IRHIDevice* m_device = nullptr;
        std::unique_ptr<RenderResourceGateway> m_gateway;
        std::unique_ptr<RenderSubmissionTracker> m_tracker;
        std::unique_ptr<RenderRetirementQueue> m_retirement;
        std::unique_ptr<RenderResourceRegistry> m_registry;
        std::unique_ptr<RenderUploadProcessor> m_processor;
        std::unordered_map<uint64, RenderResourceHandle> m_handles;
        uint64 m_nextSequence = 1;
    };
} // namespace RVX
