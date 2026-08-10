#pragma once

#include "Resource/Loader/TextureLoader.h"
#include "Resource/ResourceManager.h"
#include "Resource/Types/ModelResource.h"

#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace RVX::Resource
{
    /** @brief Budgeted worker decode queue; owner-thread publication remains external. */
    class ModelTextureStreamingService final
    {
    public:
        ModelTextureStreamingService(uint64 decodedByteBudget,
                                     uint32 maxConcurrentDecodes);
        ~ModelTextureStreamingService();

        ModelTextureStreamingService(const ModelTextureStreamingService&) = delete;
        ModelTextureStreamingService& operator=(
            const ModelTextureStreamingService&) = delete;

        bool Start(ResourceHandle<ModelResource> model);
        void DrainCompletions(
            const std::function<bool(TextureResource*)>& publishReplacement);
        [[nodiscard]] std::vector<ResourceHandle<TextureResource>>
            GetPendingPublications() const;
        bool CompletePublication(ResourceId textureId,
                                 bool succeeded,
                                 std::string error = {});
        void CancelModel(ResourceId modelId);
        void Stop();

        [[nodiscard]] ModelTextureStreamingStats GetStats() const;

    private:
        struct DecodeTask
        {
            ResourceHandle<ModelResource> model;
            ModelTextureStreamingSource source;
        };

        struct DecodeCompletion
        {
            ResourceHandle<ModelResource> model;
            ResourceHandle<TextureResource> texture;
            DecodedTextureData data;
            std::string error;
            uint64 reservedBytes = 0;
            bool cancelled = false;
        };

        struct PendingPublication
        {
            ResourceHandle<ModelResource> model;
            ResourceHandle<TextureResource> texture;
            uint64 reservedBytes = 0;
        };

        void Pump();
        void Execute(DecodeTask task, uint64 reservedBytes);
        void PruneCompletedJobs();

        mutable std::mutex m_mutex;
        std::deque<DecodeTask> m_pending;
        std::vector<DecodeCompletion> m_completions;
        std::vector<JobHandle> m_jobs;
        std::unordered_map<ResourceId, ResourceHandle<ModelResource>>
            m_liveModels;
        std::unordered_map<ResourceId, PendingPublication>
            m_pendingPublications;
        ModelTextureStreamingStats m_stats;
        bool m_stopping = false;
    };
} // namespace RVX::Resource
