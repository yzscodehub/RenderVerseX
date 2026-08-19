#include "ModelTextureStreamingService.h"

#include "Core/Job/JobSystem.h"
#include "Core/Log.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace RVX::Resource
{
ModelTextureStreamingService::ModelTextureStreamingService(
    uint64 decodedByteBudget,
    uint32 maxConcurrentDecodes)
{
    m_stats.decodedByteBudget = decodedByteBudget;
    const size_t workerCount = JobSystem::Get().GetWorkerCount();
    const uint32 workerLimit = workerCount == 0
                                   ? 1u
                                   : static_cast<uint32>(workerCount);
    m_stats.maxConcurrentDecodes =
        std::max(1u, std::min({4u, maxConcurrentDecodes, workerLimit}));
}

ModelTextureStreamingService::~ModelTextureStreamingService()
{
    Stop();
}

bool ModelTextureStreamingService::Start(
    ResourceHandle<ModelResource> model)
{
    if (!model)
        return false;

    std::vector<ModelTextureStreamingSource> sources =
        model->BeginTextureStreaming();
    const ModelTextureStreamingSnapshot snapshot =
        model->GetTextureStreamingSnapshot();
    if (sources.empty())
        return snapshot.stage == ModelTextureStreamingStage::Uploading ||
               snapshot.stage == ModelTextureStreamingStage::FullyResident ||
               snapshot.stage == ModelTextureStreamingStage::None;

    for (const ModelTextureStreamingSource& source : sources)
    {
        if (!source.texture || source.estimatedDecodedBytes == 0 ||
            source.estimatedDecodedBytes > m_stats.decodedByteBudget)
        {
            const std::string reason = !source.preflightError.empty()
                ? source.preflightError
                : "Model texture exceeds the configured decoded-byte budget";
            model->MarkTextureStreamingFailed(reason);
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_stats.failedDecodes;
            return false;
        }
    }

    const std::shared_ptr<std::atomic_bool> cancellationToken =
        std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping)
        {
            model->CancelTextureStreaming();
            return false;
        }
        m_liveModels[model.GetId()] = model;
        m_cancellationTokens[model.GetId()] = cancellationToken;
        for (ModelTextureStreamingSource& source : sources)
            m_pending.push_back(
                {model, std::move(source), cancellationToken});
        m_stats.queuedDecodes = static_cast<uint32>(m_pending.size());
    }
    Pump();
    return true;
}

void ModelTextureStreamingService::Pump()
{
    for (;;)
    {
        DecodeTask task;
        uint64 reservation = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping ||
                m_stats.activeDecodes >= m_stats.maxConcurrentDecodes)
            {
                return;
            }

            const size_t beforePrune = m_pending.size();
            std::erase_if(
                m_pending,
                [](const DecodeTask& value)
                {
                    return !value.model ||
                           !value.cancellationToken ||
                           value.cancellationToken->load(
                               std::memory_order_acquire) ||
                           value.model->GetTextureStreamingSnapshot().stage !=
                               ModelTextureStreamingStage::Decoding;
                });
            m_stats.cancelledDecodes += beforePrune - m_pending.size();

            auto selected = std::find_if(
                m_pending.begin(),
                m_pending.end(),
                [this](const DecodeTask& value)
                {
                    return value.source.estimatedDecodedBytes <=
                           m_stats.decodedByteBudget -
                               m_stats.reservedDecodedBytes;
                });
            if (selected == m_pending.end())
            {
                m_stats.queuedDecodes =
                    static_cast<uint32>(m_pending.size());
                return;
            }

            reservation = selected->source.estimatedDecodedBytes;
            task = std::move(*selected);
            m_pending.erase(selected);
            m_stats.reservedDecodedBytes += reservation;
            m_stats.peakReservedDecodedBytes =
                std::max(m_stats.peakReservedDecodedBytes,
                         m_stats.reservedDecodedBytes);
            ++m_stats.activeDecodes;
            if (task.model)
            {
                ++m_activeDecodesByModel[task.model.GetId()];
            }
            m_stats.peakActiveDecodes =
                std::max(m_stats.peakActiveDecodes,
                         m_stats.activeDecodes);
            m_stats.queuedDecodes =
                static_cast<uint32>(m_pending.size());
        }

        JobSubmissionDesc desc;
        desc.category = "Resource.ModelTextureDecode";
        desc.priority = JobPriority::Normal;
        const ResourceHandle<ModelResource> failureModel = task.model;
        const ResourceHandle<TextureResource> failureTexture =
            task.source.texture;
        const std::shared_ptr<std::atomic_bool> failureToken =
            task.cancellationToken;
        JobHandle job;
        try
        {
            job = JobSystem::Get().Submit(
                [this, task = std::move(task), reservation]() mutable
                {
                    Execute(std::move(task), reservation);
                },
                desc);
        }
        catch (const std::exception& exception)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            DecodeCompletion completion;
            completion.model = failureModel;
            completion.texture = failureTexture;
            completion.reservedBytes = reservation;
            completion.cancellationToken = failureToken;
            completion.error =
                std::string("Model texture job submission failed: ") +
                exception.what();
            m_completions.push_back(std::move(completion));
            return;
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            DecodeCompletion completion;
            completion.model = failureModel;
            completion.texture = failureTexture;
            completion.reservedBytes = reservation;
            completion.cancellationToken = failureToken;
            completion.error =
                "Model texture job submission failed with an unknown exception";
            m_completions.push_back(std::move(completion));
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.push_back(std::move(job));
    }
}

void ModelTextureStreamingService::Execute(
    DecodeTask task,
    uint64 reservedBytes)
{
    DecodeCompletion completion;
    completion.model = task.model;
    completion.texture = task.source.texture;
    completion.reservedBytes = reservedBytes;
    completion.cancellationToken = task.cancellationToken;
    try
    {
        if (!task.model || !task.cancellationToken ||
            task.cancellationToken->load(std::memory_order_acquire) ||
            task.model->GetTextureStreamingSnapshot().stage !=
                ModelTextureStreamingStage::Decoding)
        {
            completion.cancelled = true;
        }
        else
        {
            TextureLoader decoder(nullptr, true);
            if (!decoder.DecodeReference(task.source.reference,
                                         task.source.sourceModelPath,
                                         task.source.traceContext,
                                         completion.data,
                                         completion.error,
                                         [&task]()
                                         {
                                             return !task.cancellationToken ||
                                                    task.cancellationToken->load(
                                                        std::memory_order_acquire);
                                         }))
            {
                if (task.cancellationToken->load(std::memory_order_acquire))
                {
                    completion.error.clear();
                    completion.cancelled = true;
                }
                else if (completion.error.empty())
                {
                    completion.error = "Model texture decode failed";
                }
            }
            else if (task.cancellationToken->load(std::memory_order_acquire))
            {
                completion.data = {};
                completion.cancelled = true;
            }
            else if (completion.data.bytes->size() > reservedBytes)
            {
                completion.data = {};
                completion.error =
                    "Decoded texture exceeded its header-derived budget reservation";
            }
        }
    }
    catch (const std::exception& exception)
    {
        completion.data = {};
        completion.error =
            std::string("Model texture decode raised an exception: ") +
            exception.what();
    }
    catch (...)
    {
        completion.data = {};
        completion.error =
            "Model texture decode raised an unknown exception";
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_completions.push_back(std::move(completion));
}

void ModelTextureStreamingService::DrainCompletions(
    const std::function<bool(TextureResource*)>& publishReplacement)
{
    std::vector<DecodeCompletion> completions;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        completions.swap(m_completions);
    }

    for (DecodeCompletion& completion : completions)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stats.activeDecodes != 0)
                --m_stats.activeDecodes;
            if (completion.model)
            {
                const auto active =
                    m_activeDecodesByModel.find(completion.model.GetId());
                if (active != m_activeDecodesByModel.end())
                {
                    if (active->second > 1)
                    {
                        --active->second;
                    }
                    else
                    {
                        m_activeDecodesByModel.erase(active);
                    }
                }
            }
        }

        const auto releaseReservation = [this, &completion]()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stats.reservedDecodedBytes -=
                std::min(m_stats.reservedDecodedBytes,
                         completion.reservedBytes);
        };

        if (completion.cancelled || !completion.model ||
            !completion.cancellationToken ||
            completion.cancellationToken->load(std::memory_order_acquire) ||
            completion.model->GetTextureStreamingSnapshot().stage !=
                ModelTextureStreamingStage::Decoding)
        {
            releaseReservation();
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_stats.cancelledDecodes;
            continue;
        }
        if (!completion.error.empty() || !completion.data.IsValid() ||
            !completion.texture)
        {
            completion.model->MarkTextureStreamingFailed(
                completion.error.empty()
                    ? "Model texture decode produced invalid data"
                    : completion.error);
            releaseReservation();
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_stats.failedDecodes;
            continue;
        }

        completion.texture->SetDataStorage(
            completion.data.bytes,
            completion.data.metadata);
        if (!publishReplacement ||
            !publishReplacement(completion.texture.Get()))
        {
            completion.model->MarkTextureStreamingFailed(
                "Decoded texture could not be published for GPU replacement");
            completion.texture->ReleaseCPUData();
            releaseReservation();
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_stats.failedDecodes;
            continue;
        }

        bool publicationInserted = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            publicationInserted = m_pendingPublications.emplace(
                completion.texture.GetId(),
                PendingPublication{completion.model,
                                   completion.texture,
                                   completion.reservedBytes}).second;
        }
        if (!publicationInserted)
        {
            completion.model->MarkTextureStreamingFailed(
                "A model texture already has a pending GPU publication");
            completion.texture->ReleaseCPUData();
            releaseReservation();
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_stats.failedDecodes;
            continue;
        }
        completion.model->MarkTextureDecodeComplete(
            static_cast<uint64>(completion.data.bytes->size()));
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_stats.completedDecodes;
        m_stats.completedDecodedBytes += completion.data.bytes->size();
    }

    PruneCompletedJobs();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::erase_if(
            m_liveModels,
            [](const auto& entry)
            {
                if (!entry.second)
                    return true;
                const ModelTextureStreamingStage stage =
                    entry.second->GetTextureStreamingSnapshot().stage;
                return stage == ModelTextureStreamingStage::FullyResident ||
                       stage == ModelTextureStreamingStage::Failed ||
                       stage == ModelTextureStreamingStage::Cancelled ||
                       stage == ModelTextureStreamingStage::None;
            });
        std::erase_if(
            m_cancellationTokens,
            [this](const auto& entry)
            {
                return !m_liveModels.contains(entry.first);
            });
    }
    Pump();
}

std::vector<ResourceHandle<TextureResource>>
ModelTextureStreamingService::GetPendingPublications() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ResourceHandle<TextureResource>> result;
    result.reserve(m_pendingPublications.size());
    for (const auto& [id, publication] : m_pendingPublications)
    {
        (void)id;
        if (publication.texture)
            result.push_back(publication.texture);
    }
    return result;
}

bool ModelTextureStreamingService::CompletePublication(
    ResourceId textureId,
    bool succeeded,
    std::string error)
{
    PendingPublication publication;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_pendingPublications.find(textureId);
        if (found == m_pendingPublications.end())
            return false;
        publication = std::move(found->second);
        m_pendingPublications.erase(found);
        m_stats.reservedDecodedBytes -=
            std::min(m_stats.reservedDecodedBytes,
                     publication.reservedBytes);
    }

    if (publication.texture)
    {
        if (succeeded)
            publication.texture->MarkStreamingPlaceholder(false);
        publication.texture->ReleaseCPUData();
    }
    if (publication.model)
    {
        if (succeeded)
        {
            publication.model->MarkTexturePublicationComplete(textureId);
        }
        else
        {
            publication.model->MarkTextureStreamingFailed(
                error.empty()
                    ? "A streamed texture GPU publication failed"
                    : std::move(error));
        }

        const ModelTextureStreamingStage stage =
            publication.model->GetTextureStreamingSnapshot().stage;
        if (stage == ModelTextureStreamingStage::FullyResident ||
            stage == ModelTextureStreamingStage::Failed ||
            stage == ModelTextureStreamingStage::Cancelled)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_liveModels.erase(publication.model.GetId());
            m_cancellationTokens.erase(publication.model.GetId());
        }
    }

    Pump();
    return true;
}

ModelTextureStreamingCancellationResult
ModelTextureStreamingService::CancelModel(ResourceId modelId)
{
    ModelTextureStreamingCancellationResult result;
    ResourceHandle<ModelResource> model;
    std::vector<ResourceHandle<TextureResource>> textures;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto live = m_liveModels.find(modelId);
        if (live != m_liveModels.end())
        {
            model = live->second;
            m_liveModels.erase(live);
            result.modelFound = true;
        }

        const auto token = m_cancellationTokens.find(modelId);
        if (token != m_cancellationTokens.end())
        {
            token->second->store(true, std::memory_order_release);
            m_cancellationTokens.erase(token);
            result.modelFound = true;
        }

        const size_t before = m_pending.size();
        std::erase_if(
            m_pending,
            [modelId](const DecodeTask& task)
            {
                return task.model && task.model.GetId() == modelId;
            });
        result.cancelledQueuedDecodes = before - m_pending.size();
        m_stats.cancelledDecodes += result.cancelledQueuedDecodes;
        m_stats.queuedDecodes = static_cast<uint32>(m_pending.size());

        const auto active = m_activeDecodesByModel.find(modelId);
        if (active != m_activeDecodesByModel.end())
        {
            result.cancellationRequestedForActiveDecodes = active->second;
            result.modelFound = true;
        }

        for (auto it = m_pendingPublications.begin();
             it != m_pendingPublications.end();)
        {
            if (it->second.model && it->second.model.GetId() == modelId)
            {
                const uint64 released = std::min(
                    m_stats.reservedDecodedBytes,
                    it->second.reservedBytes);
                m_stats.reservedDecodedBytes -= released;
                result.releasedReservedBytes += released;
                if (it->second.texture)
                    textures.push_back(it->second.texture);
                it = m_pendingPublications.erase(it);
                ++result.cancelledPendingPublications;
                result.modelFound = true;
            }
            else
            {
                ++it;
            }
        }
    }

    for (const ResourceHandle<TextureResource>& texture : textures)
    {
        if (texture)
            texture->ReleaseCPUData();
    }
    Pump();
    return result;
}

void ModelTextureStreamingService::PruneCompletedJobs()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::erase_if(m_jobs,
                  [](const JobHandle& job) { return job.IsComplete(); });
}

void ModelTextureStreamingService::Stop()
{
    std::vector<JobHandle> jobs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping && m_jobs.empty())
            return;
        m_stopping = true;
        for (auto& [id, model] : m_liveModels)
        {
            (void)id;
            if (model)
                model->CancelTextureStreaming();
        }
        for (const auto& [id, token] : m_cancellationTokens)
        {
            (void)id;
            if (token)
            {
                token->store(true, std::memory_order_release);
            }
        }
        m_stats.cancelledDecodes += m_pending.size();
        m_pending.clear();
        jobs = m_jobs;
    }
    for (const JobHandle& job : jobs)
        job.Wait();

    std::lock_guard<std::mutex> lock(m_mutex);

    // Jobs are joined before this point, so every admitted active task has
    // either left a completion record or failed before it could publish one.
    // Stop deliberately discards those records instead of calling the
    // owner-thread publication callback; account for that cancellation here
    // rather than silently zeroing activeDecodes below. A completion that was
    // already a real decode failure remains a failure, while a valid but
    // un-published decode is cancelled by this shutdown boundary.
    uint64 cancelledCompletions = 0;
    uint64 failedCompletions = 0;
    for (const DecodeCompletion& completion : m_completions)
    {
        if (completion.cancelled || completion.data.IsValid())
        {
            ++cancelledCompletions;
        }
        else
        {
            ++failedCompletions;
        }
    }
    m_stats.cancelledDecodes += cancelledCompletions;
    m_stats.failedDecodes += failedCompletions;
    m_jobs.clear();
    m_completions.clear();
    m_pendingPublications.clear();
    m_liveModels.clear();
    m_activeDecodesByModel.clear();
    m_cancellationTokens.clear();
    m_stats.activeDecodes = 0;
    m_stats.queuedDecodes = 0;
    m_stats.reservedDecodedBytes = 0;
}

ModelTextureStreamingStats ModelTextureStreamingService::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ModelTextureStreamingStats result = m_stats;
    result.queuedDecodes = static_cast<uint32>(m_pending.size());
    result.pendingPublications =
        static_cast<uint32>(m_pendingPublications.size());
    return result;
}
} // namespace RVX::Resource
