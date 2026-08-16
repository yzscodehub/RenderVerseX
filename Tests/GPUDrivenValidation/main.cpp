#include "Core/Log.h"
#include "Render/GPUScene/GPUSceneSchema.h"
#include "GPUScene/GPUSceneUploader.h"
#include "Render/GPUDriven/GPUCulling.h"
#include "Render/GPUDriven/GPUDrivenDiagnostics.h"
#include "Render/GPUDriven/GPUDrivenPolicy.h"
#include "Render/Renderer/RenderDrawPacket.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Submission/DirectRasterReadbackQualification.h"
#include "Render/Submission/RenderSubmissionStrategy.h"
#include "Render/Visibility/RenderVisibility.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "Resources/RenderSubmissionTracker.h"
#include "Runtime/RenderResourceStatusTable.h"
#include "RHI/RHI.h"
#include "RHI/RHICommandContext.h"
#include "ShaderCompiler/ShaderCompiler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace RVX;

namespace RVX
{
    struct GPUCullingQualificationTestAccess
    {
        static bool ConfigureInputCoverage(
            GPUCulling& culling,
            std::span<const uint64> expectedIdentities,
            std::span<const uint64> observedIdentities,
            uint32 directPacketCount = 0,
            uint32 skippedPacketCount = 0,
            std::span<const uint64> expectedDirectVisibleIdentities = {})
        {
            GPUCullingQualificationInputPlan plan;
            plan.expectedGPUInputPacketCount =
                static_cast<uint32>(expectedIdentities.size());
            plan.directPacketCount = directPacketCount;
            plan.skippedPacketCount = skippedPacketCount;
            plan.expectedPacketCount = plan.expectedGPUInputPacketCount +
                plan.directPacketCount + plan.skippedPacketCount;
            plan.planPacketIdentityHash = 0xA11CE5EEDull;
            plan.expectedGPUInputIdentityHashes.assign(
                expectedIdentities.begin(), expectedIdentities.end());
            plan.expectedDirectVisibleIdentityHashes.assign(
                expectedDirectVisibleIdentities.begin(),
                expectedDirectVisibleIdentities.end());
            plan.exactlyOncePartitioned = true;
            if (!culling.BeginGPUSceneQualificationInputCoverage(plan))
            {
                return false;
            }
            for (const uint64 identity : observedIdentities)
            {
                if (!culling.ObserveGPUSceneQualificationInputIdentity(identity))
                {
                    return false;
                }
            }
            return true;
        }

        static bool SetCollectedRasterSemanticIdentities(
            GPUCulling& culling,
            std::span<const uint64> identities)
        {
            if (identities.size() != culling.m_instances.size())
            {
                return false;
            }
            try
            {
                culling.m_collectedRasterSemanticIdentities.assign(
                    identities.begin(), identities.end());
            }
            catch (...)
            {
                return false;
            }
            return true;
        }

        /**
         * @brief Supplies test readback with the same post-binding production
         * semantic finalizer used by OpaquePass.
         */
        static bool FinalizeTierOneEvidenceForExpectedReadback(
            GPUCulling& culling);

        static std::vector<uint64> GetCapturedRasterSemanticIdentities(
            const GPUCulling& culling)
        {
            return culling.m_gpuSceneQualificationCapture
                .cpuActiveRasterSemanticIdentities;
        }

        static bool IsRasterSemanticEvidenceFinalized(
            const GPUCulling& culling)
        {
            return culling.m_gpuSceneQualificationCapture
                .rasterSemanticEvidenceFinalized;
        }

        static std::array<uint64, 3> GetCanonicalVersions(
            const GPUCulling& culling)
        {
            return {culling.m_canonicalInstanceVersion,
                    culling.m_canonicalGPUSceneCandidateVersion,
                    culling.m_canonicalActiveRowVersion};
        }

        static bool ExpectedRasterPayloadMatchesResidentBuffer(
            GPUCulling& culling)
        {
            GPUCulling::GPUSceneQualificationCapture& capture =
                culling.m_gpuSceneQualificationCapture;
            if (capture.expectedRasterInstances.empty() ||
                capture.cpuActiveResidentRows.empty())
            {
                return false;
            }
            RHIBuffer* const buffer = culling.GetInstanceBuffer();
            if (buffer == nullptr)
            {
                return false;
            }
            void* const mapped = buffer->Map();
            if (mapped == nullptr)
            {
                return false;
            }
            const uint32 residentRow = capture.cpuActiveResidentRows.front();
            const bool matches = std::memcmp(
                &capture.expectedRasterInstances.front(),
                static_cast<GPUInstanceData*>(mapped) + residentRow,
                sizeof(GPUInstanceData)) == 0;
            buffer->Unmap();
            return matches;
        }

        static bool OverwriteFirstCanonicalInstance(
            GPUCulling& culling,
            const GPUInstanceData& instance)
        {
            if (culling.m_collectedCanonicalRows.empty())
            {
                return false;
            }
            const uint32 residentRow = culling.m_collectedCanonicalRows.front();
            if (residentRow >= culling.m_canonicalInstances.size())
            {
                return false;
            }
            culling.m_canonicalInstances[residentRow] =
                GPUCulling::NormalizeCanonicalInstance(instance);
            return true;
        }

        static bool WriteExpectedReadback(GPUCulling& culling,
                                          bool writeRasterPayload = true)
        {
            GPUCulling::GPUSceneQualificationCapture& capture =
                culling.m_gpuSceneQualificationCapture;
            if (!capture.referencePrepared || !capture.IsAllocated())
            {
                return false;
            }
            // Synthetic readback still uses the production finalizer. It
            // cannot bless an unfinalized capture by flipping diagnostic
            // state directly.
            if (capture.capturedTier == GPUDrivenTier::IndirectGrouped &&
                !capture.rasterSemanticEvidenceFinalized)
            {
                if (!FinalizeTierOneEvidenceForExpectedReadback(culling))
                {
                    return false;
                }
            }

            void* visibility = capture.visibilityReadback->Map();
            void* visibleRows = capture.visibleRowsReadback->Map();
            void* drawCounts = capture.drawCountsReadback->Map();
            void* indirect = capture.indirectCommandsReadback->Map();
            void* rasterInstances = writeRasterPayload &&
                    capture.capturedTier ==
                    GPUDrivenTier::IndirectGrouped
                ? capture.rasterInstanceReadback->Map()
                : nullptr;
            if (visibility == nullptr || visibleRows == nullptr ||
                drawCounts == nullptr || indirect == nullptr ||
                (writeRasterPayload &&
                 capture.capturedTier == GPUDrivenTier::IndirectGrouped &&
                 rasterInstances == nullptr))
            {
                if (visibility != nullptr) capture.visibilityReadback->Unmap();
                if (visibleRows != nullptr) capture.visibleRowsReadback->Unmap();
                if (drawCounts != nullptr) capture.drawCountsReadback->Unmap();
                if (indirect != nullptr) capture.indirectCommandsReadback->Unmap();
                if (rasterInstances != nullptr)
                    capture.rasterInstanceReadback->Unmap();
                return false;
            }

            std::memcpy(visibility,
                        capture.expectedVisibility.data(),
                        capture.expectedVisibility.size() * sizeof(uint32));
            std::memcpy(visibleRows,
                        capture.expectedVisibleResidentRows.data(),
                        capture.expectedVisibleResidentRows.size() * sizeof(uint32));
            auto* counts = static_cast<uint32*>(drawCounts);
            counts[0] = 0;
            for (uint32 groupIndex = 0;
                 groupIndex < capture.drawGroupCount;
                 ++groupIndex)
            {
                counts[groupIndex + 1u] = capture.expectedDrawCounts[groupIndex];
                counts[0] += capture.expectedDrawCounts[groupIndex];
            }
            auto* commands = static_cast<IndirectDrawIndexedCommand*>(indirect);
            for (uint32 groupIndex = 0;
                 groupIndex < capture.drawGroupCount;
                 ++groupIndex)
            {
                commands[capture.drawGroups[groupIndex].commandOffset] =
                    capture.expectedIndirectCommands[groupIndex];
            }
            if (rasterInstances != nullptr)
            {
                std::memset(rasterInstances, 0,
                            static_cast<size_t>(
                                capture.rasterInstanceReadback->GetSize()));
                auto* raster = static_cast<GPUInstanceData*>(rasterInstances);
                for (uint32 activeIndex = 0;
                     activeIndex < capture.activeRowCount; ++activeIndex)
                {
                    raster[capture.cpuActiveResidentRows[activeIndex]] =
                        capture.expectedRasterInstances[activeIndex];
                }
            }
            capture.visibilityReadback->Unmap();
            capture.visibleRowsReadback->Unmap();
            capture.drawCountsReadback->Unmap();
            capture.indirectCommandsReadback->Unmap();
            if (rasterInstances != nullptr)
                capture.rasterInstanceReadback->Unmap();
            return true;
        }

        static bool CorruptFirstVisibleResidentRow(GPUCulling& culling)
        {
            GPUCulling::GPUSceneQualificationCapture& capture =
                culling.m_gpuSceneQualificationCapture;
            if (!capture.referencePrepared || capture.expectedVisibleResidentRows.empty())
            {
                return false;
            }
            uint32 firstVisibleRow = RVX_INVALID_INDEX;
            for (uint32 index = 0;
                 index < static_cast<uint32>(capture.expectedVisibleResidentRows.size());
                 ++index)
            {
                if (capture.expectedVisibleResidentRows[index] != RVX_INVALID_INDEX)
                {
                    firstVisibleRow = index;
                    break;
                }
            }
            if (firstVisibleRow == RVX_INVALID_INDEX)
            {
                return false;
            }
            void* mapped = capture.visibleRowsReadback->Map();
            if (mapped == nullptr)
            {
                return false;
            }
            auto* rows = static_cast<uint32*>(mapped);
            rows[firstVisibleRow] = capture.expectedVisibleResidentRows[firstVisibleRow] + 1u;
            capture.visibleRowsReadback->Unmap();
            return true;
        }

        static bool CorruptFirstRasterPayload(GPUCulling& culling)
        {
            GPUCulling::GPUSceneQualificationCapture& capture =
                culling.m_gpuSceneQualificationCapture;
            if (capture.capturedTier != GPUDrivenTier::IndirectGrouped ||
                capture.cpuActiveResidentRows.empty() ||
                !capture.rasterInstanceReadback)
            {
                return false;
            }
            auto* const mapped = static_cast<GPUInstanceData*>(
                capture.rasterInstanceReadback->Map());
            if (mapped == nullptr)
            {
                return false;
            }
            std::memset(&mapped[capture.cpuActiveResidentRows.front()], 0,
                        sizeof(GPUInstanceData));
            capture.rasterInstanceReadback->Unmap();
            return true;
        }

        static bool CorruptFirstIndirectArgument(GPUCulling& culling,
                                                 uint32 fieldIndex)
        {
            GPUCulling::GPUSceneQualificationCapture& capture =
                culling.m_gpuSceneQualificationCapture;
            if (capture.drawGroups.empty() ||
                !capture.indirectCommandsReadback)
            {
                return false;
            }
            auto* const mapped = static_cast<IndirectDrawIndexedCommand*>(
                capture.indirectCommandsReadback->Map());
            if (mapped == nullptr)
            {
                return false;
            }
            IndirectDrawIndexedCommand& command = mapped[
                capture.drawGroups.front().commandOffset];
            switch (fieldIndex)
            {
                case 0u: ++command.indexCount; break;
                case 1u: ++command.instanceCount; break;
                case 2u: ++command.firstIndex; break;
                case 3u: ++command.vertexOffset; break;
                case 4u: ++command.firstInstance; break;
                default:
                    capture.indirectCommandsReadback->Unmap();
                    return false;
            }
            capture.indirectCommandsReadback->Unmap();
            return true;
        }

        static bool SetFirstVisibility(GPUCulling& culling, uint32 visibility)
        {
            GPUCulling::GPUSceneQualificationCapture& capture =
                culling.m_gpuSceneQualificationCapture;
            if (capture.activeRowCount == 0 || !capture.visibilityReadback)
            {
                return false;
            }
            auto* const mapped = static_cast<uint32*>(
                capture.visibilityReadback->Map());
            if (mapped == nullptr)
            {
                return false;
            }
            mapped[0] = visibility;
            capture.visibilityReadback->Unmap();
            return true;
        }

        static bool ReplaceActiveRowIdentities(
            GPUCulling& culling,
            std::span<const uint64> identities)
        {
            GPUCulling::GPUSceneQualificationCapture& capture =
                culling.m_gpuSceneQualificationCapture;
            if (!capture.referencePrepared)
            {
                return false;
            }
            capture.activeRowIdentityHashes.assign(
                identities.begin(), identities.end());
            return true;
        }

        static bool NotifyWithIdentity(
            GPUCulling& owner,
            GPUCulling& sealed,
            const GPUCullingRecordingIdentity& identity,
            const GPUCompletionToken& completion,
            const RenderSubmissionTracker& tracker)
        {
            return owner.AcceptGPUSceneQualificationSubmission(
                identity,
                std::move(sealed.m_gpuSceneQualificationCapture),
                completion,
                tracker);
        }
    };
} // namespace RVX

namespace
{
    struct BufferLifetimeState
    {
        uint32 destroyedCount = 0;
    };

    class FakeBuffer final : public RHIBuffer
    {
    public:
        explicit FakeBuffer(const RHIBufferDesc& desc,
                            std::shared_ptr<BufferLifetimeState> lifetimeState = {})
            : m_desc(desc)
            , m_storage(static_cast<size_t>(desc.size))
            , m_lifetimeState(std::move(lifetimeState))
        {
            m_debugName = desc.debugName ? desc.debugName : "";
        }

        ~FakeBuffer() override
        {
            if (m_lifetimeState)
            {
                ++m_lifetimeState->destroyedCount;
            }
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }

        void* Map() override
        {
            ++m_mapCallCount;
            if (!m_mapSucceeds || m_storage.empty())
            {
                return nullptr;
            }

            m_storageBeforeMappedWrite = m_storage;
            m_hasMappedWrite = true;
            return m_storage.data();
        }
        void Unmap() override
        {
            m_storageBeforeMappedWrite.clear();
            m_hasMappedWrite = false;
        }
        bool CommitMappedWrite() override
        {
            ++m_commitMappedWriteCallCount;
            const bool failThisCommit = !m_commitSucceeds ||
                m_commitFailureCountdown == 0;
            if (m_commitFailureCountdown >= 0)
            {
                --m_commitFailureCountdown;
            }
            if (failThisCommit)
            {
                if (m_hasMappedWrite)
                {
                    m_storage = std::move(m_storageBeforeMappedWrite);
                }
                m_hasMappedWrite = false;
                return false;
            }

            Unmap();
            return true;
        }

    protected:
        void* MapWriteRangeImpl(uint64 offset, uint64 size) override
        {
            m_mappedRanges.push_back({offset, size});
            void* const mapped = Map();
            return mapped != nullptr
                ? static_cast<uint8*>(mapped) + static_cast<size_t>(offset)
                : nullptr;
        }

        RHIHostWriteReceipt CommitMappedWriteRangeImpl(
            uint64 offset,
            uint64 size) override
        {
            RHIHostWriteReceipt receipt;
            if (!CommitMappedWrite())
            {
                return receipt;
            }
            receipt.committed = true;
            const RHIHostWriteSynchronization synchronization =
                m_synchronizationSequenceIndex < m_synchronizationSequence.size()
                    ? m_synchronizationSequence[
                        m_synchronizationSequenceIndex++]
                    : m_synchronization;
            receipt.synchronization = synchronization;
            if (synchronization == RHIHostWriteSynchronization::AtomAlignedRange)
            {
                receipt.synchronizedOffset = offset & ~uint64{63};
                const uint64 end = offset + size;
                receipt.synchronizedSize =
                    ((end + 63U) & ~uint64{63}) - receipt.synchronizedOffset;
                receipt.synchronizedRangeAvailable = true;
            }
            return receipt;
        }

    public:

        const std::vector<uint8>& GetStorage() const { return m_storage; }
        uint32 GetMapCallCount() const { return m_mapCallCount; }
        uint32 GetCommitMappedWriteCallCount() const { return m_commitMappedWriteCallCount; }
        const std::vector<std::pair<uint64, uint64>>& GetMappedRanges() const
        {
            return m_mappedRanges;
        }
        void SetMapSucceeds(bool succeeds) { m_mapSucceeds = succeeds; }
        void SetCommitSucceeds(bool succeeds) { m_commitSucceeds = succeeds; }
        void SetCommitFailureCountdown(int32 countdown)
        {
            m_commitFailureCountdown = countdown;
        }
        void SetSynchronization(RHIHostWriteSynchronization synchronization)
        {
            m_synchronization = synchronization;
        }
        void SetSynchronizationSequence(
            std::vector<RHIHostWriteSynchronization> synchronizations)
        {
            m_synchronizationSequence = std::move(synchronizations);
            m_synchronizationSequenceIndex = 0;
        }
        void CopyFrom(const FakeBuffer& source, uint64 sourceOffset, uint64 destinationOffset, uint64 size)
        {
            if (sourceOffset + size > source.m_storage.size() ||
                destinationOffset + size > m_storage.size())
            {
                return;
            }

            std::memcpy(m_storage.data() + destinationOffset,
                        source.m_storage.data() + sourceOffset,
                        static_cast<size_t>(size));
        }

    private:
        RHIBufferDesc m_desc;
        std::vector<uint8> m_storage;
        std::vector<uint8> m_storageBeforeMappedWrite;
        std::shared_ptr<BufferLifetimeState> m_lifetimeState;
        bool m_mapSucceeds = true;
        bool m_commitSucceeds = true;
        bool m_hasMappedWrite = false;
        int32 m_commitFailureCountdown = -1;
        RHIHostWriteSynchronization m_synchronization =
            RHIHostWriteSynchronization::CoherentNoExplicitSync;
        std::vector<RHIHostWriteSynchronization> m_synchronizationSequence;
        size_t m_synchronizationSequenceIndex = 0;
        uint32 m_mapCallCount = 0;
        uint32 m_commitMappedWriteCallCount = 0;
        std::vector<std::pair<uint64, uint64>> m_mappedRanges;
    };

    class FakeShader final : public RHIShader
    {
    public:
        explicit FakeShader(const RHIShaderDesc& desc)
            : m_stage(desc.stage)
        {
            if (desc.bytecode != nullptr && desc.bytecodeSize != 0)
            {
                const auto* bytes = static_cast<const uint8*>(desc.bytecode);
                m_bytecode.assign(bytes, bytes + desc.bytecodeSize);
            }
        }

        RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<uint8>& GetBytecode() const override { return m_bytecode; }

    private:
        RHIShaderStage m_stage = RHIShaderStage::None;
        std::vector<uint8> m_bytecode;
    };

    class FakeDescriptorSetLayout final : public RHIDescriptorSetLayout
    {
    public:
        explicit FakeDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc)
            : m_entries(desc.entries)
        {
        }

        const std::vector<RHIBindingLayoutEntry>& GetEntries() const override
        {
            return m_entries;
        }

    private:
        std::vector<RHIBindingLayoutEntry> m_entries;
    };

    class FakePipelineLayout final : public RHIPipelineLayout
    {
    public:
        explicit FakePipelineLayout(const RHIPipelineLayoutDesc& desc)
            : RHIPipelineLayout(desc)
        {
        }
    };

    class FakeComputePipeline final : public RHIPipeline
    {
    public:
        bool IsCompute() const override { return true; }
    };

    class FakeDescriptorSet final : public RHIDescriptorSet
    {
    public:
        explicit FakeDescriptorSet(const RHIDescriptorSetDesc& desc)
            : RHIDescriptorSet(desc)
        {
        }

        bool Update(const std::vector<RHIDescriptorBinding>&) override
        {
            return false;
        }
    };

    class FakeFence final : public RHIFence
    {
    public:
        explicit FakeFence(uint64 initialValue)
            : m_completedValue(initialValue)
            , m_nextValue(initialValue + 1)
        {
        }

        uint64 GetCompletedValue() const override { return m_completedValue; }
        void Signal(uint64 value) override { Complete(value); }
        void SignalOnQueue(uint64 value, RHICommandQueueType) override
        {
            Complete(value);
        }
        void Wait(uint64 value, uint64 = UINT64_MAX) override { Complete(value); }
        uint64 AllocateValue() { return m_nextValue++; }
        void Complete(uint64 value)
        {
            m_completedValue = std::max(m_completedValue, value);
        }

    private:
        uint64 m_completedValue = 0;
        uint64 m_nextValue = 1;
    };

    class FakeCommandContext final : public RHICommandContext
    {
    public:
        RHICommandQueueType GetQueueType() const override { return queueType; }
        void Begin() override {}
        void End() override {}
        void Reset() override {}
        void BeginEvent(const char*, uint32 = 0) override {}
        void EndEvent() override {}
        void SetMarker(const char*, uint32 = 0) override {}
        void BufferBarrier(const RHIBufferBarrier& barrier) override
        {
            bufferBarriers.push_back(barrier);
        }
        void TextureBarrier(const RHITextureBarrier&) override {}
        void Barriers(std::span<const RHIBufferBarrier>, std::span<const RHITextureBarrier>) override {}
        void BeginBarrier(const RHIBufferBarrier&) override {}
        void BeginBarrier(const RHITextureBarrier&) override {}
        void EndBarrier(const RHIBufferBarrier&) override {}
        void EndBarrier(const RHITextureBarrier&) override {}
        void BeginRenderPass(const RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
        void SetPipeline(RHIPipeline* pipeline) override
        {
            pipelines.push_back(pipeline);
        }
        void SetVertexBuffer(uint32, RHIBuffer*, uint64 = 0) override {}
        void SetVertexBuffers(uint32, std::span<RHIBuffer* const>, std::span<const uint64> = {}) override {}
        void SetIndexBuffer(RHIBuffer*, RHIFormat, uint64 = 0) override {}
        void SetDescriptorSet(uint32,
                              RHIDescriptorSet* descriptorSet,
                              std::span<const uint32> = {}) override
        {
            descriptorSets.push_back(descriptorSet);
        }
        void SetPushConstants(const void*, uint32, uint32 = 0) override {}
        void SetViewport(const RHIViewport&) override {}
        void SetViewports(std::span<const RHIViewport>) override {}
        void SetScissor(const RHIRect&) override {}
        void SetScissors(std::span<const RHIRect>) override {}
        void Draw(uint32, uint32 = 1, uint32 = 0, uint32 = 0) override {}
        void DrawIndexed(uint32 indexCount,
                         uint32 instanceCount = 1,
                         uint32 firstIndex = 0,
                         int32 vertexOffset = 0,
                         uint32 firstInstance = 0) override
        {
            ++drawIndexedCalls;
            lastDirectIndexCount = indexCount;
            lastDirectInstanceCount = instanceCount;
            lastDirectFirstIndex = firstIndex;
            lastDirectVertexOffset = vertexOffset;
            lastDirectFirstInstance = firstInstance;
        }

        RHICommandQueueType queueType = RHICommandQueueType::Graphics;
        void DrawIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) override
        {
            ++drawIndexedIndirectCalls;
            lastIndirectBuffer = buffer;
            lastIndirectOffset = offset;
            lastIndirectDrawCount = drawCount;
            lastIndirectStride = stride;
        }
        void DrawIndexedIndirectCount(RHIBuffer* buffer,
                                      uint64 offset,
                                      RHIBuffer* countBuffer,
                                      uint64 countOffset,
                                      uint32 maxDrawCount,
                                      uint32 stride) override
        {
            ++drawIndexedIndirectCountCalls;
            lastIndirectBuffer = buffer;
            lastIndirectOffset = offset;
            lastCountBuffer = countBuffer;
            lastCountOffset = countOffset;
            lastIndirectDrawCount = maxDrawCount;
            lastIndirectStride = stride;
        }
        void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) override
        {
            dispatches.push_back({groupCountX, groupCountY, groupCountZ});
        }
        void DispatchIndirect(RHIBuffer*, uint64) override {}
        void CopyBuffer(RHIBuffer* source,
                        RHIBuffer* destination,
                        uint64 sourceOffset,
                        uint64 destinationOffset,
                        uint64 size) override
        {
            ++copyBufferCalls;
            auto* sourceBuffer = dynamic_cast<FakeBuffer*>(source);
            auto* destinationBuffer = dynamic_cast<FakeBuffer*>(destination);
            if (!sourceBuffer || !destinationBuffer)
            {
                return;
            }

            destinationBuffer->CopyFrom(*sourceBuffer, sourceOffset, destinationOffset, size);
        }
        void CopyTexture(RHITexture*, RHITexture*, const RHITextureCopyDesc& = {}) override {}
        void CopyBufferToTexture(RHIBuffer*, RHITexture*, const RHIBufferTextureCopyDesc&) override {}
        void CopyTextureToBuffer(RHITexture*, RHIBuffer*, const RHIBufferTextureCopyDesc&) override {}
        void BeginQuery(RHIQueryPool*, uint32) override {}
        void EndQuery(RHIQueryPool*, uint32) override {}
        void WriteTimestamp(RHIQueryPool*, uint32) override {}
        void ResolveQueries(RHIQueryPool*, uint32, uint32, RHIBuffer*, uint64) override {}
        void ResetQueries(RHIQueryPool*, uint32, uint32) override {}
        void SetStencilReference(uint32) override {}
        void SetBlendConstants(const float[4]) override {}
        void SetDepthBias(float, float, float = 0.0f) override {}
        void SetDepthBounds(float, float) override {}
        void SetStencilReferenceSeparate(uint32, uint32) override {}
        void SetLineWidth(float) override {}
        void SignalFence(RHIFence*, uint64) override {}
        void WaitFence(RHIFence*, uint64) override {}

        uint32 drawIndexedCalls = 0;
        uint32 drawIndexedIndirectCalls = 0;
        uint32 drawIndexedIndirectCountCalls = 0;
        uint32 lastDirectIndexCount = 0;
        uint32 lastDirectInstanceCount = 0;
        uint32 lastDirectFirstIndex = 0;
        int32 lastDirectVertexOffset = 0;
        uint32 lastDirectFirstInstance = 0;
        RHIBuffer* lastIndirectBuffer = nullptr;
        RHIBuffer* lastCountBuffer = nullptr;
        uint64 lastIndirectOffset = 0;
        uint64 lastCountOffset = 0;
        uint32 lastIndirectDrawCount = 0;
        uint32 lastIndirectStride = 0;
        std::vector<RHIBufferBarrier> bufferBarriers;
        std::vector<std::array<uint32, 3>> dispatches;
        std::vector<RHIPipeline*> pipelines;
        std::vector<RHIDescriptorSet*> descriptorSets;
        uint32 copyBufferCalls = 0;
    };

    class FakeDevice final : public IRHIDevice
    {
    public:
        FakeDevice()
        {
            capabilities.indexedIndirectExecution.supportsFixedCount = true;
            capabilities.indexedIndirectExecution.supportsCountBuffer = true;
            capabilities.supportsIndirectDrawCount = true;
            capabilities.indexedIndirectExecution.supportsFirstInstance = true;
            capabilities.indexedIndirectExecution.requiresExactCommandStride = true;
            capabilities.indexedIndirectExecution.indexedCommandSize =
                sizeof(IndirectDrawIndexedCommand);
            capabilities.indexedIndirectExecution.minCommandStride =
                sizeof(IndirectDrawIndexedCommand);
            capabilities.indexedIndirectExecution.commandStrideAlignment = 4;
            capabilities.indexedIndirectExecution.argumentOffsetAlignment = 4;
            capabilities.indexedIndirectExecution.countOffsetAlignment = 4;
            capabilities.indexedIndirectExecution.maxDrawCount = UINT32_MAX;
            capabilities.indexedIndirectExecution.countValueSize = sizeof(uint32);
        }

        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override
        {
            RHIBufferRef buffer(new FakeBuffer(desc, bufferLifetimeState));
            auto* fakeBuffer = static_cast<FakeBuffer*>(buffer.Get());
            bool retainFailedTransientForInspection = false;
            if (failTransientUploadMap && desc.debugName != nullptr &&
                std::string(desc.debugName) == "GPUCulling.TransientUpload")
            {
                fakeBuffer->SetMapSucceeds(false);
                ++transientUploadMapFailureCount;
                retainFailedTransientForInspection = true;
            }
            else if (failTransientUploadCommit && desc.debugName != nullptr &&
                     std::string(desc.debugName) == "GPUCulling.TransientUpload")
            {
                fakeBuffer->SetCommitSucceeds(false);
                ++transientUploadCommitFailureCount;
                retainFailedTransientForInspection = true;
            }
            createdBuffers.push_back(fakeBuffer);
            if (desc.debugName != nullptr &&
                std::string(desc.debugName).starts_with(
                    "DirectOpaqueRasterQualification."))
            {
                // Keep direct qualification probes alive after their owner
                // releases them so post-fence no-Map assertions do not inspect
                // an expired raw fixture pointer.
                retainedDirectReadbackBuffers.push_back(buffer);
            }
            // Production drops a failed transient immediately. The fixture
            // intentionally retains only injected-failure buffers so exact
            // mapped-range assertions never dereference an expired raw probe.
            if (retainFailedTransientForInspection)
            {
                failedTransientUploadBuffers.push_back(buffer);
            }
            return buffer;
        }

        RHITextureRef CreateTexture(const RHITextureDesc&) override { return {}; }
        RHITextureViewRef CreateTextureView(RHITexture*, const RHITextureViewDesc& = {}) override { return {}; }
        RHISamplerRef CreateSampler(const RHISamplerDesc&) override { return {}; }
        RHIShaderRef CreateShader(const RHIShaderDesc& desc) override
        {
            return m_enablePipelineObjects ? RHIShaderRef(new FakeShader(desc)) : RHIShaderRef{};
        }
        RHIHeapRef CreateHeap(const RHIHeapDesc&) override { return {}; }
        RHITextureRef CreatePlacedTexture(RHIHeap*, uint64, const RHITextureDesc&) override { return {}; }
        RHIBufferRef CreatePlacedBuffer(RHIHeap*, uint64, const RHIBufferDesc&) override { return {}; }
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc& desc) override { return {desc.size, 256}; }
        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(
            const RHIDescriptorSetLayoutDesc& desc) override
        {
            ++createDescriptorSetLayoutCalls;
            return m_enablePipelineObjects
                ? RHIDescriptorSetLayoutRef(new FakeDescriptorSetLayout(desc))
                : RHIDescriptorSetLayoutRef{};
        }
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) override
        {
            return m_enablePipelineObjects
                ? RHIPipelineLayoutRef(new FakePipelineLayout(desc))
                : RHIPipelineLayoutRef{};
        }
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc&) override { return {}; }
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc&) override
        {
            ++createComputePipelineCalls;
            return m_enablePipelineObjects
                ? RHIPipelineRef(new FakeComputePipeline())
                : RHIPipelineRef{};
        }
        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) override
        {
            ++createDescriptorSetCalls;
            return m_enablePipelineObjects
                ? RHIDescriptorSetRef(new FakeDescriptorSet(desc))
                : RHIDescriptorSetRef{};
        }
        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return {}; }
        RHICommandContextRef CreateCommandContext(RHICommandQueueType) override { return RHICommandContextRef(new FakeCommandContext()); }
        uint64 SubmitCommandContext(RHICommandContext*, RHIFence* signalFence = nullptr) override
        {
            return signalFence
                ? static_cast<FakeFence*>(signalFence)->AllocateValue()
                : 0;
        }
        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence* = nullptr) override { return 0; }
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return {}; }
        RHIFenceRef CreateFence(uint64 initialValue = 0) override
        {
            RHIFenceRef fence(new FakeFence(initialValue));
            fences.push_back(fence);
            return fence;
        }
        void WaitForFence(RHIFence*, uint64) override {}
        void WaitIdle() override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        uint32 GetCurrentFrameIndex() const override { return m_currentFrameIndex; }
        void SetCurrentFrameIndex(uint32 frameIndex) { m_currentFrameIndex = frameIndex; }
        RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc&) override { return {}; }
        RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc&) override { return {}; }
        RHIMemoryStats GetMemoryStats() const override { return {}; }
        void BeginResourceGroup(const char*) override {}
        void EndResourceGroup() override {}
        const RHICapabilities& GetCapabilities() const override { return capabilities; }
        RHIBackendType GetBackendType() const override { return backendType; }

        FakeBuffer* FindBuffer(const char* name) const
        {
            for (FakeBuffer* buffer : createdBuffers)
            {
                if (buffer && buffer->GetDebugName() == name)
                    return buffer;
            }
            return nullptr;
        }

        std::vector<RHIBufferRef> retainedDirectReadbackBuffers;

        void EnableTimelineRetirement()
        {
            capabilities.backendType = RHIBackendType::DX12;
            capabilities.adapterName = "GPUDrivenLifetimeFake";
            capabilities.driverVersion = "1";
            capabilities.supportsComputePipeline = true;
            capabilities.supportsDescriptorSets = true;
            capabilities.supportsDynamicDescriptorOffsets = true;
            capabilities.maxDescriptorSets = 4;
            capabilities.supportsExplicitResourceBarriers = true;
            capabilities.supportsDefaultQueueFenceSignal = true;
            capabilities.supportsExplicitQueueFenceSignal = true;
            capabilities.supportsAsyncCompute = true;
            capabilities.dx12.resourceBindingTier = 2;
            capabilities.queueTopology.completionMode = RHIQueueCompletionMode::NativeTimeline;
            capabilities.queueTopology.logicalQueueDomains = {
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Compute,
                GPUQueueDomain::Copy};
            capabilities.queueTopology.activeDomainCount = 3;
        }

        void EnableGPUScenePipelineObjects()
        {
            capabilities.supportsComputePipeline = true;
            capabilities.supportsDescriptorSets = true;
            capabilities.indexedIndirectExecution.supportsCountBuffer = true;
            m_enablePipelineObjects = true;
        }

        FakeFence* GetFence(size_t index) const
        {
            return index < fences.size()
                ? static_cast<FakeFence*>(fences[index].Get())
                : nullptr;
        }

        RHICapabilities capabilities;
        RHIBackendType backendType = RHIBackendType::DX12;
        std::vector<FakeBuffer*> createdBuffers;
        std::vector<RHIBufferRef> failedTransientUploadBuffers;
        std::vector<RHIFenceRef> fences;
        std::shared_ptr<BufferLifetimeState> bufferLifetimeState;
        uint32 createDescriptorSetLayoutCalls = 0;
        uint32 createDescriptorSetCalls = 0;
        uint32 createComputePipelineCalls = 0;
        uint32 transientUploadMapFailureCount = 0;
        bool failTransientUploadCommit = false;
        uint32 transientUploadCommitFailureCount = 0;
        bool failTransientUploadMap = false;

    private:
        uint32 m_currentFrameIndex = 0;
        bool m_enablePipelineObjects = false;
    };

    class FakeEncodedCommandBuffer final : public RHIEncodedCommandBuffer
    {
    public:
        explicit FakeEncodedCommandBuffer(RHIBackendType backendType)
            : m_backendType(backendType)
        {
        }

        RHIBackendType GetBackendType() const override { return m_backendType; }

    private:
        RHIBackendType m_backendType = RHIBackendType::None;
    };

    class FakeEncodedSubmissionStrategy final : public IRenderSubmissionStrategy
    {
    public:
        RenderSubmissionResult Submit(
            RHICommandContext&,
            const RenderSubmissionRequest& request) const override
        {
            RenderSubmissionResult result;
            if (request.kind != RenderSubmissionKind::EncodedCommandBuffer)
            {
                result.validationCode =
                    RenderSubmissionValidationCode::UnsupportedSubmissionKind;
                return result;
            }
            if (request.encodedCommandBuffer.commandBuffer == nullptr ||
                request.encodedCommandBuffer.commandBuffer->GetBackendType() !=
                    RHIBackendType::Metal)
            {
                result.validationCode = RenderSubmissionValidationCode::InvalidRequest;
                return result;
            }
            result.recorded = true;
            result.submittedDrawUpperBound = 1;
            return result;
        }
    };

    RenderSubmissionResult RecordIndexedIndirectSubmission(
        FakeCommandContext& context,
        const GPUCullingIndexedIndirectSubmission& cullingSubmission)
    {
        RenderSubmissionRequest request;
        request.kind = RenderSubmissionKind::IndexedIndirect;
        request.indexedIndirect = cullingSubmission.execution;
        request.capabilities = cullingSubmission.capabilities;
        const IndexedIndirectRenderSubmissionStrategy strategy;
        return strategy.Submit(context, request);
    }

    template <typename T>
    T ReadBufferValue(const FakeBuffer& buffer, size_t index = 0)
    {
        T value{};
        const size_t offset = index * sizeof(T);
        EXPECT_LE(offset + sizeof(T), buffer.GetStorage().size());
        if (offset + sizeof(T) <= buffer.GetStorage().size())
        {
            std::memcpy(&value, buffer.GetStorage().data() + offset, sizeof(T));
        }
        return value;
    }

    GPUSceneResidentGraphLease MakeGPUSceneLease(
        FakeDevice& device,
        uint64 version,
        const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT>& capacities)
    {
        const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> rowStrides{
            sizeof(GPUScenePrimitiveRow),
            sizeof(GPUSceneBoundsRow),
            sizeof(GPUSceneTransformRow),
            sizeof(GPUSceneMaterialRow),
            sizeof(GPUSceneGeometryRow),
            sizeof(GPUSceneDrawMetadataRow)};
        GPUSceneResidentGraphLease lease;
        lease.version = version;
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_RESIDENT_TABLE_COUNT;
             ++tableIndex)
        {
            RHIBufferDesc desc;
            desc.size =
                static_cast<uint64>(capacities[tableIndex]) * rowStrides[tableIndex];
            desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
            desc.memoryType = RHIMemoryType::Upload;
            desc.stride = rowStrides[tableIndex];
            desc.debugName = "GPUDrivenValidation.GPUSceneLeaseTable";
            lease.buffers[tableIndex] = device.CreateBuffer(desc);
            lease.handles[tableIndex].index = tableIndex;
            lease.capacities[tableIndex] = capacities[tableIndex];
        }
        return lease;
    }

    Mat4 TestView()
    {
        return lookAt(Vec3(0.0f, 0.0f, 0.0f),
                      Vec3(0.0f, 0.0f, -1.0f),
                      Vec3(0.0f, 1.0f, 0.0f));
    }

    Mat4 TestProjection()
    {
        return perspective(radians(60.0f), 1.0f, 0.1f, 200.0f);
    }

    GPUInstanceData MakeInstance(const Vec3& center, float radius, uint32 indexCount)
    {
        GPUInstanceData instance = {};
        instance.worldMatrix = Mat4(1.0f);
        instance.boundingSphere = Vec4(center, radius);
        instance.aabbMin = Vec4(center - Vec3(radius), 0.0f);
        instance.aabbMax = Vec4(center + Vec3(radius), 0.0f);
        instance.indexCount = indexCount;
        instance.firstIndex = 7;
        instance.vertexOffset = -2;
        return instance;
    }

    class RasterSemanticRegistry final
    {
    public:
        RasterSemanticRegistry()
            : m_statusTable(1024)
        {
            EXPECT_TRUE(m_registry.Initialize(&m_statusTable, &m_retirement));
        }

        ~RasterSemanticRegistry()
        {
            m_registry.Shutdown();
        }

        [[nodiscard]] bool AddReadyMesh(RenderResourceHandle handle,
                                        AssetId assetId)
        {
            return BeginReady(handle, assetId, RenderResourceKind::Mesh);
        }

        [[nodiscard]] bool AddReadyTexture(RenderResourceHandle handle,
                                           AssetId assetId)
        {
            return BeginReady(handle, assetId, RenderResourceKind::Texture);
        }

        [[nodiscard]] bool AddReadyMaterial(
            RenderResourceHandle handle,
            AssetId assetId,
            const MaterialSourceData& source,
            const std::vector<MaterialUploadTextureBinding>& bindings)
        {
            if (!BeginUploading(handle) ||
                !m_registry.BeginPending(handle,
                                         RenderResourceKind::Material,
                                         {},
                                         assetId) ||
                !m_registry.SetPendingMaterialMetadata(
                    handle, MaterialUploadPayload{source, bindings}) ||
                !m_registry.Commit(handle))
            {
                return false;
            }
            return PublishReady(handle, RenderResourcePublicState::Uploading);
        }

        [[nodiscard]] bool BeginMaterialReplacement(
            RenderResourceHandle handle,
            AssetId assetId,
            uint64 sourceRevision)
        {
            const PackedRenderResourceStatus ready{
                handle.generation,
                RenderResourcePublicState::GPUReady,
                RenderResourceFailureCode::None};
            const PackedRenderResourceStatus queued{
                handle.generation,
                RenderResourcePublicState::ReplacementQueued,
                RenderResourceFailureCode::None};
            const PackedRenderResourceStatus replacing{
                handle.generation,
                RenderResourcePublicState::Replacing,
                RenderResourceFailureCode::None};
            return m_statusTable.CompareExchange(
                       handle, ready, queued, RenderStatusWriter::Update) &&
                m_statusTable.CompareExchange(
                    handle, queued, replacing, RenderStatusWriter::Render) &&
                m_registry.BeginPending(handle,
                                        RenderResourceKind::Material,
                                        {},
                                        assetId,
                                        RenderResourceContentOperation::Replace,
                                        sourceRevision);
        }

        [[nodiscard]] const RenderResourceRegistry& GetRegistry() const noexcept
        {
            return m_registry;
        }

    private:
        [[nodiscard]] bool BeginReady(RenderResourceHandle handle,
                                      AssetId assetId,
                                      RenderResourceKind kind)
        {
            if (!BeginUploading(handle) ||
                !m_registry.BeginPending(handle, kind, {}, assetId) ||
                !m_registry.Commit(handle))
            {
                return false;
            }
            return PublishReady(handle, RenderResourcePublicState::Uploading);
        }

        [[nodiscard]] bool BeginUploading(RenderResourceHandle handle)
        {
            if (!handle.IsValid())
            {
                return false;
            }
            for (uint32 generation = 1; generation <= handle.generation;
                 ++generation)
            {
                const PackedRenderResourceStatus released{
                    generation - 1U,
                    RenderResourcePublicState::Released,
                    RenderResourceFailureCode::None};
                const PackedRenderResourceStatus reserved{
                    generation,
                    RenderResourcePublicState::Reserved,
                    RenderResourceFailureCode::None};
                if (!m_statusTable.CompareExchange(
                        {handle.slot, generation - 1U},
                        released,
                        reserved,
                        RenderStatusWriter::Update))
                {
                    return false;
                }
                if (generation != handle.generation)
                {
                    const PackedRenderResourceStatus evicting{
                        generation,
                        RenderResourcePublicState::Evicting,
                        RenderResourceFailureCode::None};
                    const PackedRenderResourceStatus nextReleased{
                        generation,
                        RenderResourcePublicState::Released,
                        RenderResourceFailureCode::None};
                    if (!m_statusTable.CompareExchange(
                            {handle.slot, generation},
                            reserved,
                            evicting,
                            RenderStatusWriter::Update) ||
                        !m_statusTable.CompareExchange(
                            {handle.slot, generation},
                            evicting,
                            nextReleased,
                            RenderStatusWriter::Render))
                    {
                        return false;
                    }
                    continue;
                }

                const PackedRenderResourceStatus queued{
                    generation,
                    RenderResourcePublicState::UploadQueued,
                    RenderResourceFailureCode::None};
                const PackedRenderResourceStatus uploading{
                    generation,
                    RenderResourcePublicState::Uploading,
                    RenderResourceFailureCode::None};
                return m_statusTable.CompareExchange(
                           handle, reserved, queued, RenderStatusWriter::Update) &&
                    m_statusTable.CompareExchange(
                        handle, queued, uploading, RenderStatusWriter::Render);
            }
            return false;
        }

        [[nodiscard]] bool PublishReady(
            RenderResourceHandle handle,
            RenderResourcePublicState expectedState)
        {
            return m_statusTable.CompareExchange(
                handle,
                {handle.generation,
                 expectedState,
                 RenderResourceFailureCode::None},
                {handle.generation,
                 RenderResourcePublicState::GPUReady,
                 RenderResourceFailureCode::None},
                RenderStatusWriter::Render);
        }

        RenderResourceStatusTable m_statusTable;
        RenderRetirementQueue m_retirement;
        RenderResourceRegistry m_registry;
    };

    RenderObject MakeRenderObject(const Vec3& center, float extent, uint64 meshId)
    {
        RenderObject object;
        object.worldMatrix = Mat4(1.0f);
        object.worldMatrix[3] = Vec4(center, 1.0f);
        object.bounds = AABB(center - Vec3(extent), center + Vec3(extent));
        object.mesh = {static_cast<uint32>(meshId), 1};
        object.material = {static_cast<uint32>(meshId + 1000u), 1};
        object.drawable = true;
        object.visible = true;
        return object;
    }

    RenderDrawItem MakeDrawItem(uint32 objectIndex, uint64 meshId, uint64 materialId)
    {
        RenderDrawItem item;
        item.objectIndex = objectIndex;
        item.submeshIndex = 0;
        item.mesh = {static_cast<uint32>(meshId), 1};
        item.material = {static_cast<uint32>(materialId), 1};
        item.renderMode = MaterialRenderMode::Opaque;
        return item;
    }

    GPUSceneCullingCandidate MakeCanonicalGPUSceneCandidate(
        GPUSceneCullingCandidate candidate,
        uint32 residentRow)
    {
        // The canonical Tier 2 stream is indexed by its stable resident row.
        // Draw-group placement is deliberately supplied by the active-row
        // indirection, so a packet reorder cannot dirty this payload.
        candidate.drawGroupIndex = RVX_INVALID_INDEX;
        candidate.drawGroupVisibleOffset = 0u;
        candidate.rasterInstanceIndex = residentRow;
        return candidate;
    }

    std::filesystem::path FindWorkspaceRoot()
    {
        std::filesystem::path path = std::filesystem::current_path();
        for (int i = 0; i < 8; ++i)
        {
            if (std::filesystem::exists(path / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp"))
            {
                return path;
            }

            if (!path.has_parent_path())
            {
                break;
            }
            path = path.parent_path();
        }

        return {};
    }

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return {};
        }

        const std::istreambuf_iterator<char> begin(file);
        const std::istreambuf_iterator<char> end;
        std::string contents(begin, end);
        contents.erase(
            std::remove(contents.begin(), contents.end(), '\r'),
            contents.end());
        return contents;
    }
} // namespace

bool RVX::GPUCullingQualificationTestAccess::
    FinalizeTierOneEvidenceForExpectedReadback(GPUCulling& culling)
{
    GPUCulling::GPUSceneQualificationCapture& capture =
        culling.m_gpuSceneQualificationCapture;
    if (capture.capturedTier != GPUDrivenTier::IndirectGrouped ||
        capture.rasterSemanticEvidenceFinalized)
    {
        return capture.capturedTier != GPUDrivenTier::IndirectGrouped ||
            capture.rasterSemanticEvidenceFinalized;
    }
    if (!capture.referencePrepared || capture.drawGroups.empty() ||
        capture.drawGroups.size() != capture.drawGroupCount ||
        capture.cpuActiveInstances.size() != capture.activeRowCount ||
        capture.cpuActiveRasterSemanticIdentities.size() !=
            capture.activeRowCount)
    {
        return false;
    }

    // Generic Tier1 unit fixtures do not own an Opaque material binding. Give
    // their frozen topology a real, ready mesh and use a nonzero resolved-key
    // surrogate so the production combiner remains the only code that can
    // finalize diagnostic evidence.
    RasterSemanticRegistry registry;
    const RenderResourceHandle mesh{801u, 1u};
    if (!registry.AddReadyMesh(mesh, AssetId{0xE001u}))
    {
        return false;
    }
    std::vector<uint64> fixedKeys(capture.drawGroupCount, 0);
    for (uint32 groupIndex = 0; groupIndex < capture.drawGroupCount;
         ++groupIndex)
    {
        capture.drawGroups[groupIndex].mesh = mesh;
        capture.drawGroups[groupIndex].materialParameterSlotConsumed = false;
        fixedKeys[groupIndex] = 0xE1000000ull + groupIndex;
        for (uint32 activeIndex = 0; activeIndex < capture.activeRowCount;
             ++activeIndex)
        {
            if (capture.cpuActiveInstances[activeIndex].drawGroupIndex ==
                    groupIndex &&
                capture.cpuActiveRasterSemanticIdentities[activeIndex] != 0)
            {
                fixedKeys[groupIndex] =
                    capture.cpuActiveRasterSemanticIdentities[activeIndex];
                break;
            }
        }
    }
    return culling.FinalizeTierOneRasterSemanticEvidence(
        fixedKeys, {}, registry.GetRegistry());
}

class GPUDrivenValidationFixture : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        Log::Initialize();
    }

    static void TearDownTestSuite()
    {
        Log::Shutdown();
    }
};

TEST_F(GPUDrivenValidationFixture, DX12QualificationIsCandidateUntilProductionGatesClose)
{
    const GPUDrivenBackendQualification qualification =
        GetGPUDrivenBackendQualification(RHIBackendType::DX12);

    EXPECT_EQ(RVX_GPU_DRIVEN_QUALIFICATION_SCHEMA_VERSION,
              qualification.schemaVersion);
    EXPECT_EQ(RHIBackendType::DX12, qualification.backend);
    EXPECT_EQ(2u, qualification.revision);
    EXPECT_EQ(GPUDrivenQualificationLevel::Candidate,
              qualification.GetLevel());
    EXPECT_FALSE(qualification.IsQualified());
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::RHIContractConformance));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::MultiBatchMaterialRouting));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::GPUBasedValidation));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::CrossPathImageParity));
    EXPECT_FALSE(qualification.HasPassed(
        GPUDrivenQualificationGate::RealAssetRegression));
    EXPECT_FALSE(qualification.HasPassed(
        GPUDrivenQualificationGate::AdapterDriverMatrix));

    const uint64 expectedMissingGateMask =
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::RealAssetRegression) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::AdapterDriverMatrix);
    EXPECT_EQ(expectedMissingGateMask, qualification.GetMissingGateMask());
}

TEST_F(GPUDrivenValidationFixture, VulkanQualificationIsCandidateAndAutoRemainsDirect)
{
    const GPUDrivenBackendQualification qualification =
        GetGPUDrivenBackendQualification(RHIBackendType::Vulkan);
    EXPECT_EQ(2u, qualification.revision);
    EXPECT_EQ(GPUDrivenQualificationLevel::Candidate,
              qualification.GetLevel());
    EXPECT_FALSE(qualification.IsQualified());
    const uint64 expectedPassedGateMask =
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::RHIContractConformance) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::ShaderPipelineContracts) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::DescriptorIntegrity) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::ResourceStateValidation) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::IndirectExecutionSmoke) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::DirectFallbackSmoke) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::RepeatedFrameResize) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::CrossPathImageParity);
    EXPECT_EQ(expectedPassedGateMask, qualification.passedGateMask);
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::RHIContractConformance));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::ShaderPipelineContracts));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::IndirectExecutionSmoke));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::RepeatedFrameResize));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::CrossPathImageParity));
    EXPECT_FALSE(qualification.HasPassed(
        GPUDrivenQualificationGate::GPUBasedValidation));

    GPUDrivenPolicyInput input;
    input.requestedMode = RenderGPUDrivenMode::Auto;
    input.backend = RHIBackendType::Vulkan;
    input.supportsComputePipeline = true;
    input.supportsDescriptorSets = true;
    input.indexedIndirectExecution.supportsCountBuffer = true;
    input.pipelineReady = true;
    const GPUDrivenPolicyDecision decision = ResolveGPUDrivenPolicy(input);
    EXPECT_FALSE(decision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::BackendNotQualified, decision.reason);
}

TEST_F(GPUDrivenValidationFixture,
       GPUCullingUsesSemanticCapabilitiesInsteadOfADx12BackendGate)
{
    FakeDevice device;
    device.backendType = RHIBackendType::Vulkan;
    device.capabilities.backendType = RHIBackendType::Vulkan;
    device.EnableGPUScenePipelineObjects();

    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    const GPUCullingExecutionDecision decision = culling.GetExecutionDecision();
    EXPECT_NE(decision.fallbackReason,
              GPUCullingFallbackReason::ShaderBackendUnsupported);
}

TEST_F(GPUDrivenValidationFixture, RemainingBackendsRemainUnqualified)
{
    for (RHIBackendType backend : {
             RHIBackendType::Metal,
             RHIBackendType::DX11,
             RHIBackendType::OpenGL,
             RHIBackendType::Auto,
             RHIBackendType::None})
    {
        const GPUDrivenBackendQualification qualification =
            GetGPUDrivenBackendQualification(backend);
        EXPECT_EQ(GPUDrivenQualificationLevel::Unqualified,
                  qualification.GetLevel());
        EXPECT_FALSE(qualification.IsQualified());
        EXPECT_EQ(0u, qualification.revision);
        EXPECT_EQ(0u, qualification.passedGateMask);
        EXPECT_EQ(RVX_GPU_DRIVEN_REQUIRED_QUALIFICATION_GATE_MASK,
                  qualification.GetMissingGateMask());
    }
}

TEST_F(GPUDrivenValidationFixture, QualificationIsDerivedAndMalformedRecordsFailClosed)
{
    GPUDrivenBackendQualification qualification =
        GetGPUDrivenBackendQualification(RHIBackendType::DX12);
    qualification.revision = 3;
    qualification.passedGateMask = qualification.requiredGateMask;

    EXPECT_TRUE(qualification.IsValidManifest());
    EXPECT_TRUE(qualification.IsQualified());
    EXPECT_EQ(GPUDrivenQualificationLevel::Qualified,
              qualification.GetLevel());

    qualification.passedGateMask &= ~GetGPUDrivenQualificationGateMask(
        GPUDrivenQualificationGate::RealAssetRegression);
    EXPECT_TRUE(qualification.IsValidManifest());
    EXPECT_FALSE(qualification.IsQualified());
    EXPECT_EQ(GPUDrivenQualificationLevel::Candidate,
              qualification.GetLevel());

    qualification.passedGateMask |= 1ull << 63;
    EXPECT_FALSE(qualification.IsValidManifest());
    EXPECT_FALSE(qualification.IsQualified());
    EXPECT_EQ(GPUDrivenQualificationLevel::Unqualified,
              qualification.GetLevel());

    qualification.passedGateMask = qualification.requiredGateMask;
    ++qualification.schemaVersion;
    EXPECT_FALSE(qualification.IsValidManifest());
    EXPECT_FALSE(qualification.IsQualified());
    EXPECT_EQ(GPUDrivenQualificationLevel::Unqualified,
              qualification.GetLevel());
}

TEST_F(GPUDrivenValidationFixture, AutoModeRequiresAQualifiedBackend)
{
    GPUDrivenPolicyInput input;
    input.requestedMode = RenderGPUDrivenMode::Auto;
    input.backend = RHIBackendType::DX12;
    input.supportsComputePipeline = true;
    input.supportsDescriptorSets = true;
    input.indexedIndirectExecution.supportsCountBuffer = true;
    input.pipelineReady = true;

    const GPUDrivenPolicyDecision decision = ResolveGPUDrivenPolicy(input);
    EXPECT_FALSE(decision.enabled);
    EXPECT_FALSE(decision.backendQualified);
    EXPECT_EQ(GPUDrivenQualificationLevel::Candidate,
              decision.qualificationLevel);
    EXPECT_EQ(2u, decision.qualificationRevision);
    EXPECT_NE(0u, decision.passedQualificationGateMask);
    EXPECT_NE(0u, decision.missingQualificationGateMask);
    EXPECT_TRUE(decision.capabilitiesReady);
    EXPECT_TRUE(decision.pipelineReady);
    EXPECT_EQ(GPUDrivenPolicyReason::BackendNotQualified, decision.reason);
}

TEST_F(GPUDrivenValidationFixture, ForceEnabledBypassesQualificationButNotCapabilities)
{
    GPUDrivenPolicyInput input;
    input.requestedMode = RenderGPUDrivenMode::ForceEnabled;
    input.backend = RHIBackendType::DX12;
    input.supportsComputePipeline = true;
    input.supportsDescriptorSets = true;
    input.indexedIndirectExecution.supportsCountBuffer = true;
    input.pipelineReady = true;

    GPUDrivenPolicyDecision decision = ResolveGPUDrivenPolicy(input);
    EXPECT_TRUE(decision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::None, decision.reason);

    input.indexedIndirectExecution.supportsCountBuffer = false;
    decision = ResolveGPUDrivenPolicy(input);
    EXPECT_FALSE(decision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::IndirectDrawCountUnsupported, decision.reason);
}

TEST_F(GPUDrivenValidationFixture, ForceDisabledAlwaysSelectsDirectRendering)
{
    GPUDrivenPolicyInput input;
    input.requestedMode = RenderGPUDrivenMode::ForceDisabled;
    input.backend = RHIBackendType::DX12;
    input.supportsComputePipeline = true;
    input.supportsDescriptorSets = true;
    input.indexedIndirectExecution.supportsCountBuffer = true;
    input.pipelineReady = true;

    const GPUDrivenPolicyDecision decision = ResolveGPUDrivenPolicy(input);
    EXPECT_FALSE(decision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::ForcedDisabled, decision.reason);
}

TEST_F(GPUDrivenValidationFixture, InvalidModeFailsClosed)
{
    GPUDrivenPolicyInput input;
    input.requestedMode = static_cast<RenderGPUDrivenMode>(0xFFU);
    input.backend = RHIBackendType::DX12;
    input.supportsComputePipeline = true;
    input.supportsDescriptorSets = true;
    input.indexedIndirectExecution.supportsCountBuffer = true;
    input.pipelineReady = true;

    const GPUDrivenPolicyDecision decision = ResolveGPUDrivenPolicy(input);
    EXPECT_FALSE(decision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::InvalidMode, decision.reason);
}

TEST_F(GPUDrivenValidationFixture, InjectedQualificationMatchesLegacyAndRejectsMalformedEvidence)
{
    GPUDrivenPolicyInput input;
    input.requestedMode = RenderGPUDrivenMode::Auto;
    input.backend = RHIBackendType::DX12;
    input.supportsComputePipeline = true;
    input.supportsDescriptorSets = true;
    input.indexedIndirectExecution.supportsCountBuffer = true;
    input.pipelineReady = true;

    const GPUDrivenBackendQualification qualification =
        GetGPUDrivenBackendQualification(input.backend);
    const GPUDrivenPolicyDecision legacy = ResolveGPUDrivenPolicy(input);
    const GPUDrivenPolicyDecision injected = ResolveGPUDrivenPolicy(
        input, qualification);
    EXPECT_EQ(legacy.enabled, injected.enabled);
    EXPECT_EQ(legacy.reason, injected.reason);
    EXPECT_EQ(legacy.qualificationLevel, injected.qualificationLevel);
    EXPECT_EQ(legacy.passedQualificationGateMask,
              injected.passedQualificationGateMask);

    input.requestedMode = RenderGPUDrivenMode::ForceEnabled;
    GPUDrivenBackendQualification malformed = qualification;
    malformed.schemaVersion++;
    const GPUDrivenPolicyDecision malformedDecision = ResolveGPUDrivenPolicy(
        input, malformed);
    EXPECT_FALSE(malformedDecision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::QualificationInvalid,
              malformedDecision.reason);
}

TEST_F(GPUDrivenValidationFixture, CpuFallbackCullsInstancesAndBuildsIndirectCommands)
{
    FakeDevice device;

    GPUCullingConfig config;
    config.maxInstances = 8;
    config.enableDistanceCulling = true;
    config.maxDrawDistance = 20.0f;

    GPUCulling culling;
    culling.Initialize(&device, config);
    ASSERT_TRUE(culling.IsInitialized());

    culling.BeginFrame();
    EXPECT_EQ(0u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    EXPECT_EQ(1u, culling.AddInstance(MakeInstance(Vec3(100.0f, 0.0f, -5.0f), 1.0f, 12)));
    EXPECT_EQ(2u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -100.0f), 1.0f, 24)));
    EXPECT_EQ(3u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -6.0f), 1.0f, 0)));
    culling.EndFrame();
    EXPECT_EQ(0U,
              culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(0U, culling.GetLastGPUSceneCandidateUploadBytes());

    FakeCommandContext ctx;
    culling.Cull(ctx, TestView(), TestProjection());

    EXPECT_TRUE(culling.WasCpuFallbackUsedLastCull());
    EXPECT_EQ(1u, culling.GetDrawCount());
    ASSERT_EQ(1u, culling.GetVisibleInstanceIndices().size());
    EXPECT_EQ(0u, culling.GetVisibleInstanceIndices()[0]);

    const GPUCulling::Statistics stats = culling.GetStatistics();
    EXPECT_EQ(4u, stats.totalInstances);
    EXPECT_EQ(1u, stats.visibleInstances);
    EXPECT_EQ(1u, stats.frustumCulled);
    EXPECT_EQ(1u, stats.distanceCulled);
    EXPECT_EQ(0u, stats.occlusionCulled);

    ASSERT_EQ(4u, culling.GetIndirectCommands().size());
    const IndirectDrawIndexedCommand& command = culling.GetIndirectCommands()[0];
    EXPECT_EQ(36u, command.indexCount);
    EXPECT_EQ(1u, command.instanceCount);
    EXPECT_EQ(7u, command.firstIndex);
    EXPECT_EQ(-2, command.vertexOffset);
    EXPECT_EQ(0u, command.firstInstance);

    const FakeBuffer* drawCountBuffer = device.FindBuffer("GPUCulling.DrawCountBuffer");
    ASSERT_NE(nullptr, drawCountBuffer);
    EXPECT_EQ(1u, ReadBufferValue<uint32>(*drawCountBuffer));

    const FakeBuffer* visibleBuffer = device.FindBuffer("GPUCulling.VisibleInstanceBuffer");
    ASSERT_NE(nullptr, visibleBuffer);
    EXPECT_EQ(0u, ReadBufferValue<uint32>(*visibleBuffer));

    const FakeBuffer* indirectBuffer = device.FindBuffer("GPUCulling.IndirectDrawBuffer");
    ASSERT_NE(nullptr, indirectBuffer);
    const IndirectDrawIndexedCommand uploadedCommand =
        ReadBufferValue<IndirectDrawIndexedCommand>(*indirectBuffer);
    EXPECT_EQ(command.indexCount, uploadedCommand.indexCount);
    EXPECT_EQ(command.firstInstance, uploadedCommand.firstInstance);

    const GPUCullingIndexedIndirectSubmission cullingSubmission =
        culling.BuildIndexedIndirectGroupSubmission(0);
    EXPECT_EQ(RHIIndirectExecutionMode::FixedCount,
              cullingSubmission.execution.mode);
    EXPECT_EQ(culling.GetIndirectBuffer(),
              cullingSubmission.execution.argumentBuffer);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand),
              cullingSubmission.execution.commandStride);
    EXPECT_TRUE(cullingSubmission.execution.requiresFirstInstance);
    const RenderSubmissionResult submission =
        RecordIndexedIndirectSubmission(ctx, cullingSubmission);
    EXPECT_TRUE(submission.recorded);
    EXPECT_EQ(1u, submission.submittedDrawUpperBound);
    EXPECT_TRUE(submission.executedDrawCountAvailable);
    EXPECT_EQ(1u, submission.executedDrawCount);
    EXPECT_EQ(1u, ctx.drawIndexedIndirectCalls);
    EXPECT_EQ(culling.GetIndirectBuffer(), ctx.lastIndirectBuffer);
    EXPECT_EQ(0u, ctx.lastIndirectOffset);
    EXPECT_EQ(1u, ctx.lastIndirectDrawCount);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand), ctx.lastIndirectStride);
}

TEST_F(GPUDrivenValidationFixture,
       CpuFallbackCullingIsConservativeRelativeToCanonicalVisibilityAtFrustumEdges)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 3;
    config.enableDistanceCulling = false;

    const Mat4 view = TestView();
    Mat4 nonUniformProjection = TestProjection();
    nonUniformProjection[0] *= 1.75f;
    Mat4 degenerateFarProjection = nonUniformProjection;
    for (uint32 column = 0; column < 4; ++column)
    {
        degenerateFarProjection[column][2] =
            degenerateFarProjection[column][3];
    }

    for (const Mat4& projection :
         {TestProjection(), nonUniformProjection, degenerateFarProjection})
    {
        const Mat4 viewProjection = projection * view;
        const RenderVisibilityFrustum canonicalFrustum =
            RenderVisibilityFrustum::FromViewProjection(viewProjection);

        const Vec3 rightEdgeExtent(0.12f, 0.08f, 0.08f);
        const Vec4& rightPlane = canonicalFrustum.planes[1];
        const float32 rightProjectedRadius = dot(
            glm::abs(Vec3(rightPlane)), rightEdgeExtent);
        const float32 rightPlaneMagnitude = std::abs(rightPlane.z * -5.0f) +
            std::abs(rightPlane.w) + rightProjectedRadius;
        const float32 rightInsideMargin = 4.0f *
            std::numeric_limits<float32>::epsilon() *
            std::max(1.0f, rightPlaneMagnitude);
        ASSERT_NE(0.0f, rightPlane.x);
        const float32 rightEdgeCenterX =
            (-rightProjectedRadius + rightInsideMargin -
             rightPlane.z * -5.0f - rightPlane.w) /
            rightPlane.x;

        // The right-edge box touches the canonical side plane. The near-edge
        // box also exercises the reverse/degenerated far-plane convention.
        const std::array<AABB, 2> bounds{
            AABB(Vec3(rightEdgeCenterX, 0.0f, -5.0f) - rightEdgeExtent,
                 Vec3(rightEdgeCenterX, 0.0f, -5.0f) + rightEdgeExtent),
            AABB(Vec3(-0.08f, -0.08f, -0.101f),
                 Vec3(0.08f, 0.08f, -0.099f))};

        GPUCulling culling;
        culling.Initialize(&device, config);
        ASSERT_TRUE(culling.IsInitialized());
        culling.BeginFrame();
        uint32 canonicalVisibleCount = 0;
        for (uint32 index = 0; index < bounds.size(); ++index)
        {
            bool invalidBounds = false;
            const bool canonicalVisible = IsRenderVisibilityAABBVisible(
                bounds[index], canonicalFrustum, &invalidBounds);
            ASSERT_FALSE(invalidBounds);
            if (canonicalVisible)
            {
                ++canonicalVisibleCount;
            }

            GPUInstanceData instance{};
            instance.worldMatrix = Mat4Identity();
            instance.normalMatrix = Mat4Identity();
            instance.boundingSphere = Vec4(
                bounds[index].GetCenter(), length(bounds[index].GetExtent()));
            instance.aabbMin = Vec4(bounds[index].GetMin(), 0.0f);
            instance.aabbMax = Vec4(bounds[index].GetMax(), 0.0f);
            instance.indexCount = 3;
            EXPECT_EQ(index, culling.AddInstance(instance));
        }
        culling.EndFrame();

        culling.CullCpuFallback(view, projection);
        EXPECT_EQ(canonicalVisibleCount,
                  culling.GetStatistics().visibleInstances);
    }
}

TEST_F(GPUDrivenValidationFixture,
       RasterInstanceStreamReportsExactRangeReceiptsAndAtomEvidence)
{
    FakeDevice device;
    std::array<GPUInstanceData, 3> instances{};
    const std::array<RasterInstanceStreamKey, 3> keys{{{1, 0}, {2, 0}, {3, 0}}};
    RasterInstanceStream stream;

    ASSERT_TRUE(CreateRasterInstanceStream(
        device, instances, keys, "RasterUploadDiagnostics", stream));
    ASSERT_TRUE(stream.IsValid());
    EXPECT_EQ(3U, stream.instanceCount);
    EXPECT_EQ(3U * sizeof(GPUInstanceData), stream.instanceUploadBytes);
    EXPECT_EQ(3U * sizeof(uint32), stream.indexUploadBytes);
    EXPECT_EQ(1U, stream.instanceUploadWork.mappedRangeCount);
    EXPECT_EQ(1U, stream.instanceUploadWork.committedRangeCount);
    EXPECT_EQ(3U * sizeof(GPUInstanceData),
              stream.instanceUploadWork.committedPayloadBytes);
    ASSERT_TRUE(stream.instanceUploadWork.hostVisibilitySynchronizedBytes.IsAvailable());
    EXPECT_EQ(3U * sizeof(GPUInstanceData),
              *stream.instanceUploadWork.hostVisibilitySynchronizedBytes.GetValue());

    auto* const firstInstanceBuffer =
        static_cast<FakeBuffer*>(stream.instances.Get());
    auto* const firstIndexBuffer =
        static_cast<FakeBuffer*>(stream.instanceIndices.Get());
    ASSERT_NE(nullptr, firstInstanceBuffer);
    ASSERT_NE(nullptr, firstIndexBuffer);
    ASSERT_EQ(1U, firstInstanceBuffer->GetMappedRanges().size());
    EXPECT_EQ(0U, firstInstanceBuffer->GetMappedRanges()[0].first);
    EXPECT_EQ(3U * sizeof(GPUInstanceData),
              firstInstanceBuffer->GetMappedRanges()[0].second);
    ASSERT_EQ(1U, firstIndexBuffer->GetMappedRanges().size());
    EXPECT_EQ(0U, firstIndexBuffer->GetMappedRanges()[0].first);
    EXPECT_EQ(3U * sizeof(uint32),
              firstIndexBuffer->GetMappedRanges()[0].second);

    firstInstanceBuffer->SetSynchronization(
        RHIHostWriteSynchronization::AtomAlignedRange);
    instances[0].indexCount = 3;
    instances[2].indexCount = 9;
    ASSERT_TRUE(CreateRasterInstanceStream(
        device, instances, keys, "RasterUploadDiagnostics", stream));
    EXPECT_EQ(firstInstanceBuffer, stream.instances.Get());
    EXPECT_EQ(firstIndexBuffer, stream.instanceIndices.Get());
    EXPECT_EQ(2U * sizeof(GPUInstanceData), stream.instanceUploadBytes);
    EXPECT_EQ(0U, stream.indexUploadBytes);
    EXPECT_EQ(2U, stream.instanceUploadWork.mappedRangeCount);
    EXPECT_EQ(2U, stream.instanceUploadWork.committedRangeCount);
    ASSERT_TRUE(stream.instanceUploadWork.hostVisibilitySynchronizedBytes.IsAvailable());
    EXPECT_EQ(512U,
              *stream.instanceUploadWork.hostVisibilitySynchronizedBytes.GetValue());
    EXPECT_EQ(2U, stream.instanceUploadWork
                     .hostVisibilitySynchronizationScopeRangeCounts[3]);
    ASSERT_EQ(3U, firstInstanceBuffer->GetMappedRanges().size());
    EXPECT_EQ(0U, firstInstanceBuffer->GetMappedRanges()[1].first);
    EXPECT_EQ(sizeof(GPUInstanceData),
              firstInstanceBuffer->GetMappedRanges()[1].second);
    EXPECT_EQ(2U * sizeof(GPUInstanceData),
              firstInstanceBuffer->GetMappedRanges()[2].first);
    EXPECT_EQ(sizeof(GPUInstanceData),
              firstInstanceBuffer->GetMappedRanges()[2].second);
}

TEST_F(GPUDrivenValidationFixture,
       RasterInstanceStreamTranscriptUsesActualResidentOrderAndConsumedPayload)
{
    FakeDevice device;
    RasterInstanceStreamCache cache;
    RasterInstanceStream stream;
    std::array<GPUInstanceData, 3> instances{{
        MakeInstance(Vec3(2.0f, 0.0f, -5.0f), 1.0f, 36u),
        MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 36u),
        MakeInstance(Vec3(3.0f, 0.0f, -5.0f), 1.0f, 12u)}};
    const std::array<RasterInstanceStreamKey, 3> keys{{
        {20u, 3u}, {10u, 3u}, {30u, 7u}}};
    const std::array<uint64, 3> semanticIdentities{{
        0xD1CE000000000001ull, 0xD1CE000000000002ull, 0xD1CE000000000003ull}};
    instances[0].meshId = 120u;
    instances[0].materialId = 220u;
    instances[0].sourceIndex = 11u;
    instances[0].drawGroupIndex = 8u;
    instances[0].drawGroupVisibleOffset = 6u;
    instances[0].candidateIndex = 4u;
    instances[1].meshId = 110u;
    instances[1].materialId = 210u;
    instances[1].sourceIndex = 12u;
    instances[1].drawGroupIndex = 8u;
    instances[1].drawGroupVisibleOffset = 7u;
    instances[1].candidateIndex = 5u;
    instances[2].meshId = 130u;
    instances[2].materialId = 230u;

    RenderDrawGroupKey tableGroup{};
    tableGroup.pass = RenderPassKind::Opaque;
    tableGroup.geometry.mesh = {91u, 5u};
    tableGroup.geometry.submeshIndex = 3u;
    tableGroup.material.material = {191u, 7u};
    tableGroup.instanceMaterial.textureBindingHash = 0x9E3779B97F4A7C15ull;
    tableGroup.instanceMaterial.parameterTableCompatible = true;
    tableGroup.indexCount = 36u;
    tableGroup.firstIndex = 7u;
    tableGroup.vertexOffset = -2;
    tableGroup.usesMaterialParameterTable = true;

    // MaterialSystem's binding hash includes texture handle generations. The
    // two values model identical texture/sampler bindings after only that
    // generation changes, which must not affect cross-process identity.
    RenderDrawGroupKey textureGenerationOnly = tableGroup;
    textureGenerationOnly.instanceMaterial.textureBindingHash ^= 0x101u;
    EXPECT_EQ(GetRasterTranscriptGroupIdentity(tableGroup),
              GetRasterTranscriptGroupIdentity(textureGenerationOnly));

    RenderDrawGroupKey fixedMaterialGroup = tableGroup;
    fixedMaterialGroup.geometry.submeshIndex = 7u;
    fixedMaterialGroup.indexCount = 12u;
    fixedMaterialGroup.firstIndex = 17u;
    fixedMaterialGroup.vertexOffset = 4;
    fixedMaterialGroup.usesMaterialParameterTable = false;
    const std::array<RasterInstanceStreamBatch, 2> batches{{
        {tableGroup, 0u, 2u, false},
        {fixedMaterialGroup, 2u, 1u, false}}};

    ASSERT_TRUE(CreateRasterInstanceStream(device,
                                           instances,
                                           keys,
                                           batches,
                                           "RasterTranscript",
                                           cache,
                                           stream,
                                           semanticIdentities));
    ASSERT_TRUE(stream.IsValid());
    ASSERT_EQ(2u, stream.batchBindings.size());
    EXPECT_NE(0u, stream.batchBindings[1].firstInstance);
    const RasterInstanceStreamFrameSlot& resident =
        cache.frameSlots[stream.activeFrameSlot];
    ASSERT_LT(stream.batchBindings[0].firstInstance,
              resident.residentDrawOrder.size());
    EXPECT_EQ(keys[1], resident.residentKeys[resident.residentDrawOrder[
                          stream.batchBindings[0].firstInstance]]);

    RasterTranscriptDigest actual;
    ASSERT_TRUE(BuildRasterInstanceStreamTranscript(stream, cache, actual));
    EXPECT_TRUE(actual.available);
    EXPECT_EQ(3u, actual.entryCount);

    RasterTranscriptDigest residentOrder;
    BeginRasterTranscript(residentOrder);
    for (const uint32 inputIndex : {1u, 0u, 2u})
    {
        const RenderDrawGroupKey& group = inputIndex < 2u
            ? tableGroup
            : fixedMaterialGroup;
        AppendRasterTranscriptEntry(
            residentOrder,
            MakeRasterTranscriptEntryDigest(
                GetRasterTranscriptGroupIdentity(group),
                keys[inputIndex],
                instances[inputIndex],
                semanticIdentities[inputIndex],
                group.usesMaterialParameterTable,
                group.indexCount,
                group.firstIndex,
                group.vertexOffset));
    }
    EXPECT_EQ(residentOrder.orderedIdentityHash, actual.orderedIdentityHash);
    EXPECT_EQ(residentOrder.consumedPayloadHash, actual.consumedPayloadHash);

    RasterTranscriptDigest sourceOrder;
    BeginRasterTranscript(sourceOrder);
    for (const uint32 inputIndex : {0u, 1u, 2u})
    {
        const RenderDrawGroupKey& group = inputIndex < 2u
            ? tableGroup
            : fixedMaterialGroup;
        AppendRasterTranscriptEntry(
            sourceOrder,
            MakeRasterTranscriptEntryDigest(
                GetRasterTranscriptGroupIdentity(group),
                keys[inputIndex],
                instances[inputIndex],
                semanticIdentities[inputIndex],
                group.usesMaterialParameterTable,
                group.indexCount,
                group.firstIndex,
                group.vertexOffset));
    }
    EXPECT_NE(sourceOrder.orderedIdentityHash, actual.orderedIdentityHash);
    EXPECT_EQ(sourceOrder.unorderedIdentityHash,
              actual.unorderedIdentityHash);
    EXPECT_EQ(sourceOrder.unorderedIdentityHashSecondary,
              actual.unorderedIdentityHashSecondary);
    EXPECT_EQ(sourceOrder.unorderedConsumedPayloadHash,
              actual.unorderedConsumedPayloadHash);
    EXPECT_EQ(sourceOrder.unorderedConsumedPayloadHashSecondary,
              actual.unorderedConsumedPayloadHashSecondary);

    auto reorderedInstances = instances;
    auto reorderedKeys = keys;
    auto reorderedSemanticIdentities = semanticIdentities;
    std::swap(reorderedInstances[0], reorderedInstances[1]);
    std::swap(reorderedKeys[0], reorderedKeys[1]);
    std::swap(reorderedSemanticIdentities[0], reorderedSemanticIdentities[1]);
    ASSERT_TRUE(CreateRasterInstanceStream(device,
                                           reorderedInstances,
                                           reorderedKeys,
                                           batches,
                                           "RasterTranscript",
                                           cache,
                                           stream,
                                           reorderedSemanticIdentities));
    RasterTranscriptDigest historyPreserved;
    ASSERT_TRUE(BuildRasterInstanceStreamTranscript(
        stream, cache, historyPreserved));
    EXPECT_EQ(actual.orderedIdentityHash, historyPreserved.orderedIdentityHash);
    EXPECT_EQ(actual.consumedPayloadHash, historyPreserved.consumedPayloadHash);

    auto privateFieldMutation = reorderedInstances;
    privateFieldMutation[0].sourceIndex += 100u;
    privateFieldMutation[0].drawGroupIndex += 100u;
    privateFieldMutation[0].drawGroupVisibleOffset += 100u;
    privateFieldMutation[0].candidateIndex += 100u;
    ASSERT_TRUE(CreateRasterInstanceStream(device,
                                           privateFieldMutation,
                                           reorderedKeys,
                                           batches,
                                           "RasterTranscript",
                                           cache,
                                           stream,
                                           reorderedSemanticIdentities));
    RasterTranscriptDigest privateFieldDigest;
    ASSERT_TRUE(BuildRasterInstanceStreamTranscript(
        stream, cache, privateFieldDigest));
    EXPECT_EQ(historyPreserved.orderedIdentityHash,
              privateFieldDigest.orderedIdentityHash);
    EXPECT_EQ(historyPreserved.consumedPayloadHash,
              privateFieldDigest.consumedPayloadHash);

    auto unconsumedMaterialMutation = privateFieldMutation;
    ++unconsumedMaterialMutation[2].materialId;
    ASSERT_TRUE(CreateRasterInstanceStream(device,
                                           unconsumedMaterialMutation,
                                           reorderedKeys,
                                           batches,
                                           "RasterTranscript",
                                           cache,
                                           stream,
                                           reorderedSemanticIdentities));
    RasterTranscriptDigest unconsumedMaterialDigest;
    ASSERT_TRUE(BuildRasterInstanceStreamTranscript(
        stream, cache, unconsumedMaterialDigest));
    EXPECT_EQ(privateFieldDigest.consumedPayloadHash,
              unconsumedMaterialDigest.consumedPayloadHash);

    auto worldMutation = unconsumedMaterialMutation;
    worldMutation[0].worldMatrix[3].x += 3.0f;
    ASSERT_TRUE(CreateRasterInstanceStream(device,
                                           worldMutation,
                                           reorderedKeys,
                                           batches,
                                           "RasterTranscript",
                                           cache,
                                           stream,
                                           reorderedSemanticIdentities));
    RasterTranscriptDigest worldDigest;
    ASSERT_TRUE(BuildRasterInstanceStreamTranscript(stream, cache, worldDigest));
    EXPECT_EQ(unconsumedMaterialDigest.orderedIdentityHash,
              worldDigest.orderedIdentityHash);
    EXPECT_EQ(unconsumedMaterialDigest.unorderedIdentityHash,
              worldDigest.unorderedIdentityHash);
    EXPECT_EQ(unconsumedMaterialDigest.unorderedIdentityHashSecondary,
              worldDigest.unorderedIdentityHashSecondary);
    EXPECT_NE(unconsumedMaterialDigest.consumedPayloadHash,
              worldDigest.consumedPayloadHash);
    EXPECT_NE(unconsumedMaterialDigest.unorderedConsumedPayloadHash,
              worldDigest.unorderedConsumedPayloadHash);
    EXPECT_NE(unconsumedMaterialDigest.unorderedConsumedPayloadHashSecondary,
              worldDigest.unorderedConsumedPayloadHashSecondary);

    auto consumedMaterialMutation = worldMutation;
    ++consumedMaterialMutation[0].materialId;
    ASSERT_TRUE(CreateRasterInstanceStream(device,
                                           consumedMaterialMutation,
                                           reorderedKeys,
                                           batches,
                                           "RasterTranscript",
                                           cache,
                                           stream,
                                           reorderedSemanticIdentities));
    RasterTranscriptDigest consumedMaterialDigest;
    ASSERT_TRUE(BuildRasterInstanceStreamTranscript(
        stream, cache, consumedMaterialDigest));
    EXPECT_EQ(worldDigest.orderedIdentityHash,
              consumedMaterialDigest.orderedIdentityHash);
    EXPECT_EQ(worldDigest.consumedPayloadHash,
              consumedMaterialDigest.consumedPayloadHash);
}

TEST_F(GPUDrivenValidationFixture,
       DirectOpaqueRasterReadbackQualificationUsesTwoPostFencePhysicalCopies)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    constexpr uint32 physicalDeviceFrameSlot = 1u;
    constexpr uint32 logicalRenderContextFrameSlot = 0u;
    device.SetCurrentFrameIndex(physicalDeviceFrameSlot);
    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));

    RasterInstanceStreamCache cache;
    RasterInstanceStream stream;
    std::array<GPUInstanceData, 2> instances{{
        MakeInstance(Vec3(-1.0f, 0.0f, -5.0f), 1.0f, 36u),
        MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 36u)}};
    const std::array<RasterInstanceStreamKey, 2> keys{{{701u, 0u}, {702u, 0u}}};
    const std::array<uint64, 2> semantics{{0x7011u, 0x7021u}};
    RenderDrawGroupKey key{};
    key.pass = RenderPassKind::Opaque;
    key.indexCount = 36u;
    key.firstIndex = 7u;
    key.vertexOffset = -2;
    const std::array<RasterInstanceStreamBatch, 1> batches{{
        {key, 0u, 2u, false}}};
    ASSERT_TRUE(CreateRasterInstanceStream(device,
                                           instances,
                                           keys,
                                           batches,
                                           "DirectReadback",
                                           cache,
                                           stream,
                                           semantics));

    RasterTranscriptDigest reference;
    ASSERT_TRUE(BuildRasterInstanceStreamTranscript(stream, cache, reference));
    ASSERT_TRUE(reference.available);
    const RasterInstanceStreamBatchBinding& binding = stream.batchBindings.front();
    const std::array<DirectRasterReadbackDraw, 1> draws{{{
        binding.key,
        {binding.key.indexCount,
         binding.instanceCount,
         binding.key.firstIndex,
         binding.key.vertexOffset,
         binding.firstInstance},
        binding.instanceCount,
        true}}};
    const DirectRasterReadbackRecordingIdentity identity{77u, 1u, 9u, 3u,
                                                           stream.activeFrameSlot};
    ASSERT_EQ(physicalDeviceFrameSlot, stream.activeFrameSlot);
    ASSERT_NE(logicalRenderContextFrameSlot, identity.sourceFrameSlot);

    DirectRasterReadbackQualification qualification;
    FakeCommandContext context;
    const size_t buffersBeforeUnarmed = device.createdBuffers.size();
    EXPECT_TRUE(qualification.RecordPostRenderCopy(context, identity));
    EXPECT_EQ(buffersBeforeUnarmed, device.createdBuffers.size());
    EXPECT_EQ(0u, context.copyBufferCalls);

    ASSERT_TRUE(qualification.Arm());
    ASSERT_TRUE(qualification.Prepare(device,
                                      identity,
                                      stream,
                                      cache,
                                      draws,
                                      reference));
    EXPECT_EQ(buffersBeforeUnarmed + 2u, device.createdBuffers.size());
    ASSERT_TRUE(qualification.RecordPostRenderCopy(context, identity));
    EXPECT_EQ(2u, context.copyBufferCalls);
    FakeBuffer* const instanceReadback = device.FindBuffer(
        "DirectOpaqueRasterQualification.InstanceReadback");
    FakeBuffer* const indexReadback = device.FindBuffer(
        "DirectOpaqueRasterQualification.Slot6IndexReadback");
    ASSERT_NE(nullptr, instanceReadback);
    ASSERT_NE(nullptr, indexReadback);
    EXPECT_EQ(0u, instanceReadback->GetMapCallCount());
    EXPECT_EQ(0u, indexReadback->GetMapCallCount());

    // Simulate a logical RenderContext advance after recording.  The frozen
    // physical identity remains the resident stream slot through submission.
    device.SetCurrentFrameIndex(logicalRenderContextFrameSlot);
    FakeCommandContext computeContext;
    computeContext.queueType = RHICommandQueueType::Compute;
    const GPUCompletionPoint computePoint = tracker.Submit(&computeContext);
    ASSERT_NE(0u, computePoint.value);
    const GPUCompletionPoint point = tracker.Submit(&context);
    ASSERT_NE(0u, point.value);
    GPUCompletionToken completion;
    ASSERT_TRUE(InsertGPUCompletionPoint(completion, computePoint));
    ASSERT_TRUE(InsertGPUCompletionPoint(completion, point));
    ASSERT_TRUE(qualification.NotifySubmission(identity, completion, tracker));
    EXPECT_FALSE(qualification.PollCompletion(tracker));
    EXPECT_EQ(0u, instanceReadback->GetMapCallCount());
    EXPECT_EQ(0u, indexReadback->GetMapCallCount());

    FakeFence* const fence = device.GetFence(0);
    ASSERT_NE(nullptr, fence);
    fence->Complete(point.value);
    EXPECT_TRUE(qualification.PollCompletion(tracker));
    const DirectRasterReadbackQualificationDiagnostics& diagnostics =
        qualification.GetDiagnostics();
    EXPECT_TRUE(diagnostics.requested);
    EXPECT_TRUE(diagnostics.required);
    EXPECT_TRUE(diagnostics.readbackAllocated);
    EXPECT_TRUE(diagnostics.copyRecorded);
    EXPECT_TRUE(diagnostics.submissionAccepted);
    EXPECT_TRUE(diagnostics.completionObserved);
    EXPECT_TRUE(diagnostics.compared);
    EXPECT_TRUE(diagnostics.matched);
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::None,
              diagnostics.mismatch);
    EXPECT_EQ(identity.frameSequence, diagnostics.frameSequence);
    EXPECT_EQ(identity.sourceFrameSlot, diagnostics.sourceFrameSlot);
    EXPECT_EQ(reference.orderedIdentityHash,
              diagnostics.observedTranscript.orderedIdentityHash);

    // Mutating physical slot-6 after the CPU snapshot must classify as index
    // evidence drift, rather than as a transcript-only disagreement.
    ASSERT_TRUE(qualification.Arm());
    ASSERT_TRUE(qualification.Prepare(device,
                                      identity,
                                      stream,
                                      cache,
                                      draws,
                                      reference));
    auto* const sourceIndices = static_cast<uint32*>(stream.instanceIndices->Map());
    ASSERT_NE(nullptr, sourceIndices);
    sourceIndices[binding.firstInstance] =
        1u - sourceIndices[binding.firstInstance];
    stream.instanceIndices->Unmap();
    FakeCommandContext indexMutationContext;
    ASSERT_TRUE(qualification.RecordPostRenderCopy(indexMutationContext, identity));
    const GPUCompletionPoint indexMutationPoint = tracker.Submit(&indexMutationContext);
    GPUCompletionToken indexMutationCompletion;
    ASSERT_TRUE(InsertGPUCompletionPoint(indexMutationCompletion, indexMutationPoint));
    ASSERT_TRUE(qualification.NotifySubmission(
        identity, indexMutationCompletion, tracker));
    fence->Complete(indexMutationPoint.value);
    EXPECT_FALSE(qualification.PollCompletion(tracker));
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::InstanceIndexPayload,
              qualification.GetDiagnostics().mismatch);

    const RasterInstanceStreamFrameSlot& resident =
        cache.frameSlots[stream.activeFrameSlot];
    auto* const restoredIndices = static_cast<uint32*>(stream.instanceIndices->Map());
    ASSERT_NE(nullptr, restoredIndices);
    std::memcpy(restoredIndices,
                resident.residentDrawOrder.data(),
                resident.residentDrawOrder.size() * sizeof(uint32));
    stream.instanceIndices->Unmap();

    // A world transform byte mutation is independently classified as the
    // actual instance-buffer payload drift read by the raster stage.
    ASSERT_TRUE(qualification.Arm());
    ASSERT_TRUE(qualification.Prepare(device,
                                      identity,
                                      stream,
                                      cache,
                                      draws,
                                      reference));
    auto* const sourceInstances =
        static_cast<GPUInstanceData*>(stream.instances->Map());
    ASSERT_NE(nullptr, sourceInstances);
    sourceInstances[0].worldMatrix[3].x += 9.0f;
    stream.instances->Unmap();
    FakeCommandContext instanceMutationContext;
    ASSERT_TRUE(qualification.RecordPostRenderCopy(instanceMutationContext, identity));
    const GPUCompletionPoint instanceMutationPoint =
        tracker.Submit(&instanceMutationContext);
    GPUCompletionToken instanceMutationCompletion;
    ASSERT_TRUE(InsertGPUCompletionPoint(instanceMutationCompletion,
                                         instanceMutationPoint));
    ASSERT_TRUE(qualification.NotifySubmission(
        identity, instanceMutationCompletion, tracker));
    fence->Complete(instanceMutationPoint.value);
    EXPECT_FALSE(qualification.PollCompletion(tracker));
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::InstancePayload,
              qualification.GetDiagnostics().mismatch);

    // A valid aggregate token with a foreign identity closes its submitted
    // resources at the fence without mapping them, then permits a new arm.
    ASSERT_TRUE(qualification.Arm());
    ASSERT_TRUE(qualification.Prepare(device,
                                      identity,
                                      stream,
                                      cache,
                                      draws,
                                      reference));
    ASSERT_GE(device.retainedDirectReadbackBuffers.size(), 2u);
    auto* const closureInstanceReadback = static_cast<FakeBuffer*>(
        device.retainedDirectReadbackBuffers[
            device.retainedDirectReadbackBuffers.size() - 2u].Get());
    const uint32 mapsBeforeClosure = closureInstanceReadback->GetMapCallCount();
    FakeCommandContext foreignContext;
    ASSERT_TRUE(qualification.RecordPostRenderCopy(foreignContext, identity));
    const GPUCompletionPoint foreignPoint = tracker.Submit(&foreignContext);
    GPUCompletionToken foreignCompletion;
    ASSERT_TRUE(InsertGPUCompletionPoint(foreignCompletion, foreignPoint));
    const DirectRasterReadbackRecordingIdentity foreignIdentity{
        identity.graphIdentity + 1u,
        identity.graphRecordingGeneration,
        identity.frameSequence,
        identity.recordEpoch,
        identity.sourceFrameSlot};
    EXPECT_FALSE(qualification.NotifySubmission(
        foreignIdentity, foreignCompletion, tracker));
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::IdentityRejected,
              qualification.GetDiagnostics().mismatch);
    EXPECT_TRUE(qualification.HasPendingCompletion());
    EXPECT_FALSE(qualification.PollCompletion(tracker));
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::IdentityRejected,
              qualification.GetDiagnostics().mismatch);
    fence->Complete(foreignPoint.value);
    EXPECT_FALSE(qualification.PollCompletion(tracker));
    EXPECT_EQ(mapsBeforeClosure, closureInstanceReadback->GetMapCallCount());
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::IdentityRejected,
              qualification.GetDiagnostics().mismatch);
    EXPECT_TRUE(qualification.GetDiagnostics().completionObserved);
    EXPECT_FALSE(qualification.GetDiagnostics().compared);
    EXPECT_FALSE(qualification.GetDiagnostics().matched);
    EXPECT_FALSE(qualification.HasPendingCompletion());
    EXPECT_TRUE(qualification.Arm());

    // Submission is still closed even when a valid graphics completion proves
    // the frame submitted before the required two-copy precondition was met.
    ASSERT_TRUE(qualification.Prepare(device,
                                      identity,
                                      stream,
                                      cache,
                                      draws,
                                      reference));
    FakeCommandContext missingCopyContext;
    const GPUCompletionPoint missingCopyPoint = tracker.Submit(&missingCopyContext);
    GPUCompletionToken missingCopyCompletion;
    ASSERT_TRUE(InsertGPUCompletionPoint(missingCopyCompletion, missingCopyPoint));
    EXPECT_FALSE(qualification.NotifySubmission(
        identity, missingCopyCompletion, tracker));
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::CopyNotRecorded,
              qualification.GetDiagnostics().mismatch);
    EXPECT_TRUE(qualification.HasPendingCompletion());
    EXPECT_FALSE(qualification.PollCompletion(tracker));
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::CopyNotRecorded,
              qualification.GetDiagnostics().mismatch);
    fence->Complete(missingCopyPoint.value);
    EXPECT_FALSE(qualification.PollCompletion(tracker));
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::CopyNotRecorded,
              qualification.GetDiagnostics().mismatch);
    EXPECT_TRUE(qualification.GetDiagnostics().completionObserved);
    EXPECT_FALSE(qualification.GetDiagnostics().compared);
    EXPECT_FALSE(qualification.GetDiagnostics().matched);
    EXPECT_FALSE(qualification.HasPendingCompletion());
    EXPECT_TRUE(qualification.Arm());

    // Empty completion evidence leaves recording resources owned until the
    // explicit unsubmitted release; it must not silently become reusable.
    ASSERT_TRUE(qualification.Prepare(device,
                                      identity,
                                      stream,
                                      cache,
                                      draws,
                                      reference));
    FakeCommandContext unsubmittedContext;
    ASSERT_TRUE(qualification.RecordPostRenderCopy(unsubmittedContext, identity));
    const GPUCompletionToken emptyCompletion;
    EXPECT_FALSE(qualification.NotifySubmission(
        identity, emptyCompletion, tracker));
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::CompletionRejected,
              qualification.GetDiagnostics().mismatch);
    qualification.ReleaseUnsubmitted(identity);
    EXPECT_FALSE(qualification.HasPendingCompletion());
    EXPECT_TRUE(qualification.Arm());

    // Semantic-finalization failure is represented by an unavailable Direct
    // reference: no readback allocation or copy is allowed.
    const size_t buffersBeforeUnavailableReference = device.createdBuffers.size();
    RasterTranscriptDigest unavailableReference;
    EXPECT_FALSE(qualification.Prepare(device,
                                       identity,
                                       stream,
                                       cache,
                                       draws,
                                       unavailableReference));
    EXPECT_EQ(buffersBeforeUnavailableReference, device.createdBuffers.size());
    FakeCommandContext unavailableContext;
    EXPECT_TRUE(qualification.RecordPostRenderCopy(unavailableContext, identity));
    EXPECT_EQ(0u, unavailableContext.copyBufferCalls);
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::ReferenceUnavailable,
              qualification.GetDiagnostics().mismatch);
    EXPECT_FALSE(qualification.GetDiagnostics().matched);

    // A valid target with no Direct lane consumes the exact one-shot and
    // remains a required qualification failure.
    ASSERT_TRUE(qualification.Arm());
    qualification.RejectNoDirectLane(identity);
    EXPECT_FALSE(qualification.IsArmed());
    EXPECT_TRUE(qualification.GetDiagnostics().requested);
    EXPECT_TRUE(qualification.GetDiagnostics().required);
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::DirectLaneUnavailable,
              qualification.GetDiagnostics().mismatch);

    // The same is true when target recording failed before a valid identity
    // existed; that case is classified as identity rejection instead.
    ASSERT_TRUE(qualification.Arm());
    qualification.RejectNoDirectLane({});
    EXPECT_FALSE(qualification.IsArmed());
    EXPECT_TRUE(qualification.GetDiagnostics().requested);
    EXPECT_TRUE(qualification.GetDiagnostics().required);
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::IdentityRejected,
              qualification.GetDiagnostics().mismatch);
}

TEST_F(GPUDrivenValidationFixture,
       DirectOpaqueRasterReadbackQualificationFreezesPhysicalDeviceSlotForClosure)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());
    const std::string renderer = ReadTextFile(
        root / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
    const std::string recordContext = ReadTextFile(
        root / "Render" / "Include" / "Render" / "Passes" /
        "RenderPassRecordContext.h");
    const std::string opaque = ReadTextFile(
        root / "Render" / "Private" / "Passes" / "OpaquePass.cpp");
    ASSERT_FALSE(renderer.empty());
    ASSERT_FALSE(recordContext.empty());
    ASSERT_FALSE(opaque.empty());

    const size_t freeze = renderer.find(
        "m_directOpaqueRasterReadbackSourceFrameSlot = directReadbackDevice");
    const size_t passContextSlot = renderer.find(
        "passRecordContext.directOpaqueRasterReadbackSourceFrameSlot =");
    ASSERT_NE(std::string::npos, freeze);
    ASSERT_NE(std::string::npos, passContextSlot);
    EXPECT_LT(freeze, passContextSlot);
    EXPECT_NE(std::string::npos, renderer.find(
        "directReadbackDevice->GetCurrentFrameIndex()", freeze));
    EXPECT_NE(std::string::npos, recordContext.find(
        "Frozen physical RHI device slot selected for this graph recording"));
    EXPECT_NE(std::string::npos, opaque.find(
        "m_directReadbackSourceFrameSlot"));

    size_t identity = renderer.find(
        "const DirectRasterReadbackRecordingIdentity directQualificationIdentity{");
    ASSERT_NE(std::string::npos, identity);
    for (uint32 occurrence = 0; occurrence < 2u; ++occurrence)
    {
        const std::string identityWindow = renderer.substr(identity, 480u);
        EXPECT_NE(std::string::npos, identityWindow.find(
            "m_directOpaqueRasterReadbackSourceFrameSlot"));
        EXPECT_EQ(std::string::npos, identityWindow.find(
            "m_renderContext->GetFrameIndex()"));
        identity = renderer.find(
            "const DirectRasterReadbackRecordingIdentity directQualificationIdentity{",
            identity + 1u);
        if (occurrence == 0u)
        {
            ASSERT_NE(std::string::npos, identity);
        }
    }
}

TEST_F(GPUDrivenValidationFixture,
       DirectOpaqueRasterReadbackQualificationFailClosesWithoutChangingDraws)
{
    FakeDevice device;
    RasterInstanceStreamCache cache;
    RasterInstanceStream stream;
    const std::array<GPUInstanceData, 1> instances{{GPUInstanceData{}}};
    const std::array<RasterInstanceStreamKey, 1> keys{{{901u, 0u}}};
    ASSERT_TRUE(CreateRasterInstanceStream(device,
                                           instances,
                                           keys,
                                           "DirectReadbackSingleton",
                                           cache,
                                           stream));
    const DirectRasterReadbackRecordingIdentity identity{79u, 1u, 11u, 5u,
                                                           stream.activeFrameSlot};
    DirectRasterReadbackQualification qualification;
    ASSERT_TRUE(qualification.Arm());
    const DirectRasterReadbackDraw singleton{};
    RasterTranscriptDigest unavailableReference{};
    FakeCommandContext context;
    context.DrawIndexed(3u);
    EXPECT_FALSE(qualification.Prepare(
        device,
        identity,
        stream,
        cache,
        std::span<const DirectRasterReadbackDraw>(&singleton, 1u),
        unavailableReference));
    context.DrawIndexed(3u);
    EXPECT_EQ(2u, context.drawIndexedCalls);
    EXPECT_EQ(DirectRasterReadbackQualificationMismatch::UnsupportedNonInstancedDraw,
              qualification.GetDiagnostics().mismatch);
}

TEST_F(GPUDrivenValidationFixture,
       RasterSemanticIdentityUsesAssetIdsAcrossSlotsGenerationsAndTranscripts)
{
    MaterialSourceData source;
    source.baseColorFactor = Vec4(0.25f, 0.5f, 0.75f, 1.0f);
    source.roughnessFactor = 0.35f;

    RasterSemanticRegistry first;
    const RenderResourceHandle firstMesh{41u, 1u};
    const RenderResourceHandle firstTexture{42u, 1u};
    const RenderResourceHandle firstMaterial{43u, 1u};
    ASSERT_TRUE(first.AddReadyMesh(firstMesh, AssetId{101u}));
    ASSERT_TRUE(first.AddReadyTexture(firstTexture, AssetId{301u}));
    MaterialUploadTextureBinding firstBinding;
    firstBinding.slot = MaterialUploadTextureSlot::BaseColor;
    firstBinding.texture = firstTexture;
    ASSERT_TRUE(first.AddReadyMaterial(
        firstMaterial, AssetId{201u}, source, {firstBinding}));

    RasterSemanticRegistry second;
    const RenderResourceHandle secondMesh{51u, 2u};
    const RenderResourceHandle secondTexture{52u, 3u};
    const RenderResourceHandle secondMaterial{53u, 4u};
    ASSERT_TRUE(second.AddReadyMesh(secondMesh, AssetId{101u}));
    ASSERT_TRUE(second.AddReadyTexture(secondTexture, AssetId{301u}));
    MaterialUploadTextureBinding secondBinding = firstBinding;
    secondBinding.texture = secondTexture;
    ASSERT_TRUE(second.AddReadyMaterial(
        secondMaterial, AssetId{201u}, source, {secondBinding}));

    EXPECT_EQ(101u, first.GetRegistry().GetExactAssetId(firstMesh).value);
    EXPECT_EQ(201u, first.GetRegistry().GetExactAssetId(firstMaterial).value);
    const std::optional<uint64> firstIdentity =
        first.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            firstMesh, 0xF101u);
    const std::optional<uint64> secondIdentity =
        second.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            secondMesh, 0xF101u);
    ASSERT_TRUE(firstIdentity.has_value());
    ASSERT_TRUE(secondIdentity.has_value());
    EXPECT_EQ(*firstIdentity, *secondIdentity);

    RenderDrawGroupKey firstGroup{};
    firstGroup.pass = RenderPassKind::Opaque;
    firstGroup.geometry.mesh = firstMesh;
    firstGroup.material.material = firstMaterial;
    firstGroup.indexCount = 36u;
    firstGroup.firstIndex = 7u;
    firstGroup.vertexOffset = -2;
    RenderDrawGroupKey secondGroup = firstGroup;
    secondGroup.geometry.mesh = secondMesh;
    secondGroup.material.material = secondMaterial;
    EXPECT_EQ(GetRasterTranscriptGroupIdentity(firstGroup),
              GetRasterTranscriptGroupIdentity(secondGroup));

    GPUInstanceData firstInstance =
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36u);
    GPUInstanceData secondInstance = firstInstance;
    firstInstance.meshId = firstMesh.slot;
    firstInstance.materialId = firstMaterial.slot;
    secondInstance.meshId = secondMesh.slot;
    secondInstance.materialId = secondMaterial.slot;
    const RasterInstanceStreamKey key{7001u, 0u};
    const RasterTranscriptEntryDigest firstEntry = MakeRasterTranscriptEntryDigest(
        GetRasterTranscriptGroupIdentity(firstGroup),
        key,
        firstInstance,
        *firstIdentity,
        false,
        36u,
        7u,
        -2);
    const RasterTranscriptEntryDigest secondEntry = MakeRasterTranscriptEntryDigest(
        GetRasterTranscriptGroupIdentity(secondGroup),
        key,
        secondInstance,
        *secondIdentity,
        false,
        36u,
        7u,
        -2);
    EXPECT_TRUE(firstEntry.available);
    EXPECT_TRUE(secondEntry.available);
    EXPECT_EQ(firstEntry.identityHash, secondEntry.identityHash);
    EXPECT_EQ(firstEntry.consumedPayloadHash, secondEntry.consumedPayloadHash);

    RasterTranscriptDigest firstTranscript;
    RasterTranscriptDigest secondTranscript;
    BeginRasterTranscript(firstTranscript);
    BeginRasterTranscript(secondTranscript);
    AppendRasterTranscriptEntry(firstTranscript, firstEntry);
    AppendRasterTranscriptEntry(secondTranscript, secondEntry);
    EXPECT_EQ(firstTranscript.orderedIdentityHash,
              secondTranscript.orderedIdentityHash);
    EXPECT_EQ(firstTranscript.consumedPayloadHash,
              secondTranscript.consumedPayloadHash);

    // Replacement reuses the physical slot but cannot change the asset that
    // owns that exact Registry entry.
    EXPECT_FALSE(first.BeginMaterialReplacement(
        firstMaterial, AssetId{202u}, 1u));
}

TEST_F(GPUDrivenValidationFixture,
       RasterSemanticIdentityDistinguishesReusedSlotAssetChanges)
{
    const MaterialSourceData source{};
    RasterSemanticRegistry baseline;
    ASSERT_TRUE(baseline.AddReadyMesh({61u, 1u}, AssetId{1001u}));
    ASSERT_TRUE(baseline.AddReadyMaterial({62u, 1u}, AssetId{2001u}, source, {}));

    RasterSemanticRegistry changedMesh;
    ASSERT_TRUE(changedMesh.AddReadyMesh({61u, 2u}, AssetId{1002u}));
    ASSERT_TRUE(changedMesh.AddReadyMaterial({62u, 1u}, AssetId{2001u}, source, {}));

    RasterSemanticRegistry changedMaterial;
    ASSERT_TRUE(changedMaterial.AddReadyMesh({61u, 1u}, AssetId{1001u}));
    ASSERT_TRUE(changedMaterial.AddReadyMaterial(
        {62u, 2u}, AssetId{2002u}, source, {}));

    const std::optional<uint64> baselineIdentity =
        baseline.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            {61u, 1u}, 0xF201u);
    const std::optional<uint64> changedMeshIdentity =
        changedMesh.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            {61u, 2u}, 0xF201u);
    const std::optional<uint64> changedMaterialIdentity =
        changedMaterial.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            {61u, 1u}, 0xF202u);
    ASSERT_TRUE(baselineIdentity.has_value());
    ASSERT_TRUE(changedMeshIdentity.has_value());
    ASSERT_TRUE(changedMaterialIdentity.has_value());
    EXPECT_NE(*baselineIdentity, *changedMeshIdentity);
    EXPECT_NE(*baselineIdentity, *changedMaterialIdentity);
}

TEST_F(GPUDrivenValidationFixture,
       RasterSemanticIdentityIncludesFixedAndParameterTableMaterialContent)
{
    MaterialSourceData original;
    original.metallicFactor = 0.1f;
    original.roughnessFactor = 0.2f;
    MaterialSourceData changed = original;
    changed.roughnessFactor = 0.8f;

    RasterSemanticRegistry first;
    ASSERT_TRUE(first.AddReadyMesh({71u, 1u}, AssetId{1101u}));
    ASSERT_TRUE(first.AddReadyMaterial(
        {72u, 1u}, AssetId{2101u}, original, {}));
    RasterSemanticRegistry second;
    ASSERT_TRUE(second.AddReadyMesh({71u, 2u}, AssetId{1101u}));
    ASSERT_TRUE(second.AddReadyMaterial(
        {72u, 2u}, AssetId{2101u}, changed, {}));

    const std::optional<uint64> originalIdentity =
        first.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            {71u, 1u}, 0xF301u);
    const std::optional<uint64> changedIdentity =
        second.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            {71u, 2u}, 0xF302u);
    ASSERT_TRUE(originalIdentity.has_value());
    ASSERT_TRUE(changedIdentity.has_value());
    EXPECT_NE(*originalIdentity, *changedIdentity);

    RenderDrawGroupKey fixedGroup{};
    fixedGroup.pass = RenderPassKind::Opaque;
    fixedGroup.indexCount = 36u;
    fixedGroup.firstIndex = 7u;
    fixedGroup.vertexOffset = -2;
    RenderDrawGroupKey tableGroup = fixedGroup;
    tableGroup.usesMaterialParameterTable = true;

    GPUInstanceData originalInstance =
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36u);
    GPUInstanceData changedInstance = originalInstance;
    const RasterInstanceStreamKey key{7002u, 0u};
    const RasterTranscriptEntryDigest fixedOriginal =
        MakeRasterTranscriptEntryDigest(GetRasterTranscriptGroupIdentity(fixedGroup),
                                        key,
                                        originalInstance,
                                        *originalIdentity,
                                        false,
                                        36u,
                                        7u,
                                        -2);
    const RasterTranscriptEntryDigest fixedChanged =
        MakeRasterTranscriptEntryDigest(GetRasterTranscriptGroupIdentity(fixedGroup),
                                        key,
                                        changedInstance,
                                        *changedIdentity,
                                        false,
                                        36u,
                                        7u,
                                        -2);
    const RasterTranscriptEntryDigest tableOriginal =
        MakeRasterTranscriptEntryDigest(GetRasterTranscriptGroupIdentity(tableGroup),
                                        key,
                                        originalInstance,
                                        *originalIdentity,
                                        true,
                                        36u,
                                        7u,
                                        -2);
    const RasterTranscriptEntryDigest tableChanged =
        MakeRasterTranscriptEntryDigest(GetRasterTranscriptGroupIdentity(tableGroup),
                                        key,
                                        changedInstance,
                                        *changedIdentity,
                                        true,
                                        36u,
                                        7u,
                                        -2);
    EXPECT_NE(fixedOriginal.consumedPayloadHash, fixedChanged.consumedPayloadHash);
    EXPECT_NE(fixedOriginal.identityHash, fixedChanged.identityHash);
    EXPECT_NE(tableOriginal.consumedPayloadHash, tableChanged.consumedPayloadHash);
    EXPECT_NE(tableOriginal.identityHash, tableChanged.identityHash);
}

TEST_F(GPUDrivenValidationFixture,
       RasterSemanticIdentityUsesTextureAssetsAndStableFallbackTokens)
{
    MaterialSourceData source;
    source.textureFlags = 1u;
    const auto makeBinding = [](RenderResourceHandle texture, bool fallback)
    {
        MaterialUploadTextureBinding binding;
        binding.slot = MaterialUploadTextureSlot::BaseColor;
        binding.texture = texture;
        binding.isDefaultFallback = fallback;
        binding.uvSet = 1;
        binding.offset = Vec2(0.25f, 0.5f);
        binding.scale = Vec2(0.75f, 1.25f);
        binding.rotation = 0.125f;
        binding.wrapS = MaterialUploadWrapMode::ClampToEdge;
        binding.wrapT = MaterialUploadWrapMode::MirrorRepeat;
        binding.minFilter = MaterialUploadFilterMode::LinearMipmapNearest;
        binding.magFilter = MaterialUploadFilterMode::Nearest;
        return binding;
    };

    RasterSemanticRegistry baseline;
    ASSERT_TRUE(baseline.AddReadyMesh({81u, 1u}, AssetId{1201u}));
    ASSERT_TRUE(baseline.AddReadyTexture({82u, 1u}, AssetId{3201u}));
    ASSERT_TRUE(baseline.AddReadyMaterial(
        {83u, 1u}, AssetId{2201u}, source, {makeBinding({82u, 1u}, false)}));

    RasterSemanticRegistry generationOnly;
    ASSERT_TRUE(generationOnly.AddReadyMesh({81u, 2u}, AssetId{1201u}));
    ASSERT_TRUE(generationOnly.AddReadyTexture({82u, 3u}, AssetId{3201u}));
    ASSERT_TRUE(generationOnly.AddReadyMaterial(
        {83u, 4u}, AssetId{2201u}, source, {makeBinding({82u, 3u}, false)}));

    RasterSemanticRegistry changedTexture;
    ASSERT_TRUE(changedTexture.AddReadyMesh({81u, 2u}, AssetId{1201u}));
    ASSERT_TRUE(changedTexture.AddReadyTexture({82u, 3u}, AssetId{3202u}));
    ASSERT_TRUE(changedTexture.AddReadyMaterial(
        {83u, 4u}, AssetId{2201u}, source, {makeBinding({82u, 3u}, false)}));

    RasterSemanticRegistry fallbackOne;
    ASSERT_TRUE(fallbackOne.AddReadyMesh({91u, 1u}, AssetId{1201u}));
    ASSERT_TRUE(fallbackOne.AddReadyTexture({92u, 1u}, AssetId{4201u}));
    ASSERT_TRUE(fallbackOne.AddReadyMaterial(
        {93u, 1u}, AssetId{2201u}, source, {makeBinding({92u, 1u}, true)}));
    RasterSemanticRegistry fallbackTwo;
    ASSERT_TRUE(fallbackTwo.AddReadyMesh({91u, 2u}, AssetId{1201u}));
    ASSERT_TRUE(fallbackTwo.AddReadyTexture({92u, 3u}, AssetId{4202u}));
    ASSERT_TRUE(fallbackTwo.AddReadyMaterial(
        {93u, 4u}, AssetId{2201u}, source, {makeBinding({92u, 3u}, true)}));

    const std::optional<uint64> baselineIdentity =
        baseline.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            {81u, 1u}, 0xF401u);
    const std::optional<uint64> generationOnlyIdentity =
        generationOnly.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            {81u, 2u}, 0xF401u);
    const std::optional<uint64> changedTextureIdentity =
        changedTexture.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            {81u, 2u}, 0xF402u);
    const std::optional<uint64> fallbackOneIdentity =
        fallbackOne.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            {91u, 1u}, 0xF403u);
    const std::optional<uint64> fallbackTwoIdentity =
        fallbackTwo.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            {91u, 2u}, 0xF403u);
    ASSERT_TRUE(baselineIdentity.has_value());
    ASSERT_TRUE(generationOnlyIdentity.has_value());
    ASSERT_TRUE(changedTextureIdentity.has_value());
    ASSERT_TRUE(fallbackOneIdentity.has_value());
    ASSERT_TRUE(fallbackTwoIdentity.has_value());
    EXPECT_EQ(*baselineIdentity, *generationOnlyIdentity);
    EXPECT_NE(*baselineIdentity, *changedTextureIdentity);
    EXPECT_EQ(*fallbackOneIdentity, *fallbackTwoIdentity);
}

TEST_F(GPUDrivenValidationFixture,
       RasterTranscriptFailsClosedWhenSemanticIdentityIsUnavailable)
{
    FakeDevice device;
    const std::array<GPUInstanceData, 1> instances{{
        GPUInstanceData{}}};
    const std::array<RasterInstanceStreamKey, 1> keys{{{7003u, 0u}}};
    RasterInstanceStreamCache cache;
    RasterInstanceStream stream;
    ASSERT_TRUE(CreateRasterInstanceStream(
        device,
        instances,
        keys,
        "RasterTranscriptUnavailable",
        cache,
        stream));
    ASSERT_TRUE(stream.IsValid());

    RasterTranscriptDigest transcript;
    EXPECT_TRUE(BuildRasterInstanceStreamTranscript(
        stream, cache, transcript));
    EXPECT_FALSE(transcript.available);
    EXPECT_EQ(0u, transcript.entryCount);
}

TEST_F(GPUDrivenValidationFixture,
       RasterSemanticIdentityCanonicalizesSignedZeroMaterialFields)
{
    MaterialSourceData positiveZero;
    positiveZero.metallicFactor = 0.0f;
    positiveZero.alphaCutoff = 0.0f;
    MaterialSourceData negativeZero = positiveZero;
    negativeZero.metallicFactor = -0.0f;
    negativeZero.alphaCutoff = -0.0f;

    RasterSemanticRegistry first;
    RasterSemanticRegistry second;
    ASSERT_TRUE(first.AddReadyMesh({101u, 1u}, AssetId{3101u}));
    ASSERT_TRUE(first.AddReadyMaterial(
        {102u, 1u}, AssetId{3201u}, positiveZero, {}));
    ASSERT_TRUE(second.AddReadyMesh({111u, 2u}, AssetId{3101u}));
    ASSERT_TRUE(second.AddReadyMaterial(
        {112u, 2u}, AssetId{3201u}, negativeZero, {}));

    const std::optional<uint64> firstIdentity =
        first.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            {101u, 1u}, 0xF501u);
    const std::optional<uint64> secondIdentity =
        second.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            {111u, 2u}, 0xF501u);
    ASSERT_TRUE(firstIdentity.has_value());
    ASSERT_TRUE(secondIdentity.has_value());
    EXPECT_EQ(*firstIdentity, *secondIdentity);
}

TEST_F(GPUDrivenValidationFixture,
       RasterSemanticSidecarDoesNotPatchGpuRowsAndRemainsSlotLocal)
{
    FakeDevice device;
    RasterInstanceStreamCache cache;
    RasterInstanceStream stream;
    const std::array<GPUInstanceData, 2> instances{{
        MakeInstance(Vec3(-1.0f, 0.0f, -5.0f), 1.0f, 36u),
        MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 36u)}};
    const std::array<RasterInstanceStreamKey, 2> keys{{{8101u, 0u}, {8102u, 0u}}};
    const std::array<uint64, 2> firstSemantics{{0x8101000000000001ull,
                                                  0x8102000000000001ull}};
    ASSERT_TRUE(CreateRasterInstanceStream(
        device, instances, keys, "RasterSemanticSidecar", cache, stream));
    ASSERT_TRUE(stream.IsValid());
    ASSERT_TRUE(FinalizeRasterInstanceStreamSemanticSidecar(
        stream, cache, std::vector<uint64>(
            firstSemantics.begin(), firstSemantics.end())));
    EXPECT_EQ(0u, instances[0].padding[0]);
    EXPECT_EQ(0u, instances[0].padding[1]);
    RasterTranscriptDigest firstDigest;
    ASSERT_TRUE(BuildRasterInstanceStreamTranscript(stream, cache, firstDigest));
    ASSERT_TRUE(firstDigest.available);
    const DirectRasterMutationTotals totalsBeforeSemanticOnly =
        cache.mutationTotals;
    const uint64 instanceUploadBytesBeforeSemanticOnly =
        stream.instanceUploadBytes;
    const uint64 indexUploadBytesBeforeSemanticOnly =
        stream.indexUploadBytes;
    const uint32 instancePatchedRowsBeforeSemanticOnly =
        stream.instancePatchedRowCount;
    const uint32 indexPatchedRowsBeforeSemanticOnly =
        stream.indexPatchedRowCount;
    FakeBuffer* const instanceBuffer =
        static_cast<FakeBuffer*>(stream.instances.Get());
    ASSERT_NE(nullptr, instanceBuffer);
    const uint32 mapsBeforeSemanticOnly = instanceBuffer->GetMapCallCount();

    const std::array<uint64, 2> changedSemantics{{0x8101000000000002ull,
                                                    0x8102000000000001ull}};
    ASSERT_TRUE(FinalizeRasterInstanceStreamSemanticSidecar(
        stream, cache, std::vector<uint64>(
            changedSemantics.begin(), changedSemantics.end())));
    EXPECT_EQ(instanceUploadBytesBeforeSemanticOnly,
              stream.instanceUploadBytes);
    EXPECT_EQ(indexUploadBytesBeforeSemanticOnly,
              stream.indexUploadBytes);
    EXPECT_EQ(instancePatchedRowsBeforeSemanticOnly,
              stream.instancePatchedRowCount);
    EXPECT_EQ(indexPatchedRowsBeforeSemanticOnly,
              stream.indexPatchedRowCount);
    EXPECT_EQ(mapsBeforeSemanticOnly, instanceBuffer->GetMapCallCount());
    EXPECT_EQ(totalsBeforeSemanticOnly.instanceUploadBytes,
              cache.mutationTotals.instanceUploadBytes);
    EXPECT_EQ(totalsBeforeSemanticOnly.instancePatchedRowCount,
              cache.mutationTotals.instancePatchedRowCount);
    RasterTranscriptDigest changedDigest;
    ASSERT_TRUE(BuildRasterInstanceStreamTranscript(stream, cache, changedDigest));
    ASSERT_TRUE(changedDigest.available);
    EXPECT_NE(firstDigest.orderedIdentityHash, changedDigest.orderedIdentityHash);
    EXPECT_NE(firstDigest.consumedPayloadHash, changedDigest.consumedPayloadHash);

    const std::array<GPUInstanceData, 2> churnInstances{{
        instances[1], MakeInstance(Vec3(3.0f, 0.0f, -5.0f), 1.0f, 36u)}};
    const std::array<RasterInstanceStreamKey, 2> churnKeys{{{8102u, 0u},
                                                              {8103u, 0u}}};
    const std::array<uint64, 2> churnSemantics{{0x8102000000000002ull,
                                                 0x8103000000000001ull}};
    ASSERT_TRUE(CreateRasterInstanceStream(
        device, churnInstances, churnKeys, "RasterSemanticSidecar", cache, stream));
    const RasterInstanceStreamFrameSlot& churnSlot = cache.frameSlots[0];
    ASSERT_TRUE(churnSlot.IsValid());
    std::vector<uint64> churnResidentSemantics(churnSlot.instanceCapacity, 0);
    uint32 churnSemanticCount = 0;
    for (uint32 row = 0; row < churnSlot.instanceCapacity; ++row)
    {
        if (churnSlot.residentKeys[row] == churnKeys[0])
        {
            churnResidentSemantics[row] = churnSemantics[0];
            ++churnSemanticCount;
        }
        else if (churnSlot.residentKeys[row] == churnKeys[1])
        {
            churnResidentSemantics[row] = churnSemantics[1];
            ++churnSemanticCount;
        }
    }
    ASSERT_EQ(churnKeys.size(), churnSemanticCount);
    ASSERT_TRUE(FinalizeRasterInstanceStreamSemanticSidecar(
        stream, cache, std::move(churnResidentSemantics)));
    const RasterInstanceStreamFrameSlot& slotZero = cache.frameSlots[0];
    ASSERT_TRUE(slotZero.IsValid());
    for (uint32 row = 0; row < slotZero.instanceCapacity; ++row)
    {
        if (slotZero.residentKeys[row] == churnKeys[0])
        {
            EXPECT_EQ(churnSemantics[0], slotZero.residentRasterSemanticIdentities[row]);
        }
        if (slotZero.residentKeys[row] == churnKeys[1])
        {
            EXPECT_EQ(churnSemantics[1], slotZero.residentRasterSemanticIdentities[row]);
        }
    }

    device.SetCurrentFrameIndex(1u);
    const std::array<GPUInstanceData, 1> rotatedInstances{{churnInstances[0]}};
    const std::array<RasterInstanceStreamKey, 1> rotatedKeys{{churnKeys[0]}};
    const std::array<uint64, 1> rotatedSemantics{{0x8102000000000003ull}};
    ASSERT_TRUE(CreateRasterInstanceStream(
        device, rotatedInstances, rotatedKeys, "RasterSemanticSidecar", cache, stream));
    ASSERT_TRUE(FinalizeRasterInstanceStreamSemanticSidecar(
        stream, cache, std::vector<uint64>(
            rotatedSemantics.begin(), rotatedSemantics.end())));
    const RasterInstanceStreamFrameSlot& slotOne = cache.frameSlots[1];
    ASSERT_TRUE(slotOne.IsValid());
    EXPECT_NE(slotZero.residentRasterSemanticIdentities,
              slotOne.residentRasterSemanticIdentities);
}

TEST_F(GPUDrivenValidationFixture,
       TierOneSemanticSidecarIsCpuOnlyAndFrozenOnlyWhenQualificationIsArmed)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);
    const GPUInstanceData instance =
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36u);
    const std::array<uint64, 1> firstSemantic{{0xA001u}};
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(instance));
    culling.EndFrame();
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::
        SetCollectedRasterSemanticIdentities(culling, firstSemantic));
    const std::shared_ptr<GPUCullingRecordedState> first = culling.SealForGraph(
        GPUCullingRecordingIdentity{901u, 1u, 1u, 0u, 1u});
    ASSERT_NE(nullptr, first);
    const std::array<uint64, 3> versionsBefore =
        RVX::GPUCullingQualificationTestAccess::GetCanonicalVersions(culling);
    FakeBuffer* const residentBuffer =
        static_cast<FakeBuffer*>(culling.GetInstanceBuffer());
    ASSERT_NE(nullptr, residentBuffer);
    const uint32 mapsBeforeSemanticOnly = residentBuffer->GetMapCallCount();

    const std::array<uint64, 1> secondSemantic{{0xA002u}};
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(instance));
    culling.EndFrame();
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::
        SetCollectedRasterSemanticIdentities(culling, secondSemantic));
    const std::shared_ptr<GPUCullingRecordedState> ordinary = culling.SealForGraph(
        GPUCullingRecordingIdentity{901u, 2u, 2u, 0u, 2u});
    ASSERT_NE(nullptr, ordinary);
    EXPECT_EQ(0u, culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(0u, culling.GetIncrementalDiagnostics().instancePatchedRowCount);
    EXPECT_EQ(mapsBeforeSemanticOnly, residentBuffer->GetMapCallCount());
    EXPECT_EQ(versionsBefore,
              RVX::GPUCullingQualificationTestAccess::GetCanonicalVersions(culling));
    EXPECT_EQ(0u, ordinary->GetCopiedCpuPayloadBytes());
    EXPECT_EQ(nullptr, device.FindBuffer(
        "GPUCulling.GPUSceneQualificationVisibilityReadback"));

    const auto captureTranscript = [config](uint64 semanticIdentity)
        -> GPUSceneCullingQualificationDiagnostics
    {
        FakeDevice captureDevice;
        captureDevice.EnableTimelineRetirement();
        captureDevice.EnableGPUScenePipelineObjects();
        RenderSubmissionTracker tracker;
        EXPECT_TRUE(tracker.Initialize(&captureDevice));
        GPUCulling captureCulling;
        captureCulling.Initialize(&captureDevice, config);
        captureCulling.BeginFrame();
        EXPECT_EQ(0u, captureCulling.AddInstance(
            MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36u)));
        captureCulling.EndFrame();
        const std::array<uint64, 1> semantic{{semanticIdentity}};
        EXPECT_TRUE(RVX::GPUCullingQualificationTestAccess::
            SetCollectedRasterSemanticIdentities(captureCulling, semantic));
        EXPECT_TRUE(captureCulling.ArmGPUSceneQualificationCapture());
        const std::array<uint64, 1> coverage{{0xA0F0u}};
        EXPECT_TRUE(RVX::GPUCullingQualificationTestAccess::ConfigureInputCoverage(
            captureCulling, coverage, coverage));
        captureCulling.PublishGPUSceneQualificationRequiredLane(
            GPUDrivenTier::IndirectGrouped);
        const std::shared_ptr<GPUCullingRecordedState> recorded =
            captureCulling.SealForGraph(
                GPUCullingRecordingIdentity{902u, semanticIdentity, 3u, 0u, 3u});
        if (!recorded)
        {
            return captureCulling.GetGPUSceneQualificationDiagnostics();
        }
        FakeCommandContext context;
        recorded->Cull(context, TestView(), TestProjection());
        EXPECT_TRUE(RVX::GPUCullingQualificationTestAccess::WriteExpectedReadback(
            recorded->GetCulling()));
        GPUCompletionToken completion;
        const GPUCompletionPoint point = tracker.Submit(&context);
        EXPECT_TRUE(InsertGPUCompletionPoint(completion, point));
        EXPECT_TRUE(recorded->NotifyGPUSceneQualificationSubmission(
            captureCulling, completion, tracker));
        FakeFence* const fence = captureDevice.GetFence(0);
        if (fence != nullptr)
        {
            fence->Complete(point.value);
        }
        static_cast<void>(captureCulling.PollGPUSceneQualificationCapture(tracker));
        return captureCulling.GetGPUSceneQualificationDiagnostics();
    };

    const GPUSceneCullingQualificationDiagnostics semanticA =
        captureTranscript(0xA003u);
    const GPUSceneCullingQualificationDiagnostics semanticB =
        captureTranscript(0xA004u);
    ASSERT_TRUE(semanticA.tierOneRasterTranscriptMatched);
    ASSERT_TRUE(semanticB.tierOneRasterTranscriptMatched);
    ASSERT_TRUE(semanticA.tierOneRasterTranscript.available);
    ASSERT_TRUE(semanticB.tierOneRasterTranscript.available);
    EXPECT_NE(semanticA.tierOneRasterTranscript.orderedIdentityHash,
              semanticB.tierOneRasterTranscript.orderedIdentityHash);
    EXPECT_NE(semanticA.tierOneRasterTranscript.consumedPayloadHash,
              semanticB.tierOneRasterTranscript.consumedPayloadHash);
}

TEST_F(GPUDrivenValidationFixture,
       DirectTranscriptUsesPlannedMixedOrderAndResolvedSemanticEvidence)
{
    FakeDevice device;
    RasterSemanticRegistry registry;
    const RenderResourceHandle mesh{821u, 1u};
    ASSERT_TRUE(registry.AddReadyMesh(mesh, AssetId{0x8211u}));

    RenderScene scene;
    const auto addObject = [&scene, mesh](uint64 objectId,
                                          const Vec3& center)
    {
        RenderObject object = MakeRenderObject(center, 1.0f, mesh.slot);
        object.entityId = objectId;
        object.mesh = mesh;
        object.material = {static_cast<uint32>(objectId + 1000u), 1u};
        scene.AddObject(object);
    };
    addObject(100u, Vec3(-3.0f, 0.0f, -5.0f)); // instanced prelude
    addObject(101u, Vec3(-1.0f, 0.0f, -5.0f)); // first singleton
    addObject(102u, Vec3(3.0f, 0.0f, -5.0f));  // second singleton

    const auto makePacket = [mesh](RenderObjectId objectId,
                                   PrimitiveDataIndex primitiveData,
                                   RenderResourceHandle material,
                                   uint32 indexCount,
                                   uint32 firstIndex,
                                   int32 vertexOffset) -> DirectDrawPacket
    {
        DirectDrawPacket direct;
        direct.packet.objectId = objectId;
        direct.packet.primitiveData = primitiveData;
        direct.packet.submeshIndex = 0u;
        direct.packet.pass = RenderPassKind::Opaque;
        direct.packet.geometryKey.mesh = mesh;
        direct.packet.materialKey.material = material;
        direct.packet.arguments.indexCount = indexCount;
        direct.packet.arguments.firstIndex = firstIndex;
        direct.packet.arguments.vertexOffset = vertexOffset;
        direct.packet.arguments.instanceCount = 1u;
        return direct;
    };

    DirectDrawPacket prelude = makePacket(
        100u, 0u, {1100u, 1u}, 18u, 2u, -1);
    DirectDrawPacket singleFirst = makePacket(
        101u, 1u, {1101u, 1u}, 24u, 4u, 0);
    DirectDrawPacket instanced = makePacket(
        200u, RVX_INVALID_PRIMITIVE_DATA_INDEX, {7u, 1u}, 36u, 6u, 3);
    DirectDrawPacket singleLast = makePacket(
        102u, 2u, {1102u, 1u}, 12u, 8u, -2);

    const RenderDrawGroupKey preludeKey = MakeRenderInstanceBatchKey(
        prelude.packet, prelude.layout);
    const RenderDrawGroupKey instancedKey = MakeRenderInstanceBatchKey(
        instanced.packet, instanced.layout);
    std::array<GPUInstanceData, 3> instances{{
        MakeInstance(Vec3(-3.0f, 0.0f, -5.0f), 1.0f, 18u),
        MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 36u),
        MakeInstance(Vec3(2.0f, 0.0f, -5.0f), 1.0f, 36u)}};
    instances[0].meshId = mesh.slot;
    instances[0].materialId = RVX_INVALID_INDEX;
    instances[1].meshId = mesh.slot;
    instances[1].materialId = 7u;
    instances[2].meshId = mesh.slot;
    instances[2].materialId = 7u;
    const std::array<RasterInstanceStreamKey, 3> keys{{
        {100u, 0u}, {301u, 0u}, {300u, 0u}}};
    const std::array<RasterInstanceStreamBatch, 2> batches{{
        {preludeKey, 0u, 1u}, {instancedKey, 1u, 2u}}};
    auto cache = std::make_shared<RasterInstanceStreamCache>();
    RasterInstanceStream stream;
    ASSERT_TRUE(CreateRasterInstanceStream(
        device, instances, keys, batches, "OpaqueTranscriptMixed", *cache,
        stream));
    ASSERT_TRUE(stream.IsValid());
    ASSERT_EQ(2u, stream.batchBindings.size());
    const RasterInstanceStreamBatchBinding& preludeBinding =
        stream.batchBindings[0];
    const RasterInstanceStreamBatchBinding& instancedBinding =
        stream.batchBindings[1];
    ASSERT_EQ(preludeKey, preludeBinding.key);
    ASSERT_EQ(instancedKey, instancedBinding.key);
    ASSERT_NE(0u, instancedBinding.firstInstance);
    ASSERT_EQ(2u, instancedBinding.instanceCount);
    const RasterInstanceStreamFrameSlot& resident = cache->frameSlots[
        stream.activeFrameSlot];
    ASSERT_TRUE(resident.IsValid());
    const uint32 firstInstancedResidentRow = resident.residentDrawOrder[
        instancedBinding.firstInstance];
    ASSERT_LT(firstInstancedResidentRow, resident.residentKeys.size());
    // Input order is 301 then 300. Persistent batch residency canonicalizes
    // it to 300 then 301, so transcript must follow the physical draw order.
    EXPECT_EQ(300u, resident.residentKeys[firstInstancedResidentRow].objectId);

    prelude.packet.arguments.firstInstance = preludeBinding.firstInstance;
    instanced.packet.arguments.firstInstance = instancedBinding.firstInstance;
    instanced.packet.arguments.instanceCount = instancedBinding.instanceCount;

    constexpr uint64 preludeMaterialKey = 0x82100001ull;
    constexpr uint64 firstSingletonMaterialKey = 0x82100002ull;
    constexpr uint64 lastSingletonMaterialKey = 0x82100003ull;
    constexpr uint64 tableMaterialKey = 0x82100004ull;
    std::vector<uint64> tableKeys(8u, 0u);
    tableKeys[7u] = tableMaterialKey;
    const auto publishInstancedEvidence = [&](uint64 tableKey) -> bool
    {
        const std::optional<uint64> preludeSemantic =
            registry.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
                mesh, preludeMaterialKey);
        const std::optional<uint64> tableSemantic =
            registry.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
                mesh, tableKey);
        if (!preludeSemantic || !tableSemantic)
        {
            return false;
        }
        std::vector<uint64> semanticIdentities(resident.instanceCapacity, 0u);
        const uint32 preludeRow = resident.residentDrawOrder[
            preludeBinding.firstInstance];
        if (preludeRow >= semanticIdentities.size())
        {
            return false;
        }
        semanticIdentities[preludeRow] = *preludeSemantic;
        for (uint32 offset = 0; offset < instancedBinding.instanceCount;
             ++offset)
        {
            const uint32 residentRow = resident.residentDrawOrder[
                instancedBinding.firstInstance + offset];
            if (residentRow >= semanticIdentities.size())
            {
                return false;
            }
            semanticIdentities[residentRow] = *tableSemantic;
        }
        return FinalizeRasterInstanceStreamSemanticSidecar(
            stream, *cache, std::move(semanticIdentities));
    };
    ASSERT_TRUE(publishInstancedEvidence(tableMaterialKey));

    const std::array<DirectRasterTranscriptPlannedDraw, 4> sourceOrder{{
        {prelude, preludeMaterialKey, false, 1u, true},
        {singleFirst, firstSingletonMaterialKey, false, 1u, false},
        {instanced, 0u, true, 2u, true},
        {singleLast, lastSingletonMaterialKey, false, 1u, false}}};

    RasterTranscriptDigest sourceDigest;
    ASSERT_TRUE(BuildCompleteDirectRasterTranscript(
        sourceOrder, scene, registry.GetRegistry(), &stream, cache.get(),
        tableKeys, sourceDigest));
    ASSERT_TRUE(sourceDigest.available);
    const uint32 representedPacketCount = sourceOrder[0].representedPacketCount +
        sourceOrder[1].representedPacketCount +
        sourceOrder[2].representedPacketCount +
        sourceOrder[3].representedPacketCount;
    EXPECT_EQ(representedPacketCount, sourceDigest.entryCount);

    const std::optional<uint64> expectedTableSemantic =
        registry.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            mesh, tableMaterialKey);
    ASSERT_TRUE(expectedTableSemantic.has_value());
    for (uint32 offset = 0; offset < instancedBinding.instanceCount; ++offset)
    {
        const uint32 residentRow = resident.residentDrawOrder[
            instancedBinding.firstInstance + offset];
        ASSERT_LT(residentRow, resident.residentRasterSemanticIdentities.size());
        EXPECT_EQ(*expectedTableSemantic,
                  resident.residentRasterSemanticIdentities[residentRow]);
    }

    const std::array<DirectRasterTranscriptPlannedDraw, 4> reordered{{
        sourceOrder[0], sourceOrder[3], sourceOrder[2], sourceOrder[1]}};
    RasterTranscriptDigest reorderedDigest;
    ASSERT_TRUE(BuildCompleteDirectRasterTranscript(
        reordered, scene, registry.GetRegistry(), &stream, cache.get(),
        tableKeys, reorderedDigest));
    ASSERT_TRUE(reorderedDigest.available);
    EXPECT_EQ(sourceDigest.entryCount, reorderedDigest.entryCount);
    EXPECT_NE(sourceDigest.orderedIdentityHash,
              reorderedDigest.orderedIdentityHash);
    EXPECT_NE(sourceDigest.consumedPayloadHash,
              reorderedDigest.consumedPayloadHash);
    EXPECT_EQ(sourceDigest.unorderedIdentityHash,
              reorderedDigest.unorderedIdentityHash);
    EXPECT_EQ(sourceDigest.unorderedIdentityHashSecondary,
              reorderedDigest.unorderedIdentityHashSecondary);
    EXPECT_EQ(sourceDigest.unorderedConsumedPayloadHash,
              reorderedDigest.unorderedConsumedPayloadHash);
    EXPECT_EQ(sourceDigest.unorderedConsumedPayloadHashSecondary,
              reorderedDigest.unorderedConsumedPayloadHashSecondary);

    auto changedSingle = sourceOrder;
    changedSingle[1].fixedRasterMaterialKey = 0x82100012ull;
    RasterTranscriptDigest changedSingleDigest;
    ASSERT_TRUE(BuildCompleteDirectRasterTranscript(
        changedSingle, scene, registry.GetRegistry(), &stream, cache.get(),
        tableKeys, changedSingleDigest));
    EXPECT_NE(sourceDigest.orderedIdentityHash,
              changedSingleDigest.orderedIdentityHash);

    std::vector<uint64> changedTableKeys = tableKeys;
    changedTableKeys[7u] = 0x82100014ull;
    ASSERT_TRUE(publishInstancedEvidence(changedTableKeys[7u]));
    RasterTranscriptDigest changedTableDigest;
    ASSERT_TRUE(BuildCompleteDirectRasterTranscript(
        sourceOrder, scene, registry.GetRegistry(), &stream, cache.get(),
        changedTableKeys, changedTableDigest));
    EXPECT_NE(sourceDigest.orderedIdentityHash,
              changedTableDigest.orderedIdentityHash);
}

TEST_F(GPUDrivenValidationFixture,
       TierOneRasterSemanticEvidenceFinalizesThroughProductionCombiner)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4u;
    RasterSemanticRegistry registry;
    const RenderResourceHandle mesh{811u, 1u};
    ASSERT_TRUE(registry.AddReadyMesh(mesh, AssetId{0x8111u}));

    GPUCulling culling;
    culling.Initialize(&device, config);
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.BeginDrawGroup(
        811u, 0u, MaterialPipelineVariant::Opaque, mesh));
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36u)));
    culling.EndDrawGroup();
    culling.EndFrame();
    const std::array<uint64, 1> collected{{0x81100001u}};
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::
        SetCollectedRasterSemanticIdentities(culling, collected));
    ASSERT_TRUE(culling.ArmGPUSceneQualificationCapture());
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::ConfigureInputCoverage(
        culling, collected, collected));
    culling.PublishGPUSceneQualificationRequiredLane(
        GPUDrivenTier::IndirectGrouped);
    const std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGraph(GPUCullingRecordingIdentity{811u, 1u, 1u, 0u, 1u});
    ASSERT_NE(nullptr, recorded);

    const std::array<uint64, 1> fixedKeys{{0x81100002u}};
    const std::optional<uint64> expected =
        registry.GetRegistry().CombineRasterMeshAndMaterialSemanticIdentity(
            mesh, fixedKeys.front());
    ASSERT_TRUE(expected.has_value());
    const std::array<uint64, 3> versionsBefore =
        RVX::GPUCullingQualificationTestAccess::GetCanonicalVersions(
            recorded->GetCulling());
    ASSERT_TRUE(recorded->FinalizeTierOneRasterSemanticEvidence(
        fixedKeys, {}, registry.GetRegistry()));
    EXPECT_TRUE(RVX::GPUCullingQualificationTestAccess::
        IsRasterSemanticEvidenceFinalized(recorded->GetCulling()));
    EXPECT_EQ((std::vector<uint64>{*expected}),
              RVX::GPUCullingQualificationTestAccess::
                  GetCapturedRasterSemanticIdentities(recorded->GetCulling()));
    EXPECT_EQ(versionsBefore,
              RVX::GPUCullingQualificationTestAccess::GetCanonicalVersions(
                  recorded->GetCulling()));

    GPUCulling failingCulling;
    failingCulling.Initialize(&device, config);
    failingCulling.BeginFrame();
    ASSERT_EQ(0u, failingCulling.BeginDrawGroup(
        811u, 0u, MaterialPipelineVariant::Opaque, mesh));
    ASSERT_EQ(0u, failingCulling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36u)));
    failingCulling.EndDrawGroup();
    failingCulling.EndFrame();
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::
        SetCollectedRasterSemanticIdentities(failingCulling, collected));
    ASSERT_TRUE(failingCulling.ArmGPUSceneQualificationCapture());
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::ConfigureInputCoverage(
        failingCulling, collected, collected));
    failingCulling.PublishGPUSceneQualificationRequiredLane(
        GPUDrivenTier::IndirectGrouped);
    const std::shared_ptr<GPUCullingRecordedState> failingRecorded =
        failingCulling.SealForGraph(
            GPUCullingRecordingIdentity{811u, 2u, 2u, 0u, 2u});
    ASSERT_NE(nullptr, failingRecorded);
    const std::vector<uint64> sidecarBefore =
        RVX::GPUCullingQualificationTestAccess::
            GetCapturedRasterSemanticIdentities(failingRecorded->GetCulling());
    const std::array<uint64, 3> failureVersionsBefore =
        RVX::GPUCullingQualificationTestAccess::GetCanonicalVersions(
            failingRecorded->GetCulling());
    const std::array<uint64, 1> missingKeys{{0u}};
    EXPECT_FALSE(failingRecorded->FinalizeTierOneRasterSemanticEvidence(
        missingKeys, {}, registry.GetRegistry()));
    EXPECT_FALSE(RVX::GPUCullingQualificationTestAccess::
        IsRasterSemanticEvidenceFinalized(failingRecorded->GetCulling()));
    EXPECT_EQ(sidecarBefore,
              RVX::GPUCullingQualificationTestAccess::
                  GetCapturedRasterSemanticIdentities(
                      failingRecorded->GetCulling()));
    EXPECT_EQ(failureVersionsBefore,
              RVX::GPUCullingQualificationTestAccess::GetCanonicalVersions(
                  failingRecorded->GetCulling()));
    EXPECT_EQ(GPUSceneCullingQualificationMismatch::ReferenceUnavailable,
              failingRecorded->GetCulling()
                  .GetGPUSceneQualificationDiagnostics().mismatch);
}

TEST_F(GPUDrivenValidationFixture,
       RasterInstanceStreamInvalidatesWholeSlotForFirstMiddleAndFinalRangeFailure)
{
    const std::array<RasterInstanceStreamKey, 5> keys{{
        {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}}};
    const std::array<uint64, 5> semanticIdentities{{1u, 2u, 3u, 4u, 5u}};
    for (int32 failingRange = 0; failingRange < 3; ++failingRange)
    {
        SCOPED_TRACE(failingRange);
        FakeDevice device;
        std::array<GPUInstanceData, 5> instances{};
        RasterInstanceStream stream;
        ASSERT_TRUE(CreateRasterInstanceStream(
            device, instances, keys, "RasterRangeFailure", stream,
            semanticIdentities));
        auto* const instanceBuffer = static_cast<FakeBuffer*>(stream.instances.Get());
        ASSERT_NE(nullptr, instanceBuffer);

        instances[0].indexCount = 1;
        instances[2].indexCount = 2;
        instances[4].indexCount = 3;
        instanceBuffer->SetCommitFailureCountdown(failingRange);
        EXPECT_FALSE(CreateRasterInstanceStream(
            device, instances, keys, "RasterRangeFailure", stream));
        EXPECT_FALSE(stream.IsValid());
        EXPECT_TRUE(stream.frameSlots[0].residentRasterSemanticIdentities.empty());
        EXPECT_EQ(nullptr, stream.instances.Get());
        EXPECT_EQ(nullptr, stream.instanceIndices.Get());
        EXPECT_EQ(0U, stream.instanceUploadBytes);
        EXPECT_EQ(0U, stream.indexUploadBytes);
        EXPECT_EQ(static_cast<uint32>(failingRange + 1),
                  stream.instanceUploadWork.mappedRangeCount);
        EXPECT_EQ(static_cast<uint32>(failingRange),
                  stream.instanceUploadWork.committedRangeCount);

        ASSERT_TRUE(CreateRasterInstanceStream(
            device, instances, keys, "RasterRangeFailure", stream,
            semanticIdentities));
        ASSERT_TRUE(stream.IsValid());
        EXPECT_EQ(stream.activeInstanceCapacity,
                  stream.frameSlots[0].residentRasterSemanticIdentities.size());
        EXPECT_TRUE(stream.instanceFullMaterialization);
        EXPECT_EQ(5U * sizeof(GPUInstanceData), stream.instanceUploadBytes);
        EXPECT_EQ(1U, stream.instanceUploadWork.mappedRangeCount);
        EXPECT_EQ(1U, stream.instanceUploadWork.committedRangeCount);
    }
}

TEST_F(GPUDrivenValidationFixture,
       RasterInstanceStreamKeepsHostReceiptAvailabilityFailClosedAcrossRanges)
{
    FakeDevice device;
    std::array<GPUInstanceData, 3> instances{};
    const std::array<RasterInstanceStreamKey, 3> keys{{{1, 0}, {2, 0}, {3, 0}}};
    RasterInstanceStream stream;
    ASSERT_TRUE(CreateRasterInstanceStream(
        device, instances, keys, "RasterMixedReceiptEvidence", stream));
    auto* const instanceBuffer = static_cast<FakeBuffer*>(stream.instances.Get());
    ASSERT_NE(nullptr, instanceBuffer);

    // ExactRange without a synchronized range is a published write but has
    // no quantitative visibility evidence. A later coherent receipt for a
    // different dirty range must not turn the aggregate back to Available.
    instanceBuffer->SetSynchronizationSequence({
        RHIHostWriteSynchronization::ExactRange,
        RHIHostWriteSynchronization::CoherentNoExplicitSync});
    instances[0].indexCount = 1;
    instances[2].indexCount = 3;
    ASSERT_TRUE(CreateRasterInstanceStream(
        device, instances, keys, "RasterMixedReceiptEvidence", stream));

    const RenderUploadWorkDiagnostics& work = stream.instanceUploadWork;
    EXPECT_EQ(2U, work.mappedRangeCount);
    EXPECT_EQ(2U, work.committedRangeCount);
    EXPECT_FALSE(work.hostVisibilitySynchronizedBytes.IsAvailable());
    EXPECT_EQ(1U, work.hostVisibilitySynchronizationScopeRangeCounts[
                      static_cast<uint32>(
                          RenderHostWriteSynchronizationScope::ExactRange)]);
    EXPECT_EQ(1U, work.hostVisibilitySynchronizationScopeRangeCounts[
                      static_cast<uint32>(
                          RenderHostWriteSynchronizationScope::CoherentNoExplicitSync)]);
}

TEST_F(GPUDrivenValidationFixture,
       FrameSlotsIsolatePersistentInputsAndRestoreTheirAccessSnapshots)
{
    FakeDevice device;

    GPUCullingConfig config;
    config.maxInstances = 4;

    GPUCulling culling;
    culling.Initialize(&device, config, 2);
    ASSERT_TRUE(culling.IsInitialized());
    ASSERT_EQ(2u, culling.GetFrameSlotCount());

    ASSERT_TRUE(culling.SetFrameSlot(0));
    RHIBuffer* const slot0InstanceBuffer = culling.GetInstanceBuffer();
    RHIBuffer* const slot0ConstantsBuffer = culling.GetCullingConstantsBuffer();
    ASSERT_NE(nullptr, slot0InstanceBuffer);
    ASSERT_NE(nullptr, slot0ConstantsBuffer);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{11u, 1u, 1u, 0u, 1u}));
    const GPUInstanceData slot0InitialInstance = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(slot0InstanceBuffer));

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());
    const Mat4 slot0InitialConstants = ReadBufferValue<Mat4>(
        *static_cast<const FakeBuffer*>(slot0ConstantsBuffer));

    GPUCullingAccessSnapshots slot0Snapshots = culling.GetAccessSnapshots();
    slot0Snapshots.instances = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ShaderResource,
        RHIShaderStage::Vertex,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Valid);
    slot0Snapshots.constants = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ConstantBuffer,
        RHIShaderStage::Compute,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Valid);
    culling.CommitAccessSnapshots(slot0Snapshots);

    ASSERT_TRUE(culling.SetFrameSlot(1));
    RHIBuffer* const slot1InstanceBuffer = culling.GetInstanceBuffer();
    RHIBuffer* const slot1ConstantsBuffer = culling.GetCullingConstantsBuffer();
    ASSERT_NE(nullptr, slot1InstanceBuffer);
    ASSERT_NE(nullptr, slot1ConstantsBuffer);
    EXPECT_NE(slot0InstanceBuffer, slot1InstanceBuffer);
    EXPECT_NE(slot0ConstantsBuffer, slot1ConstantsBuffer);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(9.0f, 0.0f, -5.0f), 1.0f, 24)));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{12u, 1u, 2u, 0u, 1u}));
    const GPUInstanceData slot1InitialInstance = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(slot1InstanceBuffer));

    const Mat4 slot1View = lookAt(Vec3(2.0f, 0.0f, 0.0f),
                                  Vec3(2.0f, 0.0f, -1.0f),
                                  Vec3(0.0f, 1.0f, 0.0f));
    culling.Cull(context, slot1View, TestProjection());
    const Mat4 slot1InitialConstants = ReadBufferValue<Mat4>(
        *static_cast<const FakeBuffer*>(slot1ConstantsBuffer));

    GPUCullingAccessSnapshots slot1Snapshots = culling.GetAccessSnapshots();
    slot1Snapshots.instances = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ShaderResource,
        RHIShaderStage::Compute,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Valid);
    slot1Snapshots.constants = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ConstantBuffer,
        RHIShaderStage::All,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Valid);
    culling.CommitAccessSnapshots(slot1Snapshots);

    ASSERT_TRUE(culling.SetFrameSlot(0));
    EXPECT_EQ(slot0InstanceBuffer, culling.GetInstanceBuffer());
    EXPECT_EQ(slot0ConstantsBuffer, culling.GetCullingConstantsBuffer());
    EXPECT_EQ(slot0Snapshots.instances, culling.GetAccessSnapshots().instances);
    EXPECT_EQ(slot0Snapshots.constants, culling.GetAccessSnapshots().constants);
    const GPUInstanceData slot0InstanceAfterSlot1 = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(slot0InstanceBuffer));
    const Mat4 slot0ConstantsAfterSlot1 = ReadBufferValue<Mat4>(
        *static_cast<const FakeBuffer*>(slot0ConstantsBuffer));
    EXPECT_EQ(0, std::memcmp(&slot0InitialInstance,
                             &slot0InstanceAfterSlot1,
                             sizeof(GPUInstanceData)));
    EXPECT_EQ(0, std::memcmp(&slot0InitialConstants,
                             &slot0ConstantsAfterSlot1,
                             sizeof(Mat4)));

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(-4.0f, 0.0f, -5.0f), 1.0f, 12)));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{13u, 1u, 3u, 0u, 1u}));
    const GPUInstanceData slot0WrappedInstance = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(slot0InstanceBuffer));
    EXPECT_NE(0, std::memcmp(&slot0InitialInstance,
                             &slot0WrappedInstance,
                             sizeof(GPUInstanceData)));

    ASSERT_TRUE(culling.SetFrameSlot(1));
    EXPECT_EQ(slot1InstanceBuffer, culling.GetInstanceBuffer());
    EXPECT_EQ(slot1ConstantsBuffer, culling.GetCullingConstantsBuffer());
    EXPECT_EQ(slot1Snapshots.instances, culling.GetAccessSnapshots().instances);
    EXPECT_EQ(slot1Snapshots.constants, culling.GetAccessSnapshots().constants);
    const GPUInstanceData slot1InstanceAfterSlot0Wrap = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(slot1InstanceBuffer));
    const Mat4 slot1ConstantsAfterSlot0Wrap = ReadBufferValue<Mat4>(
        *static_cast<const FakeBuffer*>(slot1ConstantsBuffer));
    EXPECT_EQ(0, std::memcmp(&slot1InitialInstance,
                             &slot1InstanceAfterSlot0Wrap,
                             sizeof(GPUInstanceData)));
    EXPECT_EQ(0, std::memcmp(&slot1InitialConstants,
                             &slot1ConstantsAfterSlot0Wrap,
                             sizeof(Mat4)));

    const RHIBuffer* const slot1InstanceBeforeInvalidSelect = culling.GetInstanceBuffer();
    const RHIBuffer* const slot1ConstantsBeforeInvalidSelect = culling.GetCullingConstantsBuffer();
    EXPECT_FALSE(culling.SetFrameSlot(2));
    EXPECT_EQ(1u, culling.GetActiveFrameSlot());
    EXPECT_EQ(slot1InstanceBeforeInvalidSelect, culling.GetInstanceBuffer());
    EXPECT_EQ(slot1ConstantsBeforeInvalidSelect, culling.GetCullingConstantsBuffer());
}

TEST_F(GPUDrivenValidationFixture,
       TierOneSealRejectsStalePhysicalSlotBytesAfterMapOrCommitFailure)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 4;

    GPUCulling culling;
    culling.Initialize(&device, config);
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.BeginDrawGroup(11u, 21u));
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(-1.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    const GPUCullingRecordingIdentity firstIdentity{
        21u, 1u, 1u, 0u, 1u};
    ASSERT_NE(nullptr, culling.SealForGraph(firstIdentity));
    EXPECT_EQ(sizeof(GPUInstanceData), culling.GetLastInstanceUploadBytes());
    FakeBuffer* instanceBuffer = static_cast<FakeBuffer*>(
        culling.GetInstanceBuffer());
    ASSERT_NE(nullptr, instanceBuffer);
    EXPECT_EQ(36u, ReadBufferValue<GPUInstanceData>(*instanceBuffer).indexCount);

    // Reusing this physical slot leaves the old bytes in place until the new
    // exact snapshot is resident.  A failed Map must not publish them.
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 12)));
    culling.EndFrame();
    EXPECT_EQ(0U, culling.GetLastInstanceUploadBytes());
    instanceBuffer->SetMapSucceeds(false);
    const GPUCullingRecordingIdentity failedIdentity{
        21u, 2u, 2u, 0u, 2u};
    EXPECT_EQ(nullptr, culling.SealForGraph(failedIdentity));
    EXPECT_EQ(RHIContentValidity::Invalid,
              culling.GetAccessSnapshots().instances
                  .uniformAccess.contentValidity);
    EXPECT_EQ(0U, culling.GetIncrementalDiagnostics()
                     .instanceUploadWork.mappedRangeCount);
    EXPECT_EQ(0U, culling.GetIncrementalDiagnostics()
                     .instanceUploadWork.committedRangeCount);
    EXPECT_FALSE(culling.GetIncrementalDiagnostics()
                     .instanceUploadWork.hostVisibilitySynchronizedBytes
                     .IsAvailable());
    EXPECT_EQ(36u, ReadBufferValue<GPUInstanceData>(*instanceBuffer).indexCount);

    instanceBuffer->SetMapSucceeds(true);
    const std::shared_ptr<GPUCullingRecordedState> retried =
        culling.SealForGraph(failedIdentity);
    ASSERT_NE(nullptr, retried);
    EXPECT_EQ(RHIContentValidity::Valid,
              culling.GetAccessSnapshots().instances
                  .uniformAccess.contentValidity);
    EXPECT_EQ(sizeof(GPUInstanceData), culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(12u, ReadBufferValue<GPUInstanceData>(*instanceBuffer).indexCount);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 6)));
    culling.EndFrame();
    instanceBuffer->SetCommitSucceeds(false);
    const GPUCullingRecordingIdentity commitFailedIdentity{
        21u, 3u, 3u, 0u, 3u};
    EXPECT_EQ(nullptr, culling.SealForGraph(commitFailedIdentity));
    EXPECT_EQ(RHIContentValidity::Invalid,
              culling.GetAccessSnapshots().instances
                  .uniformAccess.contentValidity);
    EXPECT_EQ(0u, culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(1U, culling.GetIncrementalDiagnostics()
                     .instanceUploadWork.mappedRangeCount);
    EXPECT_EQ(0U, culling.GetIncrementalDiagnostics()
                     .instanceUploadWork.committedRangeCount);
    EXPECT_EQ(sizeof(GPUInstanceData), culling.GetIncrementalDiagnostics()
                                            .instanceUploadWork
                                            .cpuCopiedPayloadBytes);
    EXPECT_EQ(0U, culling.GetIncrementalDiagnostics()
                     .instanceUploadWork.committedPayloadBytes);
    EXPECT_EQ(12u, ReadBufferValue<GPUInstanceData>(*instanceBuffer).indexCount);

    instanceBuffer->SetCommitSucceeds(true);
    const std::shared_ptr<GPUCullingRecordedState> commitRetried =
        culling.SealForGraph(commitFailedIdentity);
    ASSERT_NE(nullptr, commitRetried);
    EXPECT_EQ(RHIContentValidity::Valid,
              culling.GetAccessSnapshots().instances
                  .uniformAccess.contentValidity);
    EXPECT_EQ(sizeof(GPUInstanceData), culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(1U, culling.GetIncrementalDiagnostics()
                     .instanceUploadWork.committedRangeCount);
    EXPECT_EQ(6u, ReadBufferValue<GPUInstanceData>(*instanceBuffer).indexCount);
}

TEST_F(GPUDrivenValidationFixture,
       TierTwoSealRejectsStaleCandidateBytesAfterMapOrCommitFailure)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;

    GPUCulling culling;
    culling.Initialize(&device, config);
    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    const GPUSceneResidentGraphLease lease =
        MakeGPUSceneLease(device, 47u, capacities);
    ASSERT_TRUE(lease.IsValid());

    GPUSceneCullingCandidate candidate{};
    candidate.primitiveSlot = 1u;
    candidate.primitiveGeneration = 7u;
    candidate.drawSlot = 3u;
    candidate.drawGeneration = 7u;
    candidate.objectIdLow = 42u;
    candidate.requiredPassMask = 2u;
    candidate.drawGroupIndex = 0u;
    candidate.drawGroupVisibleOffset = 0u;
    candidate.rasterInstanceIndex = 0u;
    candidate.materialParameterSlot = 3u;

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 47u));
    culling.EndFrame();
    const GPUCullingRecordingIdentity firstIdentity{
        22u, 1u, 1u, 0u, 1u};
    ASSERT_NE(nullptr, culling.SealForGPUSceneGraph(firstIdentity, lease));
    EXPECT_EQ(0U, culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(sizeof(GPUSceneCullingCandidate),
              culling.GetLastGPUSceneCandidateUploadBytes());
    FakeBuffer* candidateBuffer = static_cast<FakeBuffer*>(
        culling.GetGPUSceneCandidateBuffer());
    ASSERT_NE(nullptr, candidateBuffer);
    EXPECT_EQ(MakeCanonicalGPUSceneCandidate(candidate, 0u),
              ReadBufferValue<GPUSceneCullingCandidate>(*candidateBuffer));

    GPUSceneCullingCandidate replacement = candidate;
    replacement.drawSlot = 4u;
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 12)));
    ASSERT_TRUE(culling.AddGPUSceneCandidate(replacement, 47u));
    culling.EndFrame();
    candidateBuffer->SetMapSucceeds(false);
    const GPUCullingRecordingIdentity failedIdentity{
        22u, 2u, 2u, 0u, 2u};
    EXPECT_EQ(nullptr, culling.SealForGPUSceneGraph(failedIdentity, lease));
    EXPECT_EQ(RHIContentValidity::Invalid,
              culling.GetAccessSnapshots().gpuSceneCandidates
                  .uniformAccess.contentValidity);
    EXPECT_EQ(0U, culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(0U, culling.GetLastGPUSceneCandidateUploadBytes());
    EXPECT_EQ(MakeCanonicalGPUSceneCandidate(candidate, 0u),
              ReadBufferValue<GPUSceneCullingCandidate>(*candidateBuffer));

    candidateBuffer->SetMapSucceeds(true);
    const std::shared_ptr<GPUCullingRecordedState> retried =
        culling.SealForGPUSceneGraph(failedIdentity, lease);
    ASSERT_NE(nullptr, retried);
    EXPECT_EQ(nullptr, retried->GetCulling().GetInstanceBuffer());
    EXPECT_EQ(RHIContentValidity::Valid,
              culling.GetAccessSnapshots().gpuSceneCandidates
                  .uniformAccess.contentValidity);
    EXPECT_EQ(0U, culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(sizeof(GPUSceneCullingCandidate),
              culling.GetLastGPUSceneCandidateUploadBytes());
    EXPECT_EQ(MakeCanonicalGPUSceneCandidate(replacement, 0u),
              ReadBufferValue<GPUSceneCullingCandidate>(*candidateBuffer));

    GPUSceneCullingCandidate commitReplacement = replacement;
    commitReplacement.drawSlot = 5u;
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 6)));
    ASSERT_TRUE(culling.AddGPUSceneCandidate(commitReplacement, 47u));
    culling.EndFrame();
    candidateBuffer->SetCommitSucceeds(false);
    const GPUCullingRecordingIdentity commitFailedIdentity{
        22u, 3u, 3u, 0u, 3u};
    EXPECT_EQ(nullptr,
              culling.SealForGPUSceneGraph(commitFailedIdentity, lease));
    EXPECT_EQ(RHIContentValidity::Invalid,
              culling.GetAccessSnapshots().gpuSceneCandidates
                  .uniformAccess.contentValidity);
    EXPECT_EQ(0u, culling.GetLastGPUSceneCandidateUploadBytes());
    EXPECT_EQ(MakeCanonicalGPUSceneCandidate(replacement, 0u),
              ReadBufferValue<GPUSceneCullingCandidate>(*candidateBuffer));

    candidateBuffer->SetCommitSucceeds(true);
    const std::shared_ptr<GPUCullingRecordedState> commitRetried =
        culling.SealForGPUSceneGraph(commitFailedIdentity, lease);
    ASSERT_NE(nullptr, commitRetried);
    EXPECT_EQ(RHIContentValidity::Valid,
              culling.GetAccessSnapshots().gpuSceneCandidates
                  .uniformAccess.contentValidity);
    EXPECT_EQ(sizeof(GPUSceneCullingCandidate),
              culling.GetLastGPUSceneCandidateUploadBytes());
    EXPECT_EQ(MakeCanonicalGPUSceneCandidate(commitReplacement, 0u),
              ReadBufferValue<GPUSceneCullingCandidate>(*candidateBuffer));
}

TEST_F(GPUDrivenValidationFixture,
       ExactStaticContentReusesIndependentResidencyAcrossFrameSlots)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config, 2);

    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    const GPUSceneResidentGraphLease lease =
        MakeGPUSceneLease(device, 53u, capacities);
    ASSERT_TRUE(lease.IsValid());

    GPUSceneCullingCandidate candidate{};
    candidate.primitiveSlot = 1u;
    candidate.primitiveGeneration = 7u;
    candidate.drawSlot = 3u;
    candidate.drawGeneration = 7u;
    candidate.objectIdLow = 42u;
    candidate.requiredPassMask = 2u;
    candidate.drawGroupIndex = 0u;
    candidate.drawGroupVisibleOffset = 0u;
    candidate.rasterInstanceIndex = 0u;
    candidate.materialParameterSlot = 3u;

    const auto collectStaticFrame = [&culling, &candidate]()
    {
        culling.BeginFrame();
        EXPECT_EQ(0u, culling.AddInstance(
            MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
        EXPECT_TRUE(culling.AddGPUSceneCandidate(candidate, 53u));
        culling.EndFrame();
    };

    ASSERT_TRUE(culling.SetFrameSlot(0));
    collectStaticFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{31u, 1u, 1u, 0u, 1u}));
    EXPECT_EQ(sizeof(GPUInstanceData), culling.GetLastInstanceUploadBytes());
    ASSERT_NE(nullptr, culling.SealForGPUSceneGraph(
        GPUCullingRecordingIdentity{31u, 2u, 1u, 0u, 2u}, lease));
    EXPECT_EQ(sizeof(GPUSceneCullingCandidate),
              culling.GetLastGPUSceneCandidateUploadBytes());

    ASSERT_TRUE(culling.SetFrameSlot(1));
    collectStaticFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{32u, 1u, 2u, 0u, 1u}));
    EXPECT_EQ(sizeof(GPUInstanceData), culling.GetLastInstanceUploadBytes());
    ASSERT_NE(nullptr, culling.SealForGPUSceneGraph(
        GPUCullingRecordingIdentity{32u, 2u, 2u, 0u, 2u}, lease));
    EXPECT_EQ(sizeof(GPUSceneCullingCandidate),
              culling.GetLastGPUSceneCandidateUploadBytes());

    // Both physical slots now contain the canonical versions. Rebuilding the
    // same vectors must publish no new CPU copies on the second rotation.
    ASSERT_TRUE(culling.SetFrameSlot(0));
    collectStaticFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{33u, 1u, 3u, 0u, 1u}));
    EXPECT_EQ(0U, culling.GetLastInstanceUploadBytes());
    ASSERT_NE(nullptr, culling.SealForGPUSceneGraph(
        GPUCullingRecordingIdentity{33u, 2u, 3u, 0u, 2u}, lease));
    EXPECT_EQ(0U, culling.GetLastGPUSceneCandidateUploadBytes());

    ASSERT_TRUE(culling.SetFrameSlot(1));
    collectStaticFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{34u, 1u, 4u, 0u, 1u}));
    EXPECT_EQ(0U, culling.GetLastInstanceUploadBytes());
    ASSERT_NE(nullptr, culling.SealForGPUSceneGraph(
        GPUCullingRecordingIdentity{34u, 2u, 4u, 0u, 2u}, lease));
    EXPECT_EQ(0U, culling.GetLastGPUSceneCandidateUploadBytes());
}

TEST_F(GPUDrivenValidationFixture,
       IndependentTierContentChangesUploadOnlyTheirOwnStream)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    const GPUSceneResidentGraphLease lease =
        MakeGPUSceneLease(device, 59u, capacities);
    ASSERT_TRUE(lease.IsValid());

    GPUSceneCullingCandidate candidate{};
    candidate.primitiveSlot = 1u;
    candidate.primitiveGeneration = 7u;
    candidate.drawSlot = 3u;
    candidate.drawGeneration = 7u;
    candidate.objectIdLow = 42u;
    candidate.requiredPassMask = 2u;
    candidate.drawGroupIndex = 0u;
    candidate.drawGroupVisibleOffset = 0u;
    candidate.rasterInstanceIndex = 0u;
    candidate.materialParameterSlot = 3u;

    GPUInstanceData initial =
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36);
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(initial));
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 59u));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{35u, 1u, 1u, 0u, 1u}));
    ASSERT_NE(nullptr, culling.SealForGPUSceneGraph(
        GPUCullingRecordingIdentity{35u, 2u, 1u, 0u, 2u}, lease));

    GPUInstanceData transformed = initial;
    transformed.worldMatrix[3][0] = 4.0f;
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(transformed));
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 59u));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{36u, 1u, 2u, 0u, 1u}));
    EXPECT_EQ(sizeof(GPUInstanceData), culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(0U, culling.GetLastGPUSceneCandidateUploadBytes());
    ASSERT_NE(nullptr, culling.SealForGPUSceneGraph(
        GPUCullingRecordingIdentity{36u, 2u, 2u, 0u, 2u}, lease));
    EXPECT_EQ(sizeof(GPUInstanceData), culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(0U, culling.GetLastGPUSceneCandidateUploadBytes());

    GPUSceneCullingCandidate replacement = candidate;
    replacement.drawSlot = 4u;
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(transformed));
    ASSERT_TRUE(culling.AddGPUSceneCandidate(replacement, 59u));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{37u, 1u, 3u, 0u, 1u}));
    EXPECT_EQ(0U, culling.GetLastInstanceUploadBytes());
    ASSERT_NE(nullptr, culling.SealForGPUSceneGraph(
        GPUCullingRecordingIdentity{37u, 2u, 3u, 0u, 2u}, lease));
    EXPECT_EQ(0U, culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(sizeof(GPUSceneCullingCandidate),
              culling.GetLastGPUSceneCandidateUploadBytes());

    // Changing only the external resident-scene lease does not alter either
    // CPU vector, so neither Tier 1 nor Tier 2 needs another upload.
    const GPUSceneResidentGraphLease nextLease =
        MakeGPUSceneLease(device, 60u, capacities);
    ASSERT_TRUE(nextLease.IsValid());
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(transformed));
    ASSERT_TRUE(culling.AddGPUSceneCandidate(replacement, 60u));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{38u, 1u, 4u, 0u, 1u}));
    EXPECT_EQ(0U, culling.GetLastInstanceUploadBytes());
    ASSERT_NE(nullptr, culling.SealForGPUSceneGraph(
        GPUCullingRecordingIdentity{38u, 2u, 4u, 0u, 2u}, nextLease));
    EXPECT_EQ(0U, culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(0U, culling.GetLastGPUSceneCandidateUploadBytes());
}

TEST_F(GPUDrivenValidationFixture,
       ShrinkingBelowAFinalizedSnapshotRejectsDeferredUploadsUntilTheNextLegalFrame)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    GPUSceneCullingCandidate candidate{};
    candidate.primitiveSlot = 1u;
    candidate.primitiveGeneration = 7u;
    candidate.drawSlot = 3u;
    candidate.drawGeneration = 7u;
    candidate.objectIdLow = 42u;
    candidate.requiredPassMask = 2u;
    candidate.drawGroupIndex = 0u;
    candidate.drawGroupVisibleOffset = 0u;
    candidate.rasterInstanceIndex = 0u;
    candidate.materialParameterSlot = 3u;

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 71u));
    ASSERT_EQ(1u, culling.AddInstance(
        MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 36)));
    GPUSceneCullingCandidate secondCandidate = candidate;
    secondCandidate.drawSlot = 4u;
    secondCandidate.rasterInstanceIndex = 1u;
    ASSERT_TRUE(culling.AddGPUSceneCandidate(secondCandidate, 71u));
    culling.EndFrame();

    GPUCullingConfig shrunkenConfig = config;
    shrunkenConfig.maxInstances = 1;
    culling.SetConfig(shrunkenConfig);

    FakeBuffer* const resizedInstanceBuffer = static_cast<FakeBuffer*>(
        culling.GetInstanceBuffer());
    ASSERT_NE(nullptr, resizedInstanceBuffer);
    EXPECT_EQ(sizeof(GPUInstanceData), resizedInstanceBuffer->GetSize());
    const uint32 instanceMapsBeforeRejectedUse =
        resizedInstanceBuffer->GetMapCallCount();

    EXPECT_EQ(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{61u, 1u, 1u, 0u, 1u}));
    EXPECT_EQ(instanceMapsBeforeRejectedUse,
              resizedInstanceBuffer->GetMapCallCount());

    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    const GPUSceneResidentGraphLease lease =
        MakeGPUSceneLease(device, 71u, capacities);
    ASSERT_TRUE(lease.IsValid());
    EXPECT_EQ(nullptr, culling.SealForGPUSceneGraph(
        GPUCullingRecordingIdentity{61u, 2u, 1u, 0u, 2u}, lease));
    FakeBuffer* const resizedCandidateBuffer = static_cast<FakeBuffer*>(
        culling.GetGPUSceneCandidateBuffer());
    ASSERT_NE(nullptr, resizedCandidateBuffer);
    EXPECT_EQ(sizeof(GPUSceneCullingCandidate), resizedCandidateBuffer->GetSize());
    EXPECT_EQ(0u, resizedCandidateBuffer->GetMapCallCount());

    FakeCommandContext rejectedContext;
    culling.Cull(rejectedContext, TestView(), TestProjection());
    EXPECT_TRUE(rejectedContext.dispatches.empty());
    EXPECT_EQ(instanceMapsBeforeRejectedUse,
              resizedInstanceBuffer->GetMapCallCount());
    EXPECT_FALSE(culling.WasGpuExecutionUsedLastCull());

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 24)));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{61u, 1u, 2u, 0u, 3u}));
    EXPECT_EQ(sizeof(GPUInstanceData), culling.GetLastInstanceUploadBytes());

    FakeCommandContext legalContext;
    culling.Cull(legalContext, TestView(), TestProjection());
    EXPECT_EQ(3u, legalContext.dispatches.size());
    EXPECT_TRUE(culling.WasGpuExecutionUsedLastCull());
}

TEST_F(GPUDrivenValidationFixture,
       DirectCullMaterializesDeferredTierOneResidencyBeforeComputeDispatch)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();
    EXPECT_EQ(0u, culling.GetLastInstanceUploadBytes());

    FakeBuffer* const instanceBuffer = static_cast<FakeBuffer*>(
        culling.GetInstanceBuffer());
    ASSERT_NE(nullptr, instanceBuffer);
    EXPECT_EQ(0u, instanceBuffer->GetMapCallCount());

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());

    EXPECT_EQ(sizeof(GPUInstanceData), culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(1u, instanceBuffer->GetMapCallCount());
    EXPECT_EQ(3u, context.dispatches.size());
    EXPECT_TRUE(culling.WasGpuExecutionUsedLastCull());
}

TEST_F(GPUDrivenValidationFixture,
       NormalCullRejectsConstantsMapFailureBeforeComputeDispatch)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    FakeBuffer* const constantsBuffer = static_cast<FakeBuffer*>(
        culling.GetCullingConstantsBuffer());
    ASSERT_NE(nullptr, constantsBuffer);
    constantsBuffer->SetMapSucceeds(false);

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());

    EXPECT_TRUE(context.dispatches.empty());
    EXPECT_FALSE(culling.WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(culling.WasCpuFallbackUsedLastCull());
    EXPECT_EQ(RHIContentValidity::Invalid,
              culling.GetAccessSnapshots().constants
                  .uniformAccess.contentValidity);
}

TEST_F(GPUDrivenValidationFixture,
       NormalCullRejectsConstantsCommitFailureBeforeComputeDispatch)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    FakeBuffer* const constantsBuffer = static_cast<FakeBuffer*>(
        culling.GetCullingConstantsBuffer());
    ASSERT_NE(nullptr, constantsBuffer);
    constantsBuffer->SetCommitSucceeds(false);

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());

    EXPECT_TRUE(context.dispatches.empty());
    EXPECT_FALSE(culling.WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(culling.WasCpuFallbackUsedLastCull());
    EXPECT_EQ(RHIContentValidity::Invalid,
              culling.GetAccessSnapshots().constants
                  .uniformAccess.contentValidity);
    EXPECT_EQ(1u, constantsBuffer->GetCommitMappedWriteCallCount());
}

TEST_F(GPUDrivenValidationFixture,
       CpuFallbackRejectsDirectOutputMapFailureWithoutPublishingSubmission)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    FakeBuffer* const drawCountBuffer = static_cast<FakeBuffer*>(
        culling.GetDrawCountBuffer());
    ASSERT_NE(nullptr, drawCountBuffer);
    EXPECT_EQ(RHIMemoryType::Upload, drawCountBuffer->GetMemoryType());
    drawCountBuffer->SetMapSucceeds(false);

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());

    EXPECT_TRUE(context.dispatches.empty());
    EXPECT_FALSE(culling.WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(culling.WasCpuFallbackUsedLastCull());
    EXPECT_EQ(1u, culling.GetDrawCount());
    ASSERT_EQ(1u, culling.GetIndirectCommands().size());
    EXPECT_EQ(GPUCullingFallbackReason::PipelineResourcesUnavailable,
              culling.GetLastFallbackReason());
    EXPECT_EQ(nullptr,
              culling.BuildIndexedIndirectSubmission().execution.argumentBuffer);
}

TEST_F(GPUDrivenValidationFixture,
       CpuFallbackRejectsDirectOutputCommitFailureWithoutPublishingSubmission)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    FakeBuffer* const drawCountBuffer = static_cast<FakeBuffer*>(
        culling.GetDrawCountBuffer());
    ASSERT_NE(nullptr, drawCountBuffer);
    EXPECT_EQ(RHIMemoryType::Upload, drawCountBuffer->GetMemoryType());
    drawCountBuffer->SetCommitSucceeds(false);

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());

    EXPECT_TRUE(context.dispatches.empty());
    EXPECT_EQ(0u, context.copyBufferCalls);
    EXPECT_FALSE(culling.WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(culling.WasCpuFallbackUsedLastCull());
    EXPECT_EQ(1u, culling.GetDrawCount());
    ASSERT_EQ(1u, culling.GetIndirectCommands().size());
    EXPECT_EQ(GPUCullingFallbackReason::PipelineResourcesUnavailable,
              culling.GetLastFallbackReason());
    ASSERT_EQ(1U, drawCountBuffer->GetMappedRanges().size());
    EXPECT_EQ(0U, drawCountBuffer->GetMappedRanges()[0].first);
    EXPECT_EQ(2U * sizeof(uint32), drawCountBuffer->GetMappedRanges()[0].second);
    EXPECT_EQ(1u, drawCountBuffer->GetCommitMappedWriteCallCount());
    EXPECT_EQ(nullptr,
              culling.BuildIndexedIndirectSubmission().execution.argumentBuffer);
}

TEST_F(GPUDrivenValidationFixture,
       CpuFallbackRejectsStagingOutputMapFailureWithoutPublishingSubmission)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    FakeBuffer* const drawCountBuffer = static_cast<FakeBuffer*>(
        culling.GetDrawCountBuffer());
    ASSERT_NE(nullptr, drawCountBuffer);
    EXPECT_EQ(RHIMemoryType::Default, drawCountBuffer->GetMemoryType());
    device.failTransientUploadMap = true;

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());

    EXPECT_EQ(1u, device.transientUploadMapFailureCount);
    ASSERT_FALSE(device.createdBuffers.empty());
    const FakeBuffer* const stagingBuffer = device.createdBuffers.back();
    ASSERT_EQ(1U, stagingBuffer->GetMappedRanges().size());
    EXPECT_EQ(0U, stagingBuffer->GetMappedRanges()[0].first);
    EXPECT_EQ(2U * sizeof(uint32), stagingBuffer->GetMappedRanges()[0].second);
    EXPECT_TRUE(context.dispatches.empty());
    EXPECT_FALSE(culling.WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(culling.WasCpuFallbackUsedLastCull());
    EXPECT_EQ(1u, culling.GetDrawCount());
    ASSERT_EQ(1u, culling.GetIndirectCommands().size());
    EXPECT_EQ(nullptr,
              culling.BuildIndexedIndirectSubmission().execution.argumentBuffer);
}

TEST_F(GPUDrivenValidationFixture,
       CpuFallbackRejectsStagingOutputCommitFailureBeforeCopyOrPublication)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    FakeBuffer* const drawCountBuffer = static_cast<FakeBuffer*>(
        culling.GetDrawCountBuffer());
    ASSERT_NE(nullptr, drawCountBuffer);
    EXPECT_EQ(RHIMemoryType::Default, drawCountBuffer->GetMemoryType());
    device.failTransientUploadCommit = true;

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());

    EXPECT_EQ(1u, device.transientUploadCommitFailureCount);
    ASSERT_FALSE(device.createdBuffers.empty());
    const FakeBuffer* const stagingBuffer = device.createdBuffers.back();
    ASSERT_EQ(1U, stagingBuffer->GetMappedRanges().size());
    EXPECT_EQ(0U, stagingBuffer->GetMappedRanges()[0].first);
    EXPECT_EQ(2U * sizeof(uint32), stagingBuffer->GetMappedRanges()[0].second);
    EXPECT_EQ(0u, context.copyBufferCalls);
    EXPECT_TRUE(context.dispatches.empty());
    EXPECT_FALSE(culling.WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(culling.WasCpuFallbackUsedLastCull());
    EXPECT_EQ(1u, culling.GetDrawCount());
    ASSERT_EQ(1u, culling.GetIndirectCommands().size());
    EXPECT_EQ(nullptr,
              culling.BuildIndexedIndirectSubmission().execution.argumentBuffer);
}

TEST_F(GPUDrivenValidationFixture,
       NoContextCpuFallbackKeepsCpuReferencesWithoutPublishingGpuSubmission)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    culling.CullCpuFallback(TestView(), TestProjection());

    EXPECT_FALSE(culling.WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(culling.WasCpuFallbackUsedLastCull());
    EXPECT_EQ(1u, culling.GetDrawCount());
    ASSERT_EQ(1u, culling.GetIndirectCommands().size());
    EXPECT_EQ(nullptr,
              culling.BuildIndexedIndirectSubmission().execution.argumentBuffer);
}

TEST_F(GPUDrivenValidationFixture,
       TierTwoOnlySealsCannotDispatchNormalCullOrRetainTierOneInstanceBindings)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    GPUSceneCullingCandidate candidate{};
    candidate.primitiveSlot = 1u;
    candidate.primitiveGeneration = 7u;
    candidate.drawSlot = 3u;
    candidate.drawGeneration = 7u;
    candidate.objectIdLow = 42u;
    candidate.requiredPassMask = 2u;
    candidate.drawGroupIndex = 0u;
    candidate.drawGroupVisibleOffset = 0u;
    candidate.rasterInstanceIndex = 0u;
    candidate.materialParameterSlot = 3u;

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 73u));
    culling.EndFrame();

    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    const std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGPUSceneGraph(
            GPUCullingRecordingIdentity{62u, 2u, 1u, 0u, 1u},
            MakeGPUSceneLease(device, 73u, capacities));
    ASSERT_NE(nullptr, recorded);
    EXPECT_EQ(nullptr, recorded->GetCulling().GetInstanceBuffer());
    EXPECT_NE(RHIContentValidity::Valid,
              recorded->GetAccessSnapshots().instances
                  .uniformAccess.contentValidity);

    FakeCommandContext context;
    recorded->Cull(context, TestView(), TestProjection());
    EXPECT_TRUE(context.dispatches.empty());
    EXPECT_FALSE(recorded->GetCulling().WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(recorded->GetCulling().WasCpuFallbackUsedLastCull());
}

TEST_F(GPUDrivenValidationFixture,
       SealedRecordingStateOwnsSlotInputsAcrossSourceMutation)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 4;

    GPUCulling culling;
    culling.Initialize(&device, config, 2);
    ASSERT_TRUE(culling.SetFrameSlot(1));
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    const GPUCullingRecordingIdentity identity{
        101u, 7u, 88u, 1u, 5u};
    const RHIBuffer* const slot1InstanceBuffer = culling.GetInstanceBuffer();
    const RHIBuffer* const slot1IndirectBuffer = culling.GetIndirectBuffer();
    const size_t createdBufferCountBeforeSeal = device.createdBuffers.size();
    const uint32 descriptorSetCountBeforeSeal =
        device.createDescriptorSetCalls;
    const std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGraph(identity);
    ASSERT_NE(nullptr, recorded);
    ASSERT_TRUE(recorded->IsValid());
    EXPECT_TRUE(recorded->Matches(identity));
    EXPECT_EQ(1u, recorded->GetSourceFrameSlot());
    EXPECT_EQ(0u, recorded->GetCopiedCpuPayloadBytes());
    ASSERT_NE(nullptr, recorded->GetCulling().GetInstanceBuffer());
    EXPECT_EQ(slot1InstanceBuffer,
              recorded->GetCulling().GetInstanceBuffer());
    EXPECT_EQ(slot1IndirectBuffer,
              recorded->GetCulling().GetIndirectBuffer());
    EXPECT_EQ(createdBufferCountBeforeSeal, device.createdBuffers.size());
    EXPECT_EQ(descriptorSetCountBeforeSeal,
              device.createDescriptorSetCalls);

    // RenderContext may prepare another waited slot while this graph remains
    // recorded, but it must not recycle source slot 1 before its completion.
    ASSERT_TRUE(culling.SetFrameSlot(0));
    EXPECT_NE(slot1InstanceBuffer, culling.GetInstanceBuffer());
    EXPECT_NE(slot1IndirectBuffer, culling.GetIndirectBuffer());
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 12)));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{102u, 7u, 89u, 0u, 5u}));

    const GPUInstanceData sealedInstance = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(recorded->GetCulling().GetInstanceBuffer()));
    const GPUInstanceData mutatedSourceInstance = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(culling.GetInstanceBuffer()));
    EXPECT_EQ(36u, sealedInstance.indexCount);
    EXPECT_EQ(12u, mutatedSourceInstance.indexCount);

    FakeCommandContext context;
    recorded->Cull(context, TestView(), TestProjection());
    const GPUCulling& sealedCulling = recorded->GetCulling();
    EXPECT_TRUE(context.dispatches.empty());
    EXPECT_FALSE(sealedCulling.WasCpuFallbackUsedLastCull());
    EXPECT_TRUE(sealedCulling.GetIndirectCommands().empty());
}

TEST_F(GPUDrivenValidationFixture,
       SealedRecordingSubmissionRetainsGpuObjectsUntilCompletion)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.bufferLifetimeState = std::make_shared<BufferLifetimeState>();

    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));

    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    const GPUCullingRecordingIdentity identity{
        201u, 11u, 99u, 0u, 6u};
    std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGraph(identity);
    ASSERT_NE(nullptr, recorded);

    RenderSubmissionResourceBatch batch;
    ASSERT_TRUE(recorded->RetainSubmissionResources(batch));

    // This CPU-fallback fixture has exactly the active instance/constants
    // pair plus the five shared culling/indirect buffers. A future omission is
    // therefore observable as a smaller retained set.
    constexpr uint32 expectedSealedPrimaryObjectCount = 7;
    EXPECT_EQ(expectedSealedPrimaryObjectCount,
              batch.GetRetainedObjectCount());

    const uint32 destroyedBeforeStateRelease =
        device.bufferLifetimeState->destroyedCount;
    recorded.reset();
    EXPECT_EQ(destroyedBeforeStateRelease,
              device.bufferLifetimeState->destroyedCount);

    FakeCommandContext context;
    GPUCompletionToken completion;
    const GPUCompletionPoint submittedPoint = tracker.Submit(&context);
    ASSERT_TRUE(InsertGPUCompletionPoint(completion, submittedPoint));
    batch.SealAndTransfer(completion, retirement);
    EXPECT_EQ(retirement.GetDiagnostics().entryCount,
              expectedSealedPrimaryObjectCount);
    EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Pending);
    EXPECT_EQ(destroyedBeforeStateRelease,
              device.bufferLifetimeState->destroyedCount);

    FakeFence* fence = device.GetFence(0);
    ASSERT_NE(nullptr, fence);
    fence->Complete(submittedPoint.value);
    EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Completed);
    EXPECT_EQ(destroyedBeforeStateRelease,
              device.bufferLifetimeState->destroyedCount);

    // Completion releases submission ownership, while the frame slot remains
    // persistent until its owning culler is shut down or safely resized.
    culling.Shutdown();
    EXPECT_EQ(destroyedBeforeStateRelease + expectedSealedPrimaryObjectCount,
              device.bufferLifetimeState->destroyedCount);
}

TEST_F(GPUDrivenValidationFixture, CpuFallbackBuffersAvoidDx11InvalidGpuOnlyFlags)
{
    FakeDevice device;

    GPUCullingConfig config;
    config.maxInstances = 8;

    GPUCulling culling;
    culling.Initialize(&device, config);

    const FakeBuffer* visibilityBuffer = device.FindBuffer("GPUCulling.VisibilityBuffer");
    const FakeBuffer* visibleInstanceBuffer = device.FindBuffer("GPUCulling.VisibleInstanceBuffer");
    const FakeBuffer* indirectBuffer = device.FindBuffer("GPUCulling.IndirectDrawBuffer");
    const FakeBuffer* drawCountBuffer = device.FindBuffer("GPUCulling.DrawCountBuffer");
    ASSERT_NE(nullptr, visibilityBuffer);
    ASSERT_NE(nullptr, visibleInstanceBuffer);
    ASSERT_NE(nullptr, indirectBuffer);
    ASSERT_NE(nullptr, drawCountBuffer);

    const auto expectNoGpuOnlyFlags = [](const FakeBuffer& buffer) {
        EXPECT_FALSE(HasFlag(buffer.GetUsage(), RHIBufferUsage::Structured));
        EXPECT_FALSE(HasFlag(buffer.GetUsage(), RHIBufferUsage::ShaderResource));
        EXPECT_FALSE(HasFlag(buffer.GetUsage(), RHIBufferUsage::UnorderedAccess));
    };

    EXPECT_EQ(RHIMemoryType::Upload, visibilityBuffer->GetMemoryType());
    expectNoGpuOnlyFlags(*visibilityBuffer);

    EXPECT_EQ(RHIMemoryType::Upload, visibleInstanceBuffer->GetMemoryType());
    expectNoGpuOnlyFlags(*visibleInstanceBuffer);

    EXPECT_EQ(RHIMemoryType::Upload, drawCountBuffer->GetMemoryType());
    expectNoGpuOnlyFlags(*drawCountBuffer);
    EXPECT_FALSE(HasFlag(drawCountBuffer->GetUsage(), RHIBufferUsage::IndirectArgs));

    EXPECT_EQ(RHIMemoryType::Default, indirectBuffer->GetMemoryType());
    EXPECT_TRUE(HasFlag(indirectBuffer->GetUsage(), RHIBufferUsage::IndirectArgs));
    EXPECT_TRUE(HasFlag(indirectBuffer->GetUsage(), RHIBufferUsage::CopyDst));
    expectNoGpuOnlyFlags(*indirectBuffer);
}

TEST_F(GPUDrivenValidationFixture, GpuExecutionBuffersKeepStructuredUavFlags)
{
    FakeDevice device;
    device.capabilities.supportsComputePipeline = true;
    device.capabilities.supportsDescriptorSets = true;
    device.capabilities.indexedIndirectExecution.supportsCountBuffer = true;

    GPUCullingConfig config;
    config.maxInstances = 8;

    GPUCulling culling;
    culling.Initialize(&device, config);

    const FakeBuffer* visibilityBuffer = device.FindBuffer("GPUCulling.VisibilityBuffer");
    const FakeBuffer* indirectBuffer = device.FindBuffer("GPUCulling.IndirectDrawBuffer");
    const FakeBuffer* drawCountBuffer = device.FindBuffer("GPUCulling.DrawCountBuffer");
    ASSERT_NE(nullptr, visibilityBuffer);
    ASSERT_NE(nullptr, indirectBuffer);
    ASSERT_NE(nullptr, drawCountBuffer);

    const auto expectGpuWritableStructuredBuffer = [](const FakeBuffer& buffer) {
        EXPECT_EQ(RHIMemoryType::Default, buffer.GetMemoryType());
        EXPECT_TRUE(HasFlag(buffer.GetUsage(), RHIBufferUsage::Structured));
        EXPECT_TRUE(HasFlag(buffer.GetUsage(), RHIBufferUsage::ShaderResource));
        EXPECT_TRUE(HasFlag(buffer.GetUsage(), RHIBufferUsage::UnorderedAccess));
        EXPECT_TRUE(HasFlag(buffer.GetUsage(), RHIBufferUsage::CopyDst));
    };

    expectGpuWritableStructuredBuffer(*visibilityBuffer);
    expectGpuWritableStructuredBuffer(*indirectBuffer);
    expectGpuWritableStructuredBuffer(*drawCountBuffer);
    EXPECT_TRUE(HasFlag(indirectBuffer->GetUsage(), RHIBufferUsage::IndirectArgs));
    EXPECT_TRUE(HasFlag(drawCountBuffer->GetUsage(), RHIBufferUsage::IndirectArgs));
}

TEST_F(GPUDrivenValidationFixture, GpuExecutionDecisionReportsCapabilityAndPipelineFallbacks)
{
    {
        FakeDevice device;
        device.capabilities.supportsDescriptorSets = true;
        device.capabilities.indexedIndirectExecution.supportsCountBuffer = true;

        GPUCullingConfig config;
        config.maxInstances = 8;

        GPUCulling culling;
        culling.Initialize(&device, config);

        GPUCullingExecutionDecision decision = culling.GetExecutionDecision();
        EXPECT_EQ(GPUCullingExecutionMode::CpuFallback, decision.mode);
        EXPECT_FALSE(decision.gpuCapable);
        EXPECT_FALSE(decision.pipelineReady);
        EXPECT_EQ(GPUCullingFallbackReason::ComputePipelineUnsupported, decision.fallbackReason);
    }

    {
        FakeDevice device;
        device.capabilities.supportsComputePipeline = true;
        device.capabilities.supportsDescriptorSets = true;
        device.capabilities.indexedIndirectExecution.supportsCountBuffer = true;

        GPUCullingConfig config;
        config.maxInstances = 8;

        GPUCulling culling;
        culling.Initialize(&device, config);

        GPUCullingExecutionDecision decision = culling.GetExecutionDecision();
        EXPECT_EQ(GPUCullingExecutionMode::CpuFallback, decision.mode);
        EXPECT_TRUE(decision.gpuCapable);
        EXPECT_FALSE(decision.pipelineReady);
        EXPECT_EQ(GPUCullingFallbackReason::DescriptorSetLayoutCreationFailed, decision.fallbackReason);

        culling.BeginFrame();
        EXPECT_EQ(0u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
        culling.EndFrame();

        FakeCommandContext ctx;
        culling.Cull(ctx, TestView(), TestProjection());

        EXPECT_TRUE(culling.WasCpuFallbackUsedLastCull());
        EXPECT_FALSE(culling.WasGpuExecutionUsedLastCull());
        EXPECT_EQ(GPUCullingFallbackReason::DescriptorSetLayoutCreationFailed,
                  culling.GetLastFallbackReason());
    }
}

TEST_F(GPUDrivenValidationFixture,
       GpuCullingFallbackReasonPreservesExistingDiagnosticValues)
{
    EXPECT_EQ(4u, static_cast<uint32>(
        GPUCullingFallbackReason::IndirectDrawCountUnsupported));
    EXPECT_EQ(12u, static_cast<uint32>(
        GPUCullingFallbackReason::PipelineResourcesUnavailable));
    EXPECT_EQ(13u, static_cast<uint32>(
        GPUCullingFallbackReason::IndirectDrawFirstInstanceUnsupported));
}

TEST_F(GPUDrivenValidationFixture,
       GpuExecutionRejectsMissingFirstInstanceBeforePipelineCreationOrRecording)
{
    FakeDevice device;
    device.capabilities.supportsComputePipeline = true;
    device.capabilities.supportsDescriptorSets = true;
    device.capabilities.indexedIndirectExecution.supportsCountBuffer = true;
    device.capabilities.indexedIndirectExecution.supportsFirstInstance = false;

    GPUCulling culling;
    culling.Initialize(&device, GPUCullingConfig{});

    const GPUCullingExecutionDecision decision = culling.GetExecutionDecision();
    EXPECT_EQ(GPUCullingExecutionMode::CpuFallback, decision.mode);
    EXPECT_FALSE(decision.gpuCapable);
    EXPECT_FALSE(decision.pipelineReady);
    EXPECT_EQ(GPUCullingFallbackReason::IndirectDrawFirstInstanceUnsupported,
              decision.fallbackReason);
    EXPECT_EQ(0u, device.createDescriptorSetLayoutCalls);
    EXPECT_EQ(0u, device.createComputePipelineCalls);

    culling.BeginFrame();
    EXPECT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());
    EXPECT_TRUE(culling.WasCpuFallbackUsedLastCull());
    EXPECT_FALSE(culling.WasGpuExecutionUsedLastCull());
    EXPECT_EQ(GPUCullingFallbackReason::IndirectDrawFirstInstanceUnsupported,
              culling.GetLastFallbackReason());
    EXPECT_TRUE(context.dispatches.empty());
}

TEST_F(GPUDrivenValidationFixture,
       IndexedIndirectCountSubmissionRejectsUnsupportedCapabilityWithoutRecording)
{
    FakeDevice device;
    device.capabilities.indexedIndirectExecution.supportsCountBuffer = false;

    RHIBufferDesc argumentDesc;
    argumentDesc.size = sizeof(IndirectDrawIndexedCommand);
    argumentDesc.usage = RHIBufferUsage::IndirectArgs;
    argumentDesc.memoryType = RHIMemoryType::Default;
    argumentDesc.stride = sizeof(IndirectDrawIndexedCommand);
    RHIBufferRef argumentBuffer = device.CreateBuffer(argumentDesc);
    ASSERT_NE(nullptr, argumentBuffer.Get());

    RHIBufferDesc countDesc;
    countDesc.size = sizeof(uint32);
    countDesc.usage = RHIBufferUsage::IndirectArgs;
    countDesc.memoryType = RHIMemoryType::Default;
    countDesc.stride = sizeof(uint32);
    RHIBufferRef countBuffer = device.CreateBuffer(countDesc);
    ASSERT_NE(nullptr, countBuffer.Get());

    GPUCullingIndexedIndirectSubmission cullingSubmission;
    cullingSubmission.capabilities = &device.capabilities;
    cullingSubmission.execution.mode = RHIIndirectExecutionMode::CountBuffer;
    cullingSubmission.execution.argumentBuffer = argumentBuffer.Get();
    cullingSubmission.execution.commandStride = sizeof(IndirectDrawIndexedCommand);
    cullingSubmission.execution.maxDrawCount = 1;
    cullingSubmission.execution.requiresFirstInstance = true;
    cullingSubmission.execution.countBuffer = countBuffer.Get();

    FakeCommandContext context;
    const RenderSubmissionResult result =
        RecordIndexedIndirectSubmission(context, cullingSubmission);
    EXPECT_FALSE(result.recorded);
    EXPECT_EQ(RenderSubmissionValidationCode::IndexedIndirectValidationFailed,
              result.validationCode);
    EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::CapabilityUnsupported,
              result.indexedIndirectValidationCode);
    EXPECT_EQ(0u, context.drawIndexedIndirectCalls);
    EXPECT_EQ(0u, context.drawIndexedIndirectCountCalls);
}

TEST_F(GPUDrivenValidationFixture, DrawIndexedIndirectHonorsMaxDrawCount)
{
    FakeDevice device;
    GPUCulling culling;
    culling.Initialize(&device, GPUCullingConfig{});

    culling.BeginFrame();
    EXPECT_EQ(0u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 12)));
    EXPECT_EQ(1u, culling.AddInstance(MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 12)));
    culling.EndFrame();

    FakeCommandContext ctx;
    culling.Cull(ctx, TestView(), TestProjection());
    ASSERT_EQ(1u, culling.GetDrawCount());

    const GPUCullingIndexedIndirectSubmission cullingSubmission =
        culling.BuildIndexedIndirectSubmission(1);
    EXPECT_EQ(RHIIndirectExecutionMode::FixedCount,
              cullingSubmission.execution.mode);
    EXPECT_EQ(1u, cullingSubmission.execution.maxDrawCount);
    const RenderSubmissionResult submission =
        RecordIndexedIndirectSubmission(ctx, cullingSubmission);
    EXPECT_TRUE(submission.recorded);
    EXPECT_EQ(1u, submission.submittedDrawUpperBound);
    EXPECT_TRUE(submission.executedDrawCountAvailable);
    EXPECT_EQ(1u, submission.executedDrawCount);
    EXPECT_EQ(1u, ctx.drawIndexedIndirectCalls);
    EXPECT_EQ(1u, ctx.lastIndirectDrawCount);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand), ctx.lastIndirectStride);
}

TEST_F(GPUDrivenValidationFixture, CpuFallbackBuildsMeshGroupedIndirectRanges)
{
    FakeDevice device;

    GPUCullingConfig config;
    config.maxInstances = 8;
    config.enableDistanceCulling = false;

    GPUCulling culling;
    culling.Initialize(&device, config);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.BeginDrawGroup(7001u));
    EXPECT_EQ(0u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    EXPECT_EQ(1u, culling.AddInstance(MakeInstance(Vec3(100.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndDrawGroup();
    ASSERT_EQ(1u, culling.BeginDrawGroup(7002u));
    EXPECT_EQ(2u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -6.0f), 1.0f, 24)));
    culling.EndDrawGroup();
    culling.EndFrame();

    FakeCommandContext ctx;
    culling.Cull(ctx, TestView(), TestProjection());

    ASSERT_EQ(2u, culling.GetDrawCount());
    ASSERT_EQ(2u, culling.GetDrawGroups().size());
    EXPECT_EQ(7001u, culling.GetDrawGroups()[0].meshId);
    EXPECT_EQ(0u, culling.GetDrawGroups()[0].commandOffset);
    EXPECT_EQ(sizeof(uint32), culling.GetDrawGroups()[0].countBufferOffset);
    EXPECT_EQ(2u, culling.GetDrawGroups()[0].maxDrawCount);
    EXPECT_EQ(1u, culling.GetDrawGroups()[0].visibleDrawCount);
    EXPECT_EQ(7002u, culling.GetDrawGroups()[1].meshId);
    EXPECT_EQ(1u, culling.GetDrawGroups()[1].commandOffset);
    EXPECT_EQ(2u, culling.GetDrawGroups()[1].visibleInstanceOffset);
    EXPECT_EQ(sizeof(uint32) * 2u, culling.GetDrawGroups()[1].countBufferOffset);
    EXPECT_EQ(1u, culling.GetDrawGroups()[1].maxDrawCount);
    EXPECT_EQ(1u, culling.GetDrawGroups()[1].visibleDrawCount);

    const FakeBuffer* drawCountBuffer = device.FindBuffer("GPUCulling.DrawCountBuffer");
    ASSERT_NE(nullptr, drawCountBuffer);
    EXPECT_EQ(2u, ReadBufferValue<uint32>(*drawCountBuffer, 0));
    EXPECT_EQ(1u, ReadBufferValue<uint32>(*drawCountBuffer, 1));
    EXPECT_EQ(1u, ReadBufferValue<uint32>(*drawCountBuffer, 2));

    const FakeBuffer* indirectBuffer = device.FindBuffer("GPUCulling.IndirectDrawBuffer");
    ASSERT_NE(nullptr, indirectBuffer);
    const IndirectDrawIndexedCommand firstGroupCommand =
        ReadBufferValue<IndirectDrawIndexedCommand>(*indirectBuffer, 0);
    const IndirectDrawIndexedCommand secondGroupCommand =
        ReadBufferValue<IndirectDrawIndexedCommand>(*indirectBuffer, 1);
    EXPECT_EQ(36u, firstGroupCommand.indexCount);
    EXPECT_EQ(0u, firstGroupCommand.firstInstance);
    EXPECT_EQ(24u, secondGroupCommand.indexCount);
    EXPECT_EQ(2u, secondGroupCommand.firstInstance);

    const FakeBuffer* visibleBuffer =
        device.FindBuffer("GPUCulling.VisibleInstanceBuffer");
    ASSERT_NE(nullptr, visibleBuffer);
    EXPECT_EQ(0u, ReadBufferValue<uint32>(*visibleBuffer, 0));
    EXPECT_EQ(RVX_INVALID_INDEX, ReadBufferValue<uint32>(*visibleBuffer, 1));
    EXPECT_EQ(2u, ReadBufferValue<uint32>(*visibleBuffer, 2));

    const GPUCullingIndexedIndirectSubmission wholeSubmission =
        culling.BuildIndexedIndirectSubmission();
    EXPECT_EQ(nullptr, wholeSubmission.execution.argumentBuffer);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCalls);

    const GPUCullingIndexedIndirectSubmission firstCullingSubmission =
        culling.BuildIndexedIndirectGroupSubmission(0);
    EXPECT_EQ(RHIIndirectExecutionMode::FixedCount,
              firstCullingSubmission.execution.mode);
    EXPECT_TRUE(firstCullingSubmission.execution.requiresFirstInstance);
    EXPECT_EQ(0u, firstCullingSubmission.execution.argumentOffset);
    const RenderSubmissionResult firstSubmission =
        RecordIndexedIndirectSubmission(ctx, firstCullingSubmission);
    EXPECT_TRUE(firstSubmission.recorded);
    EXPECT_EQ(1u, firstSubmission.submittedDrawUpperBound);
    EXPECT_TRUE(firstSubmission.executedDrawCountAvailable);
    EXPECT_EQ(1u, firstSubmission.executedDrawCount);
    EXPECT_EQ(0u, ctx.lastIndirectOffset);
    EXPECT_EQ(1u, ctx.lastIndirectDrawCount);
    const GPUCullingIndexedIndirectSubmission secondCullingSubmission =
        culling.BuildIndexedIndirectGroupSubmission(1);
    EXPECT_TRUE(secondCullingSubmission.execution.requiresFirstInstance);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand),
              secondCullingSubmission.execution.argumentOffset);
    const RenderSubmissionResult secondSubmission =
        RecordIndexedIndirectSubmission(ctx, secondCullingSubmission);
    EXPECT_TRUE(secondSubmission.recorded);
    EXPECT_EQ(1u, secondSubmission.submittedDrawUpperBound);
    EXPECT_TRUE(secondSubmission.executedDrawCountAvailable);
    EXPECT_EQ(1u, secondSubmission.executedDrawCount);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand), ctx.lastIndirectOffset);
    EXPECT_EQ(1u, ctx.lastIndirectDrawCount);
}

TEST(RenderSubmissionStrategyValidation, DirectStrategyRecordsExactResultSemantics)
{
    FakeCommandContext context;
    RenderSubmissionRequest request;
    request.kind = RenderSubmissionKind::DirectIndexed;
    request.directIndexed = {36u, 2u, 7u, -3, 4u};

    const DirectRenderSubmissionStrategy strategy;
    const RenderSubmissionResult result = strategy.Submit(context, request);

    EXPECT_EQ(RenderSubmissionValidationCode::Success, result.validationCode);
    EXPECT_TRUE(result.recorded);
    EXPECT_EQ(1u, result.submittedDrawUpperBound);
    EXPECT_TRUE(result.executedDrawCountAvailable);
    EXPECT_EQ(1u, result.executedDrawCount);
    EXPECT_EQ(1u, context.drawIndexedCalls);
    EXPECT_EQ(36u, context.lastDirectIndexCount);
    EXPECT_EQ(2u, context.lastDirectInstanceCount);
    EXPECT_EQ(7u, context.lastDirectFirstIndex);
    EXPECT_EQ(-3, context.lastDirectVertexOffset);
    EXPECT_EQ(4u, context.lastDirectFirstInstance);
}

TEST(RenderSubmissionStrategyValidation, IndexedStrategyPreservesFixedAndCountSemantics)
{
    FakeDevice device;
    RHIBufferDesc argumentDesc;
    argumentDesc.size = sizeof(IndirectDrawIndexedCommand) * 2u;
    argumentDesc.usage = RHIBufferUsage::IndirectArgs;
    FakeBuffer argumentBuffer(argumentDesc);

    RHIBufferDesc countDesc;
    countDesc.size = sizeof(uint32);
    countDesc.usage = RHIBufferUsage::IndirectArgs;
    FakeBuffer countBuffer(countDesc);

    FakeCommandContext context;
    RenderSubmissionRequest fixedRequest;
    fixedRequest.kind = RenderSubmissionKind::IndexedIndirect;
    fixedRequest.capabilities = &device.capabilities;
    fixedRequest.indexedIndirect.argumentBuffer = &argumentBuffer;
    fixedRequest.indexedIndirect.commandStride = sizeof(IndirectDrawIndexedCommand);
    fixedRequest.indexedIndirect.maxDrawCount = 2;

    const IndexedIndirectRenderSubmissionStrategy strategy;
    const RenderSubmissionResult fixedResult = strategy.Submit(context, fixedRequest);
    EXPECT_EQ(RenderSubmissionValidationCode::Success, fixedResult.validationCode);
    EXPECT_TRUE(fixedResult.recorded);
    EXPECT_EQ(2u, fixedResult.submittedDrawUpperBound);
    EXPECT_TRUE(fixedResult.executedDrawCountAvailable);
    EXPECT_EQ(2u, fixedResult.executedDrawCount);
    EXPECT_EQ(1u, context.drawIndexedIndirectCalls);
    EXPECT_EQ(0u, context.drawIndexedIndirectCountCalls);

    RenderSubmissionRequest countRequest = fixedRequest;
    countRequest.indexedIndirect.mode = RHIIndirectExecutionMode::CountBuffer;
    countRequest.indexedIndirect.countBuffer = &countBuffer;
    const RenderSubmissionResult countResult = strategy.Submit(context, countRequest);
    EXPECT_EQ(RenderSubmissionValidationCode::Success, countResult.validationCode);
    EXPECT_TRUE(countResult.recorded);
    EXPECT_EQ(2u, countResult.submittedDrawUpperBound);
    EXPECT_FALSE(countResult.executedDrawCountAvailable);
    EXPECT_EQ(0u, countResult.executedDrawCount);
    EXPECT_EQ(1u, context.drawIndexedIndirectCalls);
    EXPECT_EQ(1u, context.drawIndexedIndirectCountCalls);
}

TEST(RenderSubmissionStrategyValidation, IndexedStrategyZeroAndInvalidRequestsNeverRecord)
{
    FakeDevice device;
    FakeCommandContext context;
    const IndexedIndirectRenderSubmissionStrategy strategy;

    RenderSubmissionRequest zeroRequest;
    zeroRequest.kind = RenderSubmissionKind::IndexedIndirect;
    zeroRequest.capabilities = &device.capabilities;
    zeroRequest.indexedIndirect.maxDrawCount = 0;
    const RenderSubmissionResult zeroResult = strategy.Submit(context, zeroRequest);
    EXPECT_EQ(RenderSubmissionValidationCode::Success, zeroResult.validationCode);
    EXPECT_FALSE(zeroResult.recorded);
    EXPECT_TRUE(zeroResult.executedDrawCountAvailable);
    EXPECT_EQ(0u, zeroResult.executedDrawCount);

    RenderSubmissionRequest invalidRequest = zeroRequest;
    invalidRequest.indexedIndirect.maxDrawCount = 1;
    const RenderSubmissionResult invalidResult = strategy.Submit(context, invalidRequest);
    EXPECT_EQ(RenderSubmissionValidationCode::IndexedIndirectValidationFailed,
              invalidResult.validationCode);
    EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::MissingArgumentBuffer,
              invalidResult.indexedIndirectValidationCode);
    EXPECT_FALSE(invalidResult.recorded);
    EXPECT_EQ(0u, context.drawIndexedIndirectCalls);
    EXPECT_EQ(0u, context.drawIndexedIndirectCountCalls);
}

TEST(RenderSubmissionStrategyValidation, EncodedExtensionUsesFrozenTypedRequestBoundary)
{
    FakeCommandContext context;
    FakeEncodedCommandBuffer encodedCommandBuffer(RHIBackendType::Metal);
    RenderSubmissionRequest request;
    request.kind = RenderSubmissionKind::EncodedCommandBuffer;
    request.encodedCommandBuffer.commandBuffer = &encodedCommandBuffer;

    const DirectRenderSubmissionStrategy directStrategy;
    EXPECT_EQ(RenderSubmissionValidationCode::UnsupportedSubmissionKind,
              directStrategy.Submit(context, request).validationCode);
    const IndexedIndirectRenderSubmissionStrategy indexedStrategy;
    EXPECT_EQ(RenderSubmissionValidationCode::UnsupportedSubmissionKind,
              indexedStrategy.Submit(context, request).validationCode);

    const FakeEncodedSubmissionStrategy encodedStrategy;
    const RenderSubmissionResult result = encodedStrategy.Submit(context, request);
    EXPECT_EQ(RenderSubmissionValidationCode::Success, result.validationCode);
    EXPECT_TRUE(result.recorded);
    EXPECT_EQ(1u, result.submittedDrawUpperBound);
}

TEST_F(GPUDrivenValidationFixture, DrawItemsMapBackThroughVisibleSourceIndices)
{
    FakeDevice device;

    GPUCullingConfig config;
    config.maxInstances = 8;
    config.enableDistanceCulling = true;
    config.maxDrawDistance = 30.0f;

    GPUCulling culling;
    culling.Initialize(&device, config);

    RenderScene scene;
    RenderObject nearObject =
        MakeRenderObject(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 7001u);
    nearObject.entityId = 1u;
    RenderObject sideObject =
        MakeRenderObject(Vec3(75.0f, 0.0f, -5.0f), 1.0f, 7002u);
    sideObject.entityId = 2u;
    RenderObject farObject =
        MakeRenderObject(Vec3(0.0f, 0.0f, -100.0f), 1.0f, 7003u);
    farObject.entityId = 3u;
    scene.AddObject(nearObject);
    scene.AddObject(sideObject);
    scene.AddObject(farObject);

    const std::vector<RenderDrawItem> drawItems = {
        MakeDrawItem(0, 7001u, 9001u),
        MakeDrawItem(1, 7002u, 9002u),
        MakeDrawItem(2, 7003u, 9003u),
    };

    culling.BeginFrame();
    EXPECT_EQ(0u, culling.AddDrawItemInstance(scene, drawItems[0], GPUIndexedDrawDesc{48u, 4u, -3}, 0u));
    EXPECT_EQ(1u, culling.AddDrawItemInstance(scene, drawItems[1], GPUIndexedDrawDesc{12u, 9u, 2}, 1u));
    EXPECT_EQ(2u, culling.AddDrawItemInstance(scene, drawItems[2], GPUIndexedDrawDesc{24u, 0u, 0}, 2u));
    culling.EndFrame();

    FakeCommandContext ctx;
    culling.Cull(ctx, TestView(), TestProjection());

    ASSERT_EQ(1u, culling.GetVisibleSourceIndices().size());
    EXPECT_EQ(0u, culling.GetVisibleSourceIndices()[0]);

    ASSERT_EQ(3u, culling.GetIndirectCommands().size());
    const IndirectDrawIndexedCommand& command = culling.GetIndirectCommands()[0];
    EXPECT_EQ(48u, command.indexCount);
    EXPECT_EQ(4u, command.firstIndex);
    EXPECT_EQ(-3, command.vertexOffset);
    EXPECT_EQ(0u, command.firstInstance);

    const GPUCulling::Statistics stats = culling.GetStatistics();
    EXPECT_EQ(3u, stats.totalInstances);
    EXPECT_EQ(1u, stats.visibleInstances);
    EXPECT_EQ(1u, stats.frustumCulled);
    EXPECT_EQ(1u, stats.distanceCulled);
}

TEST_F(GPUDrivenValidationFixture, SceneRendererWiresMeshDrawPacketsBeforeTypedPassRecording)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::string header =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Renderer" / "SceneRenderer.h");
    const std::string source =
        ReadTextFile(root / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
    const std::string depthHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Passes" / "DepthPrepass.h");
    const std::string depthSource =
        ReadTextFile(root / "Render" / "Private" / "Passes" / "DepthPrepass.cpp");
    const std::string subsystemSource =
        ReadTextFile(root / "Render" / "Private" / "RenderSubsystem.cpp");
    const std::string renderCMake =
        ReadTextFile(root / "Render" / "CMakeLists.txt");
    const std::string skyboxHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Passes" / "SkyboxPass.h");
    const std::string skyboxSource =
        ReadTextFile(root / "Render" / "Private" / "Passes" / "SkyboxPass.cpp");
    const std::string transparentHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Passes" / "TransparentPass.h");
    const std::string transparentSource =
        ReadTextFile(root / "Render" / "Private" / "Passes" / "TransparentPass.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(depthHeader.empty());
    ASSERT_FALSE(depthSource.empty());
    ASSERT_FALSE(subsystemSource.empty());
    ASSERT_FALSE(renderCMake.empty());
    ASSERT_FALSE(skyboxHeader.empty());
    ASSERT_FALSE(skyboxSource.empty());
    ASSERT_FALSE(transparentHeader.empty());
    ASSERT_FALSE(transparentSource.empty());

    EXPECT_NE(header.find("SceneGPUDrivenCullingStats"), std::string::npos);
    EXPECT_NE(header.find("graphPassAdded"), std::string::npos);
    EXPECT_NE(header.find("graphPassRecorded"), std::string::npos);
    EXPECT_NE(header.find("gpuExecutionRecorded"), std::string::npos);
    EXPECT_NE(header.find("graphInputDrawItemCount"), std::string::npos);
    EXPECT_NE(header.find("opaqueIndirectRequested"), std::string::npos);
    EXPECT_NE(header.find("opaqueGpuDrivenIndirectDrawCount"), std::string::npos);
    EXPECT_NE(header.find("opaqueGpuDrivenIndirectSubmittedDrawUpperBound"),
              std::string::npos);
    EXPECT_NE(header.find("opaqueGpuDrivenExecutedDrawCountAvailable"),
              std::string::npos);
    EXPECT_NE(header.find("SetGPUDrivenCullingMode"), std::string::npos);
    EXPECT_NE(header.find("const SceneGPUDrivenCullingStats& GetGPUDrivenCullingStats() const"),
              std::string::npos);
    EXPECT_NE(header.find("void AddGPUDrivenCullingPass("), std::string::npos);
    EXPECT_NE(header.find("void PrepareGPUDrivenGraphCullInputs()"), std::string::npos);
    EXPECT_NE(header.find("void BuildGPUDrivenVisibilityInputs()"), std::string::npos);
    EXPECT_NE(header.find("void PrepareMeshPassPackets()"), std::string::npos);
    EXPECT_NE(header.find("void CompileRenderFramePlan()"), std::string::npos);
    EXPECT_NE(header.find("RenderPolicyDiagnostics m_renderPolicyDiagnostics"),
              std::string::npos);
    EXPECT_NE(header.find("const SceneMeshPassPreparation& GetMeshPassPreparation() const"),
              std::string::npos);

    const size_t buildDrawLists = source.find("void SceneRenderer::BuildMaterialDrawLists()");
    ASSERT_NE(buildDrawLists, std::string::npos);
    const size_t prepareMeshDefinition =
        source.find("void SceneRenderer::PrepareMeshPassPackets()", buildDrawLists);
    ASSERT_NE(prepareMeshDefinition, std::string::npos);
    const std::string buildMaterialSegment =
        source.substr(buildDrawLists, prepareMeshDefinition - buildDrawLists);
    const size_t prepareMeshPasses = buildMaterialSegment.find("PrepareMeshPassPackets();");
    ASSERT_NE(prepareMeshPasses, std::string::npos);
    EXPECT_LT(prepareMeshPasses, prepareMeshDefinition - buildDrawLists);
    EXPECT_EQ(buildMaterialSegment.find("BuildGPUDrivenVisibilityInputs();"), std::string::npos);
    EXPECT_EQ(buildMaterialSegment.find("m_objectVelocityPass->SetRenderScene"), std::string::npos);
    EXPECT_EQ(buildMaterialSegment.find("m_objectVelocityPass->SetRenderTargets"), std::string::npos);
    EXPECT_EQ(buildMaterialSegment.find("m_objectVelocityPass->SetDrawItems"), std::string::npos);

    EXPECT_FALSE(std::filesystem::exists(
        root / "Render" / "Private" / "Renderer" / "RenderFrameResourceBinder.h"));
    EXPECT_FALSE(std::filesystem::exists(
        root / "Render" / "Private" / "Renderer" / "RenderFrameResourceBinder.cpp"));
    EXPECT_EQ(header.find("UpdatePassResources"), std::string::npos);
    EXPECT_EQ(header.find("ExecutePasses"), std::string::npos);
    EXPECT_EQ(source.find("RenderFrameResourceBinder"), std::string::npos);
    EXPECT_EQ(source.find("UpdatePassResources"), std::string::npos);
    EXPECT_EQ(source.find("ExecutePasses"), std::string::npos);
    EXPECT_EQ(renderCMake.find("RenderFrameResourceBinder"), std::string::npos);
    EXPECT_EQ(source.find("m_skyboxPass->SetCubemap"), std::string::npos);
    EXPECT_EQ(source.find("m_skyboxPass->SetProceduralSkyParams"), std::string::npos);
    EXPECT_EQ(source.find("m_skyboxPass->SetSolidColor"), std::string::npos);
    EXPECT_EQ(source.find("m_skyboxPass->ClearSkybox"), std::string::npos);
    EXPECT_EQ(skyboxHeader.find("SetCubemap"), std::string::npos);
    EXPECT_EQ(skyboxHeader.find("SetProceduralSkyParams"), std::string::npos);
    EXPECT_EQ(skyboxHeader.find("SetSolidColor"), std::string::npos);
    EXPECT_EQ(skyboxHeader.find("ClearSkybox"), std::string::npos);
    EXPECT_EQ(skyboxHeader.find("SetRenderTargets"), std::string::npos);
    EXPECT_EQ(skyboxSource.find("SkyboxPass::SetRenderTargets"), std::string::npos);
    EXPECT_EQ(skyboxSource.find("SkyboxPass::SetCubemap"), std::string::npos);
    EXPECT_EQ(skyboxSource.find("SkyboxPass::SetProceduralSkyParams"), std::string::npos);
    EXPECT_EQ(skyboxSource.find("SkyboxPass::SetSolidColor"), std::string::npos);
    EXPECT_EQ(skyboxSource.find("SkyboxPass::ClearSkybox"), std::string::npos);
    EXPECT_EQ(transparentHeader.find("SetRenderScene"), std::string::npos);
    EXPECT_EQ(transparentHeader.find("SetRenderTargets"), std::string::npos);
    EXPECT_EQ(transparentSource.find("TransparentPass::SetRenderScene"), std::string::npos);
    EXPECT_EQ(transparentSource.find("TransparentPass::SetRenderTargets"), std::string::npos);

    const size_t renderDefinition = source.find("void SceneRenderer::Render()");
    const size_t renderGuardEnd = source.find(
        "PreparePassesForFrame();", renderDefinition);
    const size_t compileCall = source.find("CompileRenderFramePlan();", renderDefinition);
    const size_t cullingCall = source.find(
        "BuildGPUDrivenVisibilityInputs();", compileCall);
    const size_t createGraph = source.find(
        "m_renderGraph = std::make_unique<RenderGraph>();", cullingCall);
    const size_t buildGraphCall = source.find("BuildRenderGraph();", createGraph);
    ASSERT_NE(renderDefinition, std::string::npos);
    ASSERT_NE(renderGuardEnd, std::string::npos);
    EXPECT_EQ(source.substr(renderDefinition,
                            renderGuardEnd - renderDefinition)
                  .find("!m_renderGraph"),
              std::string::npos);
    ASSERT_NE(compileCall, std::string::npos);
    ASSERT_NE(cullingCall, std::string::npos);
    ASSERT_NE(createGraph, std::string::npos);
    ASSERT_NE(buildGraphCall, std::string::npos);
    EXPECT_LT(compileCall, cullingCall);
    EXPECT_LT(cullingCall, createGraph);
    EXPECT_LT(createGraph, buildGraphCall);

    const size_t setModeDefinition =
        source.find("void SceneRenderer::SetGPUDrivenCullingMode");
    const size_t setEnabledDefinition =
        source.find("void SceneRenderer::SetGPUDrivenCullingEnabled", setModeDefinition);
    ASSERT_NE(setModeDefinition, std::string::npos);
    ASSERT_NE(setEnabledDefinition, std::string::npos);
    const std::string setModeBody = source.substr(
        setModeDefinition, setEnabledDefinition - setModeDefinition);
    EXPECT_EQ(setModeBody.find("ResolveGPUDrivenPolicy"), std::string::npos);
    EXPECT_EQ(setModeBody.find("GetExecutionDecision"), std::string::npos);

    const size_t applyDefinition =
        source.find("void SceneRenderer::PrepareFrameApplyResources");
    const size_t invalidateCall =
        source.find("InvalidateRenderFramePlan();", applyDefinition);
    const size_t registryAssignment =
        source.find("m_renderResourceRegistry = &registry;", applyDefinition);
    ASSERT_NE(applyDefinition, std::string::npos);
    ASSERT_NE(invalidateCall, std::string::npos);
    ASSERT_NE(registryAssignment, std::string::npos);
    EXPECT_LT(invalidateCall, registryAssignment);
    EXPECT_NE(subsystemSource.find("features.policy = frame.policy;"),
              std::string::npos);
    EXPECT_EQ(depthSource.find("GetBackendType"), std::string::npos);
    EXPECT_EQ(depthSource.find("GetGPUDrivenBackendQualification"),
              std::string::npos);

    EXPECT_EQ(source.find("ApplyGPUDrivenCullingToDrawLists"), std::string::npos);
    EXPECT_EQ(source.find("ApplyGPUDrivenCullingToDrawList"), std::string::npos);
    EXPECT_EQ(source.find("m_gpuCulling->CullCpuFallback"), std::string::npos);
    EXPECT_EQ(source.find("getSourceDrawItem"), std::string::npos);
    EXPECT_NE(source.find("reference.sourcePacketIndex"), std::string::npos);
    EXPECT_NE(source.find("m_renderCandidates.Find(pass, sourcePacketIndex)"),
              std::string::npos);
    EXPECT_NE(header.find("m_depthGPUCulling"), std::string::npos);
    EXPECT_NE(header.find("m_opaqueGPUCulling"), std::string::npos);
    EXPECT_NE(source.find("gpuCullingFrameSlotCount"), std::string::npos);
    EXPECT_NE(source.find("frameSynchronizer->GetFrameCount()"),
              std::string::npos);
    const size_t depthSlotSelection = source.find(
        "m_depthGPUCulling->SetFrameSlot(frameSlot)");
    const size_t opaqueSlotSelection = source.find(
        "m_opaqueGPUCulling->SetFrameSlot(frameSlot)");
    ASSERT_NE(std::string::npos, depthSlotSelection);
    ASSERT_NE(std::string::npos, opaqueSlotSelection);
    const size_t firstGpuBeginFrame = source.find("owner->BeginFrame(");
    ASSERT_NE(std::string::npos, firstGpuBeginFrame);
    EXPECT_LT(depthSlotSelection, firstGpuBeginFrame);
    EXPECT_LT(opaqueSlotSelection, firstGpuBeginFrame);
    EXPECT_EQ(std::string::npos,
              source.find("owner->BeginFrame(", firstGpuBeginFrame + 1));
    const size_t planValidation = source.find(
        "ValidatePlannedGPUDrivenPacketRange(*framePlan");
    ASSERT_NE(std::string::npos, planValidation);
    EXPECT_LT(planValidation, firstGpuBeginFrame);
    EXPECT_NE(source.find("PrepareGPUDrivenGraphCullInputs();"), std::string::npos);
    EXPECT_NE(source.find("m_meshPassPreparation.depth"), std::string::npos);
    EXPECT_NE(source.find("m_meshPassPreparation.opaque"), std::string::npos);
    EXPECT_NE(source.find("for (const RenderDrawGroupRange& group : stream.groups)"),
              std::string::npos);
    EXPECT_EQ(source.find("std::find_if(drawGroups.begin(), drawGroups.end()"),
              std::string::npos);
    EXPECT_NE(source.find("owner->BeginDrawGroup("), std::string::npos);
    EXPECT_NE(source.find("key.geometry.mesh"), std::string::npos);
    EXPECT_NE(source.find("leaderPacket.materialKey.material"), std::string::npos);
    EXPECT_NE(source.find("key.pipeline.materialVariant"), std::string::npos);

    const std::string opaqueHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Passes" / "OpaquePass.h");
    const std::string opaqueSource =
        ReadTextFile(root / "Render" / "Private" / "Passes" / "OpaquePass.cpp");
    EXPECT_EQ(opaqueHeader.find("FindGPUDrivenGroupRepresentative"), std::string::npos);
    EXPECT_EQ(opaqueSource.find("FindGPUDrivenGroupRepresentative"), std::string::npos);
    EXPECT_EQ(opaqueSource.find("GetBackendType"), std::string::npos);
    EXPECT_EQ(opaqueSource.find("GetGPUDrivenBackendQualification"),
              std::string::npos);

    const size_t buildGraph = source.find("void SceneRenderer::BuildRenderGraph()");
    ASSERT_NE(buildGraph, std::string::npos);
    const size_t cullGraphCall = source.find(
        "AddGPUDrivenCullingPass(passRecordContext.identity);", buildGraph);
    const size_t passRegistryLoop = source.find("for (auto& pass : m_passRegistry->GetPasses())", buildGraph);
    ASSERT_NE(cullGraphCall, std::string::npos);
    ASSERT_NE(passRegistryLoop, std::string::npos);
    EXPECT_LT(cullGraphCall, passRegistryLoop);
    const size_t typedPassRecord = source.find(
        "pass->AddToGraph(*m_renderGraph, passRecordContext);", passRegistryLoop);
    ASSERT_NE(typedPassRecord, std::string::npos);
    EXPECT_EQ(source.find("pass->AddToGraph(*m_renderGraph, m_viewData)"),
              std::string::npos);

    EXPECT_NE(source.find("\"GPUDrivenDepthCull\""), std::string::npos);
    EXPECT_NE(source.find("\"GPUDrivenOpaqueCull\""), std::string::npos);
    EXPECT_NE(source.find("RenderGraphPassType::Compute"), std::string::npos);
    EXPECT_NE(source.find("MakeRHIAccessSnapshot(RHIResourceState::ConstantBuffer"), std::string::npos);
    EXPECT_NE(source.find("MakeRHIAccessSnapshot(RHIResourceState::ShaderResource"), std::string::npos);
    EXPECT_NE(source.find("RHIResourceState::UnorderedAccess,"), std::string::npos);
    EXPECT_NE(source.find("data.indirectDraws = builder.Write(data.indirectDraws, unorderedAccess)"),
              std::string::npos);
    EXPECT_EQ(source.find("m_depthPrepass->SetGPUDriven" "RenderGraphResources"),
              std::string::npos);
    EXPECT_EQ(source.find("m_opaquePass->SetGPUDriven" "RenderGraphResources"),
              std::string::npos);
    EXPECT_NE(source.find("RenderPassRecordContext passRecordContext"), std::string::npos);
    EXPECT_NE(source.find("passRecordContext.depthGPUDriven"), std::string::npos);
    EXPECT_NE(source.find("passRecordContext.opaqueGPUDriven"), std::string::npos);
    EXPECT_NE(source.find("SealForGraph"), std::string::npos);
    EXPECT_NE(source.find("recordedState->Cull"), std::string::npos);
    EXPECT_EQ(source.find("owner->Cull(ctx, m_viewData.viewMatrix, m_viewData.projectionMatrix)"),
              std::string::npos);
    EXPECT_EQ(source.find("owner->Cull(ctx, cullViewMatrix, cullProjectionMatrix)"),
              std::string::npos);
    EXPECT_NE(source.find("cullingStatsSink->gpuExecutionRecorded"), std::string::npos);
    EXPECT_NE(source.find("m_opaquePass->GetDrawStats()"), std::string::npos);
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.opaqueIndirectRequested"), std::string::npos);
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.opaqueGpuDrivenIndirectDrawCount"), std::string::npos);
    EXPECT_NE(source.find(
                  "opaqueGpuDrivenIndirectSubmittedDrawUpperBound"),
              std::string::npos);
    EXPECT_EQ(depthHeader.find("SetGPUDriven" "RenderGraphResources"),
              std::string::npos);
    EXPECT_NE(depthSource.find("builder.Read(m_gpuDrivenInstanceHandle, RHIShaderStage::Vertex)"),
              std::string::npos);
    EXPECT_NE(depthSource.find("builder.Read(m_gpuDrivenIndirectHandle, RHIResourceState::IndirectArgument)"),
              std::string::npos);
    EXPECT_NE(depthSource.find("builder.Read(m_gpuDrivenDrawCountHandle, RHIResourceState::IndirectArgument)"),
              std::string::npos);
}

TEST_F(GPUDrivenValidationFixture,
       SceneRendererResetsBothCullingUploadDiagnosticsBeforeLaneSelection)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());
    const std::string source = ReadTextFile(
        root / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
    ASSERT_FALSE(source.empty());

    const size_t buildInputs = source.find(
        "void SceneRenderer::BuildGPUDrivenVisibilityInputs()");
    const size_t prepareInputs = source.find(
        "void SceneRenderer::PrepareGPUDrivenGraphCullInputs()", buildInputs);
    ASSERT_NE(std::string::npos, buildInputs);
    ASSERT_NE(std::string::npos, prepareInputs);
    const std::string body = source.substr(buildInputs, prepareInputs - buildInputs);

    const size_t depthReset = body.find(
        "m_depthGPUCulling->ResetFrameUploadDiagnostics();");
    const size_t opaqueReset = body.find(
        "m_opaqueGPUCulling->ResetFrameUploadDiagnostics();");
    const size_t gpuPreparation = body.find(
        "if (m_gpuDrivenCullingEnabled &&");
    ASSERT_NE(std::string::npos, depthReset);
    ASSERT_NE(std::string::npos, opaqueReset);
    ASSERT_NE(std::string::npos, gpuPreparation);
    // This position covers GPU-to-Direct, dual-lane-to-single-lane, and
    // empty-lane transitions: none can reach a lane-specific early return
    // before both owners advertise current-attempt byte counts.
    EXPECT_LT(depthReset, gpuPreparation);
    EXPECT_LT(opaqueReset, gpuPreparation);
}

TEST_F(GPUDrivenValidationFixture, SceneRendererFrameDiagnosticsExposeGPUDrivenExecutionDecision)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::string header =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Renderer" / "SceneRenderer.h");
    const std::string source =
        ReadTextFile(root / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
    const std::string diagnosticsHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "RenderDiagnostics.h");
    const std::string subsystemSource =
        ReadTextFile(root / "Render" / "Private" / "RenderSubsystem.cpp");
    const std::string artifactSource =
        ReadTextFile(root / "Render" / "Private" / "Diagnostics" / "RenderToolArtifacts.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(diagnosticsHeader.empty());
    ASSERT_FALSE(subsystemSource.empty());
    ASSERT_FALSE(artifactSource.empty());

    EXPECT_NE(header.find("bool executionDecisionAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("GPUCullingExecutionDecision executionDecision;"), std::string::npos);
    EXPECT_NE(header.find("bool opaqueCullingReady = false;"), std::string::npos);
    EXPECT_NE(header.find("bool opaquePipelineReady = false;"), std::string::npos);
    EXPECT_NE(header.find("bool opaqueIndirectSubmitted = false;"), std::string::npos);
    EXPECT_NE(header.find("GPUDrivenDrawFallbackReason opaqueFallbackReason"),
              std::string::npos);
    EXPECT_NE(header.find("SceneGPUDrivenCullingStats gpuDrivenCullingStats;"), std::string::npos);
    EXPECT_NE(source.find("diagnostics.gpuDrivenCullingStats = m_gpuDrivenCullingStats;"),
              std::string::npos);
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.executionDecisionAvailable = true;"),
              std::string::npos);
    EXPECT_NE(source.find("diagnosticGPUCulling->GetExecutionDecision()"),
              std::string::npos);
    EXPECT_NE(source.find("const GPUCullingExecutionDecision depthGPUExecution"),
              std::string::npos);
    EXPECT_NE(source.find("const GPUCullingExecutionDecision opaqueGPUExecution"),
              std::string::npos);
    EXPECT_NE(source.find("m_meshPassPreparation.depth,\n                                  depthGPUExecution"),
              std::string::npos);
    EXPECT_NE(source.find("m_meshPassPreparation.opaque,\n                                  opaqueGPUExecution"),
              std::string::npos);
    EXPECT_EQ(source.find("primaryGPUCulling"), std::string::npos);
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.outputOpaqueDrawItemCount"),
              std::string::npos);
    EXPECT_NE(source.find("m_opaqueGPUCulling->GetStatistics()"),
              std::string::npos);
    for (const char* field : {"visibilityCandidateCount",
                              "cpuVisibleCandidateCount",
                              "passVisibilityCandidateCount",
                              "gpuPlannedVisibilityCandidateCount",
                              "invalidVisibilityBoundsCount",
                              "gpuDeferredVisibilityCandidateCount",
                              "gpuVisibilityReadbackPerformed",
                              "occlusionRequestedButUnavailable",
                              "gpuCullingGraphPassCount",
                              "activeRowUploadBytes",
                              "activeRowCount",
                              "activeRowHighWatermark",
                              "instancePatchedRowCount",
                              "gpuSceneCandidatePatchedRowCount",
                              "activeRowPatchedRowCount",
                              "instanceFullMaterializationCount",
                              "gpuSceneCandidateFullMaterializationCount",
                              "activeRowFullMaterializationCount",
                              "continuityFullMaterializationCount",
                              "capacityFullMaterializationCount",
                              "directOpaqueRasterTranscript"})
    {
        EXPECT_NE(header.find(field), std::string::npos);
        EXPECT_NE(diagnosticsHeader.find(field), std::string::npos);
        EXPECT_NE(subsystemSource.find(std::string("RVX_COPY_GPU_CULLING_FIELD(") + field + ")"),
                  std::string::npos);
    }
    EXPECT_NE(artifactSource.find("gpuPlannedCandidates="), std::string::npos);
    EXPECT_NE(artifactSource.find("gpuDeferredCandidates="), std::string::npos);
}

TEST_F(GPUDrivenValidationFixture,
       IndependentCullingOwnersKeepDepthAndOpaqueStreamsIsolated)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 8;
    config.enableDistanceCulling = false;

    GPUCulling depthOwner;
    GPUCulling opaqueOwner;
    depthOwner.Initialize(&device, config);
    opaqueOwner.Initialize(&device, config);

    depthOwner.BeginFrame();
    ASSERT_EQ(0u, depthOwner.BeginDrawGroup(1001u));
    EXPECT_EQ(0u, depthOwner.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -4.0f), 0.5f, 3)));
    depthOwner.EndDrawGroup();
    depthOwner.EndFrame();

    opaqueOwner.BeginFrame();
    ASSERT_EQ(0u, opaqueOwner.BeginDrawGroup(2001u));
    EXPECT_EQ(0u, opaqueOwner.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -6.0f), 0.5f, 6)));
    EXPECT_EQ(1u, opaqueOwner.AddInstance(
        MakeInstance(Vec3(100.0f, 0.0f, -6.0f), 0.5f, 6)));
    opaqueOwner.EndDrawGroup();
    opaqueOwner.EndFrame();

    // Supply a command context so the Default indirect buffers can receive
    // their staged copies; the test validates owner isolation, not the
    // no-context CPU-reference-only path.
    FakeCommandContext depthContext;
    FakeCommandContext opaqueContext;
    depthOwner.Cull(depthContext, TestView(), TestProjection());
    opaqueOwner.Cull(opaqueContext, TestView(), TestProjection());
    EXPECT_EQ(1u, depthOwner.GetInstanceCount());
    EXPECT_EQ(2u, opaqueOwner.GetInstanceCount());
    EXPECT_EQ(1u, depthOwner.GetDrawGroups().size());
    EXPECT_EQ(1u, opaqueOwner.GetDrawGroups().size());
    EXPECT_EQ(1u, depthOwner.GetDrawCount());
    EXPECT_EQ(1u, opaqueOwner.GetDrawCount());
    EXPECT_NE(depthOwner.GetInstanceBuffer(), opaqueOwner.GetInstanceBuffer());
    EXPECT_NE(depthOwner.GetIndirectBuffer(), opaqueOwner.GetIndirectBuffer());

    // A malformed/rejected next Depth frame resets only its own owner. Opaque
    // remains a valid independently prepared stream for the current frame.
    depthOwner.BeginFrame();
    EXPECT_EQ(0u, depthOwner.GetInstanceCount());
    EXPECT_EQ(2u, opaqueOwner.GetInstanceCount());
    EXPECT_EQ(1u, opaqueOwner.GetDrawGroups().size());
}

TEST_F(GPUDrivenValidationFixture,
       VisibilityCandidateInstanceRequiresStablePacketObjectIdentity)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 8;
    GPUCulling culling;
    culling.Initialize(&device, config);

    RenderObject object = MakeRenderObject(
        Vec3(0.0f, 0.0f, -5.0f), 1.0f, 7001u);
    object.entityId = 42u;
    RenderScene scene;
    scene.AddObject(object);

    RenderVisibilityCandidate candidate;
    candidate.candidateIndex = 5;
    candidate.sourcePacketIndex = 3;
    candidate.objectIndex = 0;
    candidate.pass = RenderPassKind::Depth;
    candidate.objectVisible = true;
    candidate.drawable = true;
    candidate.worldBounds = object.bounds;

    RenderDrawPacket packet;
    packet.objectId = object.entityId;
    packet.primitiveData = candidate.objectIndex;
    packet.pass = candidate.pass;
    packet.geometryKey.mesh = object.mesh;
    packet.arguments.indexCount = 36;
    packet.arguments.firstIndex = 4;
    packet.arguments.vertexOffset = -2;
    const GPUIndexedDrawDesc drawDesc{36, 4, -2};

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.BeginDrawGroup(7001u));
    EXPECT_EQ(0u, culling.AddVisibilityCandidateInstance(
                      scene, candidate, packet, drawDesc));

    RenderVisibilityCandidate invalidCandidate = candidate;
    invalidCandidate.candidateIndex = RVX_INVALID_INDEX;
    EXPECT_EQ(RVX_INVALID_INDEX,
              culling.AddVisibilityCandidateInstance(
                  scene, invalidCandidate, packet, drawDesc));

    RenderDrawPacket wrongPass = packet;
    wrongPass.pass = RenderPassKind::Opaque;
    EXPECT_EQ(RVX_INVALID_INDEX,
              culling.AddVisibilityCandidateInstance(
                  scene, candidate, wrongPass, drawDesc));

    RenderDrawPacket wrongObject = packet;
    wrongObject.objectId++;
    EXPECT_EQ(RVX_INVALID_INDEX,
              culling.AddVisibilityCandidateInstance(
                  scene, candidate, wrongObject, drawDesc));

    GPUIndexedDrawDesc wrongDraw = drawDesc;
    wrongDraw.firstIndex++;
    EXPECT_EQ(RVX_INVALID_INDEX,
              culling.AddVisibilityCandidateInstance(
                  scene, candidate, packet, wrongDraw));
    EXPECT_EQ(1u, culling.GetInstanceCount());
}

TEST_F(GPUDrivenValidationFixture, OcclusionRequestRemainsExplicitlyUnavailable)
{
    GPUCullingConfig defaults;
    EXPECT_FALSE(defaults.enableOcclusionCulling);
    EXPECT_FALSE(defaults.twoPhaseOcclusion);
    RenderGPUCullingSettings frameDefaults;
    EXPECT_FALSE(frameDefaults.enableOcclusionCulling);

    FakeDevice device;
    GPUCullingConfig config;
    config.enableOcclusionCulling = true;
    config.twoPhaseOcclusion = true;
    GPUCulling culling;
    culling.Initialize(&device, config);

    EXPECT_TRUE(culling.WasOcclusionRequested());
    EXPECT_FALSE(culling.IsOcclusionAvailable());
    EXPECT_FALSE(culling.GetConfig().enableOcclusionCulling);
    EXPECT_FALSE(culling.GetConfig().twoPhaseOcclusion);
}

TEST_F(GPUDrivenValidationFixture,
       HZBDiagnosticsRemainStructuredAndBackendNeutrallyUnavailable)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::string rendererHeader = ReadTextFile(
        root / "Render" / "Include" / "Render" / "Renderer" /
        "SceneRenderer.h");
    const std::string rendererSource = ReadTextFile(
        root / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
    const std::string diagnosticsHeader = ReadTextFile(
        root / "Render" / "Include" / "Render" / "RenderDiagnostics.h");
    const std::string subsystemSource = ReadTextFile(
        root / "Render" / "Private" / "RenderSubsystem.cpp");

    EXPECT_NE(rendererHeader.find("bool hzbRequested = false;"),
              std::string::npos);
    EXPECT_NE(rendererHeader.find("bool hzbSupported = false;"),
              std::string::npos);
    EXPECT_NE(rendererHeader.find("bool hzbEnabled = false;"),
              std::string::npos);
    EXPECT_NE(rendererSource.find("diagnostics.hzbSupported = false;"),
              std::string::npos);
    EXPECT_NE(rendererSource.find("occlusionRequestedButUnavailable"),
              std::string::npos);
    EXPECT_NE(diagnosticsHeader.find("RenderPassFeatureDiagnostics hzb{};"),
              std::string::npos);
    EXPECT_NE(subsystemSource.find("features.hzb.requested = frame.hzbRequested;"),
              std::string::npos);
    EXPECT_NE(subsystemSource.find("features.hzb.reason = frame.hzbReason;"),
              std::string::npos);
}

TEST_F(GPUDrivenValidationFixture, ActiveRowIndirectionUsesStableComputeRowsAndDrawCountsReserveNPlusOne)
{
    FakeDevice device;

    GPUCullingConfig config;
    config.maxInstances = 64;

    GPUCulling culling;
    culling.Initialize(&device, config);

    const FakeBuffer* instanceIndexBuffer =
        device.FindBuffer("GPUCulling.ActiveRowsBuffer");
    ASSERT_NE(nullptr, instanceIndexBuffer);
    EXPECT_EQ(sizeof(GPUCullingActiveRow) * 64u, instanceIndexBuffer->GetSize());
    EXPECT_EQ(sizeof(GPUCullingActiveRow), instanceIndexBuffer->GetStride());
    EXPECT_TRUE(HasFlag(instanceIndexBuffer->GetUsage(), RHIBufferUsage::Structured));
    EXPECT_TRUE(HasFlag(instanceIndexBuffer->GetUsage(), RHIBufferUsage::ShaderResource));
    EXPECT_EQ(RHIMemoryType::Upload, instanceIndexBuffer->GetMemoryType());
    EXPECT_EQ(instanceIndexBuffer, culling.GetInstanceIndexBuffer());

    const FakeBuffer* visibleInstanceBuffer =
        device.FindBuffer("GPUCulling.VisibleInstanceBuffer");
    ASSERT_NE(nullptr, visibleInstanceBuffer);
    EXPECT_TRUE(HasFlag(visibleInstanceBuffer->GetUsage(),
                        RHIBufferUsage::Vertex));

    const FakeBuffer* drawCountBuffer =
        device.FindBuffer("GPUCulling.DrawCountBuffer");
    ASSERT_NE(nullptr, drawCountBuffer);
    EXPECT_GE(drawCountBuffer->GetSize(), sizeof(uint32) * 65u);

    const GPUCullingAccessSnapshots& snapshots = culling.GetAccessSnapshots();
    EXPECT_EQ(RHIResourceState::ShaderResource,
              ProjectRHIResourceState(snapshots.instanceIndices.uniformAccess));
}

TEST_F(GPUDrivenValidationFixture,
       StableRowsPatchActiveRowRemovalAcrossFrameSlotsWithoutTopologyRecovery)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 8;
    GPUCulling culling;
    culling.Initialize(&device, config, 2);

    RenderScene scene;
    RenderObject objectA = MakeRenderObject(Vec3(-1.0f, 0.0f, -5.0f), 1.0f, 77u);
    objectA.entityId = 100u;
    objectA.objectRevision = 1u;
    RenderObject objectB = MakeRenderObject(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 77u);
    objectB.entityId = 200u;
    objectB.objectRevision = 1u;
    scene.AddObject(objectA);
    scene.AddObject(objectB);

    const std::array<RenderDrawItem, 2> items{
        MakeDrawItem(0u, 77u, 88u),
        MakeDrawItem(1u, 77u, 88u)};
    const auto collect = [&culling, &scene, &items](
                             std::initializer_list<uint32> itemIndices)
    {
        ASSERT_NE(RVX_INVALID_INDEX,
                  culling.BeginDrawGroup(77u, 88u));
        for (uint32 itemIndex : itemIndices)
        {
            ASSERT_LT(itemIndex, items.size());
            EXPECT_NE(RVX_INVALID_INDEX,
                      culling.AddDrawItemInstance(
                          scene, items[itemIndex],
                          GPUIndexedDrawDesc{36u, 0u, 0}, itemIndex));
        }
        culling.EndDrawGroup();
    };
    const auto seal = [&culling](uint64 frame)
    {
        EXPECT_NE(nullptr, culling.SealForGraph(
            GPUCullingRecordingIdentity{91u, 1u, frame, 0u, frame}));
    };

    ASSERT_TRUE(culling.SetFrameSlot(0u));
    const std::array<uint64, 2> initialChanged{100u, 200u};
    culling.BeginFrame(1u, initialChanged, {}, true);
    collect({0u, 1u});
    culling.EndFrame();
    seal(1u);
    EXPECT_EQ(2u * sizeof(GPUInstanceData),
              culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(2u * sizeof(GPUCullingActiveRow),
              culling.GetLastActiveRowUploadBytes());

    ASSERT_TRUE(culling.SetFrameSlot(1u));
    culling.BeginFrame(1u, {}, {}, false);
    collect({0u, 1u});
    culling.EndFrame();
    seal(2u);
    const GPUCullingIncrementalDiagnostics warmedDiagnostics =
        culling.GetIncrementalDiagnostics();

    // Removing B changes the single group's max draw count from two to one.
    // Dense active rows cover only the current dispatch range, so the removed
    // tail needs no patch: Counts.x prevents it from being consumed.
    ASSERT_TRUE(culling.SetFrameSlot(0u));
    const std::array<uint64, 1> removedB{200u};
    culling.BeginFrame(2u, {}, removedB, false);
    collect({0u});
    culling.EndFrame();
    seal(3u);
    EXPECT_EQ(0u, culling.GetLastActiveRowUploadBytes());
    EXPECT_EQ(0u, culling.GetIncrementalDiagnostics().activeRowPatchedRowCount);
    EXPECT_EQ(0u,
              culling.GetIncrementalDiagnostics()
                  .activeRowUploadWork.committedRangeCount);
    EXPECT_EQ(warmedDiagnostics.activeRowFullMaterializationCount,
              culling.GetIncrementalDiagnostics().activeRowFullMaterializationCount);
    EXPECT_EQ(warmedDiagnostics.continuityFullMaterializationCount,
              culling.GetIncrementalDiagnostics().continuityFullMaterializationCount);

    // The second warmed slot likewise remains valid without a sparse tombstone
    // upload because the active dispatch count has already shrunk.
    ASSERT_TRUE(culling.SetFrameSlot(1u));
    culling.BeginFrame(2u, {}, {}, false);
    collect({0u});
    culling.EndFrame();
    seal(4u);
    EXPECT_EQ(0u, culling.GetLastActiveRowUploadBytes());
    EXPECT_EQ(0u,
              culling.GetIncrementalDiagnostics()
                  .activeRowUploadWork.committedRangeCount);
    EXPECT_EQ(warmedDiagnostics.activeRowFullMaterializationCount,
              culling.GetIncrementalDiagnostics().activeRowFullMaterializationCount);
    EXPECT_EQ(warmedDiagnostics.continuityFullMaterializationCount,
              culling.GetIncrementalDiagnostics().continuityFullMaterializationCount);

    // Once both slots have applied the removal, unchanged collection stays
    // completely quiet.
    ASSERT_TRUE(culling.SetFrameSlot(0u));
    culling.BeginFrame(2u, {}, {}, false);
    collect({0u});
    culling.EndFrame();
    seal(5u);
    EXPECT_EQ(0u, culling.GetLastActiveRowUploadBytes());
    EXPECT_EQ(0u, culling.GetIncrementalDiagnostics().activeRowPatchedRowCount);
}

TEST_F(GPUDrivenValidationFixture,
       DenseActiveRowsFollowPacketGroupOrderWhileRetainingStableResidentRows)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 8u;
    GPUCulling culling;
    culling.Initialize(&device, config);

    RenderScene scene;
    RenderObject objectA =
        MakeRenderObject(Vec3(-1.0f, 0.0f, -5.0f), 1.0f, 77u);
    objectA.entityId = 100u;
    objectA.objectRevision = 1u;
    RenderObject objectB =
        MakeRenderObject(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 78u);
    objectB.entityId = 200u;
    objectB.objectRevision = 1u;
    scene.AddObject(objectA);
    scene.AddObject(objectB);

    const std::array<RenderDrawItem, 2> items{
        MakeDrawItem(0u, 77u, 88u),
        MakeDrawItem(1u, 78u, 89u)};
    const auto collectInGroupOrder =
        [&culling, &scene, &items](std::initializer_list<uint32> itemOrder)
    {
        for (uint32 itemIndex : itemOrder)
        {
            ASSERT_LT(itemIndex, items.size());
            ASSERT_NE(RVX_INVALID_INDEX,
                      culling.BeginDrawGroup(
                          700u + itemIndex, 800u + itemIndex));
            ASSERT_NE(RVX_INVALID_INDEX,
                      culling.AddDrawItemInstance(
                          scene,
                          items[itemIndex],
                          GPUIndexedDrawDesc{36u, 0u, 0},
                          itemIndex));
            culling.EndDrawGroup();
        }
    };
    const auto seal = [&culling](uint64 frame)
    {
        ASSERT_NE(nullptr, culling.SealForGraph(
            GPUCullingRecordingIdentity{115u, 1u, frame, 0u, frame}));
    };

    const std::array<uint64, 2> changed{100u, 200u};
    culling.BeginFrame(1u, changed, {}, true);
    collectInGroupOrder({0u, 1u});
    culling.EndFrame();
    seal(1u);

    const FakeBuffer* activeRows = static_cast<const FakeBuffer*>(
        culling.GetInstanceIndexBuffer());
    ASSERT_NE(nullptr, activeRows);
    EXPECT_EQ(0u, ReadBufferValue<GPUCullingActiveRow>(*activeRows, 0u).residentRow);
    EXPECT_EQ(1u, ReadBufferValue<GPUCullingActiveRow>(*activeRows, 1u).residentRow);

    // The stable resident tables retain A=0/B=1, but the next deterministic
    // packet groups B first. Its dense dispatch indices must therefore become
    // B,A without uploading either sparse canonical payload again.
    culling.BeginFrame(2u, {}, {}, false);
    collectInGroupOrder({1u, 0u});
    culling.EndFrame();
    seal(2u);

    EXPECT_EQ(0u, culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(2u * sizeof(GPUCullingActiveRow),
              culling.GetLastActiveRowUploadBytes());
    const GPUCullingActiveRow first =
        ReadBufferValue<GPUCullingActiveRow>(*activeRows, 0u);
    const GPUCullingActiveRow second =
        ReadBufferValue<GPUCullingActiveRow>(*activeRows, 1u);
    EXPECT_EQ(1u, first.residentRow);
    EXPECT_EQ(0u, first.drawGroupIndex);
    EXPECT_EQ(0u, first.drawGroupVisibleOffset);
    EXPECT_EQ(0u, second.residentRow);
    EXPECT_EQ(1u, second.drawGroupIndex);
    EXPECT_EQ(1u, second.drawGroupVisibleOffset);
}

TEST_F(GPUDrivenValidationFixture,
       CompactDispatchUsesTwoDimensionalGroupsAtTheNativeXBoundary)
{
    constexpr uint32 drawGroupCount = 65536u;
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = drawGroupCount;
    GPUCulling culling;
    culling.Initialize(&device, config);
    ASSERT_TRUE(culling.IsGPUSceneExecutionReady());

    culling.BeginFrame();
    const GPUInstanceData instance =
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 3u);
    for (uint32 groupIndex = 0u; groupIndex < drawGroupCount; ++groupIndex)
    {
        ASSERT_NE(RVX_INVALID_INDEX,
                  culling.BeginDrawGroup(1000u + groupIndex, 2000u + groupIndex));
        ASSERT_NE(RVX_INVALID_INDEX, culling.AddInstance(instance));
        culling.EndDrawGroup();
    }
    culling.EndFrame();

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());
    ASSERT_TRUE(culling.WasGpuExecutionUsedLastCull());
    ASSERT_EQ(3u, context.dispatches.size());
    EXPECT_EQ((std::array<uint32, 3>{1025u, 1u, 1u}),
              context.dispatches[0]);
    EXPECT_EQ((std::array<uint32, 3>{65535u, 2u, 1u}),
              context.dispatches[1]);
    EXPECT_EQ((std::array<uint32, 3>{1024u, 1u, 1u}),
              context.dispatches[2]);

    const FakeBuffer* constantsBuffer = static_cast<const FakeBuffer*>(
        culling.GetCullingConstantsBuffer());
    ASSERT_NE(nullptr, constantsBuffer);
    const GPUCullingConstants constants =
        ReadBufferValue<GPUCullingConstants>(*constantsBuffer);
    EXPECT_EQ(drawGroupCount, constants.counts[0]);
    EXPECT_EQ(drawGroupCount, constants.counts[1]);
    EXPECT_EQ(65535u, constants.counts[2]);

    const FakeBuffer* commandBuffer = device.FindBuffer(
        "GPUCulling.IndirectDrawBuffer");
    ASSERT_NE(nullptr, commandBuffer);
    const IndirectDrawIndexedCommand first =
        ReadBufferValue<IndirectDrawIndexedCommand>(*commandBuffer, 0u);
    const IndirectDrawIndexedCommand last =
        ReadBufferValue<IndirectDrawIndexedCommand>(
            *commandBuffer, drawGroupCount - 1u);
    EXPECT_EQ(0u, first.firstInstance);
    EXPECT_EQ(1u, first.instanceCount);
    EXPECT_EQ(drawGroupCount - 1u, last.firstInstance);
    EXPECT_EQ(1u, last.instanceCount);
}

TEST_F(GPUDrivenValidationFixture,
       DeterministicCompactCpuReferencePreservesDenseGroupOrderAcrossBlocks)
{
    // 65 entries force the first group through two 64-wide scan blocks. The
    // second group has mixed visibility and must not consume the first
    // group's output range.
    constexpr uint32 firstGroupCount = 65u;
    constexpr uint32 totalCount = firstGroupCount + 3u;
    std::array<uint32, totalCount> residentRows{};
    std::array<uint8, totalCount> visibility{};
    std::array<uint32, totalCount> compacted{};
    compacted.fill(RVX_INVALID_INDEX);
    visibility.fill(1u);
    for (uint32 index = 0u; index < firstGroupCount; ++index)
    {
        residentRows[index] = 100u + index;
    }
    residentRows[65u] = 200u;
    residentRows[66u] = 201u;
    residentRows[67u] = 202u;
    visibility[0u] = 0u;
    visibility[63u] = 0u;
    visibility[65u] = 0u;
    visibility[67u] = 0u;

    const auto compactGroup = [&residentRows, &visibility, &compacted](
                                  uint32 first,
                                  uint32 count)
    {
        uint32 visibleCount = 0u;
        for (uint32 blockStart = 0u; blockStart < count; blockStart += 64u)
        {
            const uint32 blockCount = std::min(64u, count - blockStart);
            for (uint32 localIndex = 0u; localIndex < blockCount; ++localIndex)
            {
                const uint32 activeIndex = first + blockStart + localIndex;
                if (visibility[activeIndex] != 0u)
                {
                    compacted[first + visibleCount++] = residentRows[activeIndex];
                }
            }
        }
    };
    compactGroup(0u, firstGroupCount);
    compactGroup(firstGroupCount, 3u);

    EXPECT_EQ(101u, compacted[0u]);
    EXPECT_EQ(162u, compacted[61u]);
    EXPECT_EQ(164u, compacted[62u]);
    EXPECT_EQ(RVX_INVALID_INDEX, compacted[63u]);
    EXPECT_EQ(201u, compacted[65u]);
    EXPECT_EQ(RVX_INVALID_INDEX, compacted[66u]);
    EXPECT_EQ(RVX_INVALID_INDEX, compacted[67u]);
}

TEST_F(GPUDrivenValidationFixture,
       StableTierOneRowsMaterializeCandidatesWhenCompanionsFirstArrive)
{
    FakeDevice device;
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    RenderScene scene;
    RenderObject object =
        MakeRenderObject(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 77u);
    object.entityId = 400u;
    object.objectRevision = 9u;
    scene.AddObject(object);
    const RenderDrawItem item = MakeDrawItem(0u, 77u, 88u);
    const auto collectTierOne = [&culling, &scene, &item]()
    {
        ASSERT_NE(RVX_INVALID_INDEX, culling.BeginDrawGroup(77u, 88u));
        ASSERT_EQ(0u,
                  culling.AddDrawItemInstance(
                      scene, item, GPUIndexedDrawDesc{36u, 0u, 0}, 0u));
        culling.EndDrawGroup();
    };

    GPUSceneCullingCandidate candidate{};
    candidate.primitiveSlot = 1u;
    candidate.primitiveGeneration = 7u;
    candidate.drawSlot = 3u;
    candidate.drawGeneration = 7u;
    candidate.objectIdLow = 400u;
    candidate.requiredPassMask = 2u;
    candidate.drawGroupIndex = 0u;
    candidate.drawGroupVisibleOffset = 0u;
    candidate.rasterInstanceIndex = 0u;
    candidate.materialParameterSlot = 3u;

    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    const GPUSceneResidentGraphLease lease =
        MakeGPUSceneLease(device, 79u, capacities);
    ASSERT_TRUE(lease.IsValid());

    // Frame one establishes the stable Tier 1 row without a companion.
    // Frame two preserves the same key and source revision, so it exercises
    // the candidate-baseline path rather than a normal object mutation.
    const std::array<uint64, 1> changedObjectIds{400u};
    culling.BeginFrame(1u, changedObjectIds, {}, true);
    collectTierOne();
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{93u, 1u, 1u, 0u, 1u}));
    EXPECT_FALSE(culling.HasCompleteGPUSceneCandidates());

    culling.BeginFrame(1u, {}, {}, false);
    collectTierOne();
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 79u));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGPUSceneGraph(
        GPUCullingRecordingIdentity{93u, 2u, 2u, 0u, 2u}, lease));
    EXPECT_GT(culling.GetLastGPUSceneCandidateUploadBytes(), 0u);

    // Once the baseline exists, an unchanged complete stream must stay quiet.
    culling.BeginFrame(1u, {}, {}, false);
    collectTierOne();
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 79u));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGPUSceneGraph(
        GPUCullingRecordingIdentity{93u, 2u, 3u, 0u, 3u}, lease));
    EXPECT_EQ(0u, culling.GetLastGPUSceneCandidateUploadBytes());
}

TEST_F(GPUDrivenValidationFixture,
       TierOneOnlyTransformChangeValidatesRetainedCandidateWithoutUploadingIt)
{
    FakeDevice device;
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    RenderScene scene;
    RenderObject object =
        MakeRenderObject(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 77u);
    object.entityId = 400u;
    object.objectRevision = 1u;
    scene.AddObject(object);
    const RenderDrawItem item = MakeDrawItem(0u, 77u, 88u);
    const auto collect = [&culling, &scene, &item]()
    {
        ASSERT_NE(RVX_INVALID_INDEX, culling.BeginDrawGroup(77u, 88u));
        ASSERT_EQ(0u,
                  culling.AddDrawItemInstance(
                      scene, item, GPUIndexedDrawDesc{36u, 0u, 0}, 0u));
        culling.EndDrawGroup();
    };

    GPUSceneCullingCandidate candidate{};
    candidate.primitiveSlot = 1u;
    candidate.primitiveGeneration = 7u;
    candidate.drawSlot = 3u;
    candidate.drawGeneration = 7u;
    candidate.objectIdLow = 400u;
    candidate.requiredPassMask = 2u;
    candidate.drawGroupIndex = 0u;
    candidate.drawGroupVisibleOffset = 0u;
    candidate.rasterInstanceIndex = 0u;
    candidate.materialParameterSlot = 3u;

    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    const GPUSceneResidentGraphLease lease =
        MakeGPUSceneLease(device, 79u, capacities);
    ASSERT_TRUE(lease.IsValid());

    const std::array<uint64, 1> changedObjectIds{400u};
    culling.BeginFrame(1u, changedObjectIds, {}, true);
    collect();
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 79u));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGPUSceneGraph(
        GPUCullingRecordingIdentity{95u, 2u, 1u, 0u, 1u}, lease));
    EXPECT_EQ(sizeof(GPUSceneCullingCandidate),
              culling.GetLastGPUSceneCandidateUploadBytes());

    // Tier 1 advances only its transform payload. Candidate bytes are absent,
    // so the row becomes invalid for candidate comparison without clearing the
    // retained candidate payload.
    RenderObject& transformed = scene.GetMutableObject(0u);
    transformed.objectRevision = 2u;
    transformed.worldMatrix[3].x = 2.0f;
    transformed.bounds = AABB(Vec3(1.0f, -1.0f, -6.0f),
                              Vec3(3.0f, 1.0f, -4.0f));
    culling.BeginFrame(2u, changedObjectIds, {}, false);
    collect();
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{95u, 1u, 2u, 0u, 2u}));
    EXPECT_FALSE(culling.HasCompleteGPUSceneCandidates());

    // The candidate is byte-identical to the retained baseline. It still has
    // to be compared because Tier 1 invalidated the row, but it needs no
    // sparse patch or upload once the comparison succeeds.
    culling.BeginFrame(2u, {}, {}, false);
    collect();
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 79u));
    culling.EndFrame();
    const std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGPUSceneGraph(
            GPUCullingRecordingIdentity{95u, 2u, 3u, 0u, 3u}, lease);
    ASSERT_NE(nullptr, recorded);
    EXPECT_EQ(0u, culling.GetIncrementalDiagnostics().candidatePatchedRowCount);
    EXPECT_EQ(0u, culling.GetLastGPUSceneCandidateUploadBytes());
    EXPECT_EQ(0u, recorded->GetCopiedCpuPayloadBytes());
    const FakeBuffer* candidateBuffer = static_cast<const FakeBuffer*>(
        culling.GetGPUSceneCandidateBuffer());
    ASSERT_NE(nullptr, candidateBuffer);
    EXPECT_EQ(MakeCanonicalGPUSceneCandidate(candidate, 0u),
              ReadBufferValue<GPUSceneCullingCandidate>(*candidateBuffer));
}

TEST_F(GPUDrivenValidationFixture,
       TierOneOnlyReusedRowPatchesExactlyOneCandidateWhenCompanionReturns)
{
    FakeDevice device;
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    RenderScene scene;
    RenderObject objectA =
        MakeRenderObject(Vec3(-1.0f, 0.0f, -5.0f), 1.0f, 77u);
    objectA.entityId = 100u;
    objectA.objectRevision = 1u;
    RenderObject objectB =
        MakeRenderObject(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 77u);
    objectB.entityId = 200u;
    objectB.objectRevision = 1u;
    scene.AddObject(objectA);
    scene.AddObject(objectB);
    const RenderDrawItem drawA = MakeDrawItem(0u, 77u, 88u);
    const RenderDrawItem drawB = MakeDrawItem(1u, 77u, 88u);
    const auto collect = [&culling, &scene](const RenderDrawItem& item,
                                             uint32 sourceIndex)
    {
        ASSERT_NE(RVX_INVALID_INDEX, culling.BeginDrawGroup(77u, 88u));
        ASSERT_EQ(0u,
                  culling.AddDrawItemInstance(
                      scene, item, GPUIndexedDrawDesc{36u, 0u, 0}, sourceIndex));
        culling.EndDrawGroup();
    };

    GPUSceneCullingCandidate candidateA{};
    candidateA.primitiveSlot = 1u;
    candidateA.primitiveGeneration = 7u;
    candidateA.drawSlot = 3u;
    candidateA.drawGeneration = 7u;
    candidateA.objectIdLow = 100u;
    candidateA.requiredPassMask = 2u;
    candidateA.drawGroupIndex = 0u;
    candidateA.drawGroupVisibleOffset = 0u;
    candidateA.rasterInstanceIndex = 0u;
    candidateA.materialParameterSlot = 3u;
    GPUSceneCullingCandidate candidateB = candidateA;
    candidateB.primitiveSlot = 2u;
    candidateB.drawSlot = 4u;
    candidateB.objectIdLow = 200u;

    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    const GPUSceneResidentGraphLease lease =
        MakeGPUSceneLease(device, 79u, capacities);
    ASSERT_TRUE(lease.IsValid());

    const std::array<uint64, 1> changedA{100u};
    culling.BeginFrame(1u, changedA, {}, true);
    collect(drawA, 0u);
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidateA, 79u));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGPUSceneGraph(
        GPUCullingRecordingIdentity{96u, 2u, 1u, 0u, 1u}, lease));
    const uint64 candidateFullMaterializationsBeforeReuse =
        culling.GetIncrementalDiagnostics().candidateFullMaterializationCount;

    // Retire A and admit B in the same Tier 1 collection. The deterministic
    // allocator reuses A's resident row, but no companion is available yet.
    const std::array<uint64, 1> changedB{200u};
    const std::array<uint64, 1> removedA{100u};
    culling.BeginFrame(2u, changedB, removedA, false);
    collect(drawB, 0u);
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{96u, 1u, 2u, 0u, 2u}));
    EXPECT_FALSE(culling.HasCompleteGPUSceneCandidates());

    culling.BeginFrame(2u, {}, {}, false);
    collect(drawB, 0u);
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidateB, 79u));
    culling.EndFrame();
    const std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGPUSceneGraph(
            GPUCullingRecordingIdentity{96u, 2u, 3u, 0u, 3u}, lease);
    ASSERT_NE(nullptr, recorded);
    EXPECT_EQ(1u, culling.GetIncrementalDiagnostics().candidatePatchedRowCount);
    EXPECT_EQ(sizeof(GPUSceneCullingCandidate),
              culling.GetLastGPUSceneCandidateUploadBytes());
    EXPECT_EQ(candidateFullMaterializationsBeforeReuse,
              culling.GetIncrementalDiagnostics()
                  .candidateFullMaterializationCount);
    EXPECT_EQ(0u, recorded->GetCopiedCpuPayloadBytes());
    const FakeBuffer* candidateBuffer = static_cast<const FakeBuffer*>(
        culling.GetGPUSceneCandidateBuffer());
    ASSERT_NE(nullptr, candidateBuffer);
    EXPECT_EQ(MakeCanonicalGPUSceneCandidate(candidateB, 0u),
              ReadBufferValue<GPUSceneCullingCandidate>(*candidateBuffer));
}

TEST_F(GPUDrivenValidationFixture,
       StableRowPreparationFailureRollsBackFreeRowsAndRetriesTheSameCanonicalSlot)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    RenderScene scene;
    RenderObject objectA = MakeRenderObject(Vec3(-1.0f, 0.0f, -5.0f), 1.0f, 77u);
    objectA.entityId = 100u;
    objectA.objectRevision = 1u;
    RenderObject objectB = MakeRenderObject(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 77u);
    objectB.entityId = 200u;
    objectB.objectRevision = 1u;
    scene.AddObject(objectA);
    scene.AddObject(objectB);
    const RenderDrawItem drawA = MakeDrawItem(0u, 77u, 88u);
    const RenderDrawItem drawB = MakeDrawItem(1u, 77u, 88u);
    const GPUCullingRecordingIdentity identity{92u, 1u, 1u, 0u, 1u};

    const auto collect = [&culling, &scene](const RenderDrawItem& item,
                                             uint32 sourceIndex)
    {
        ASSERT_NE(RVX_INVALID_INDEX, culling.BeginDrawGroup(77u, 88u));
        ASSERT_NE(RVX_INVALID_INDEX,
                  culling.AddDrawItemInstance(
                      scene, item, GPUIndexedDrawDesc{36u, 0u, 0}, sourceIndex));
        culling.EndDrawGroup();
    };

    const std::array<uint64, 1> changedA{100u};
    culling.BeginFrame(1u, changedA, {}, true);
    collect(drawA, 0u);
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(identity));
    const GPUCullingCanonicalMutationTotals beforeFailure =
        culling.GetMutationTotals();

    // B needs the currently lowest free physical row (1). Fail after that
    // free-row reservation but before canonical/object-map publication.
    const std::array<uint64, 1> changedB{200u};
    culling.BeginFrame(2u, changedB, {}, false);
    collect(drawA, 0u);
    collect(drawB, 1u);
    culling.SetStableRowPreparationFailureCountdownForTesting(0);
    culling.EndFrame();
    EXPECT_EQ(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{92u, 1u, 2u, 0u, 2u}));
    EXPECT_EQ(culling.GetMutationTotals().instancePatchedRowCount,
              beforeFailure.instancePatchedRowCount);
    EXPECT_EQ(culling.GetMutationTotals().instanceUploadedRowCount,
              beforeFailure.instanceUploadedRowCount);

    // The exact same collection must be retryable. A leaked reservation
    // would instead materialize B at row 2 and leave row 1 stale.
    culling.SetStableRowPreparationFailureCountdownForTesting(-1);
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{92u, 1u, 3u, 0u, 3u}));
    EXPECT_EQ(sizeof(GPUInstanceData), culling.GetLastInstanceUploadBytes());
    EXPECT_GT(culling.GetMutationTotals().instancePatchedRowCount,
              beforeFailure.instancePatchedRowCount);
    EXPECT_GT(culling.GetMutationTotals().instanceUploadedRowCount,
              beforeFailure.instanceUploadedRowCount);
    const FakeBuffer* instances = static_cast<const FakeBuffer*>(
        culling.GetInstanceBuffer());
    ASSERT_NE(nullptr, instances);
    const GPUInstanceData rowOne = ReadBufferValue<GPUInstanceData>(*instances, 1u);
    EXPECT_EQ(77u, rowOne.meshId);
    EXPECT_EQ(88u, rowOne.materialId);
}

TEST_F(GPUDrivenValidationFixture,
       GrowingCullingCapacityReservesCollectionVectorsBeforeAdmittingNewRows)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 1;
    GPUCulling culling;
    culling.Initialize(&device, config);

    GPUCullingConfig grown = config;
    grown.maxInstances = 3;
    culling.SetConfig(grown);
    EXPECT_EQ(3u, culling.GetConfig().maxInstances);

    culling.BeginFrame();
    EXPECT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(-1.0f, 0.0f, -5.0f), 1.0f, 36u)));
    EXPECT_EQ(1u, culling.AddInstance(
        MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 36u)));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{93u, 1u, 1u, 0u, 1u}));
    EXPECT_EQ(2u * sizeof(GPUInstanceData),
              culling.GetLastInstanceUploadBytes());
}

TEST_F(GPUDrivenValidationFixture,
       DirtyJournalFailureInvalidatesEverySlotAndForcesFullCanonicalRecovery)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config, 2);

    ASSERT_TRUE(culling.SetFrameSlot(0u));
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(-1.0f, 0.0f, -5.0f), 1.0f, 36u)));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{94u, 1u, 1u, 0u, 1u}));

    ASSERT_TRUE(culling.SetFrameSlot(1u));
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(-1.0f, 0.0f, -5.0f), 1.0f, 36u)));
    ASSERT_EQ(1u, culling.AddInstance(
        MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 36u)));
    const uint64 continuityBeforeJournalFailure =
        culling.GetMutationTotals().continuityFullMaterializationCount;
    culling.SetDirtyJournalFailureCountdownForTesting(0);
    culling.EndFrame();
    culling.SetDirtyJournalFailureCountdownForTesting(-1);
    EXPECT_EQ(culling.GetMutationTotals().continuityFullMaterializationCount,
              continuityBeforeJournalFailure + 1U);

    // The journal was not publishable, so even the previously warmed slot
    // must re-materialize canonical bytes rather than infer a missing delta.
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{94u, 1u, 2u, 1u, 2u}));
    EXPECT_EQ(2u * sizeof(GPUInstanceData),
              culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(2u * sizeof(GPUCullingActiveRow),
              culling.GetLastActiveRowUploadBytes());
    ASSERT_TRUE(culling.SetFrameSlot(0u));
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{94u, 1u, 3u, 0u, 3u}));
    EXPECT_EQ(2u * sizeof(GPUInstanceData),
              culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(2u * sizeof(GPUCullingActiveRow),
              culling.GetLastActiveRowUploadBytes());
    EXPECT_GT(culling.GetIncrementalDiagnostics().continuityFullMaterializationCount,
              0u);
    // The next static slot is forced to upload a full recovery, but it must
    // not erase the earlier journal-discontinuity evidence.
    EXPECT_EQ(culling.GetMutationTotals().continuityFullMaterializationCount,
              continuityBeforeJournalFailure + 1U);
}

TEST_F(GPUDrivenValidationFixture, ScopedUAVBarrierCarriesMemoryDependency)
{
    const RHIAccessSnapshot unorderedAccess = MakeRHIAccessSnapshot(
        RHIResourceState::UnorderedAccess,
        RHIShaderStage::Compute,
        GPUQueueDomain::Graphics);
    const RHIBufferBarrier barrier = MakeRHIBufferBarrier(
        nullptr, unorderedAccess, unorderedAccess);

    EXPECT_TRUE(barrier.hasScopedAccess);
    EXPECT_EQ(RHIResourceState::UnorderedAccess, barrier.stateBefore);
    EXPECT_EQ(RHIResourceState::UnorderedAccess, barrier.stateAfter);
    EXPECT_TRUE(HasDependencyKind(barrier.dependencyKind,
                                  RHIDependencyKind::Memory));
}

TEST_F(GPUDrivenValidationFixture, GPUDrivenDrawFallbackReasonNamesAreStable)
{
    EXPECT_STREQ(GetGPUDrivenDrawFallbackReasonName(
                     GPUDrivenDrawFallbackReason::None),
                 "None");
    EXPECT_STREQ(GetGPUDrivenDrawFallbackReasonName(
                     GPUDrivenDrawFallbackReason::PipelineUnavailable),
                 "PipelineUnavailable");
    EXPECT_STREQ(GetGPUDrivenDrawFallbackReasonName(
                     GPUDrivenDrawFallbackReason::Disabled),
                 "Disabled");
}

TEST_F(GPUDrivenValidationFixture, GPUCullingDeclaresComputeCompactionAndIndirectCountContracts)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::string header =
        ReadTextFile(root / "Render" / "Include" / "Render" / "GPUDriven" / "GPUCulling.h");
    const std::string source =
        ReadTextFile(root / "Render" / "Private" / "GPUDriven" / "GPUCulling.cpp");
    const std::string rhiCommandContext =
        ReadTextFile(root / "RHI" / "Include" / "RHI" / "RHICommandContext.h");
    const std::string shader =
        ReadTextFile(root / "Render" / "Shaders" / "GPUDriven" / "GPUCulling.hlsl");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(rhiCommandContext.empty());
    ASSERT_FALSE(shader.empty());

    EXPECT_NE(header.find("RHIDescriptorSetRef descriptorSet"), std::string::npos);
    EXPECT_NE(header.find("std::vector<GPUCullingFrameInputs> m_frameInputs"),
              std::string::npos);
    EXPECT_NE(header.find("RHIPipelineRef m_frustumCullPipeline"), std::string::npos);
    EXPECT_NE(header.find("RHIPipelineRef m_compactPipeline"), std::string::npos);
    EXPECT_NE(header.find("WasGpuExecutionUsedLastCull"), std::string::npos);
    EXPECT_NE(header.find("GPUCullingFallbackReason"), std::string::npos);
    EXPECT_NE(header.find("GetExecutionDecision"), std::string::npos);
    EXPECT_NE(header.find("GetLastFallbackReason"), std::string::npos);

    EXPECT_NE(source.find("CreatePipelineResources()"), std::string::npos);
    EXPECT_NE(source.find("EvaluateGpuExecution"), std::string::npos);
    EXPECT_NE(source.find("supportsComputePipeline"), std::string::npos);
    EXPECT_NE(source.find("CreateComputePipeline"), std::string::npos);
    EXPECT_NE(source.find("inputs.descriptorSet = m_device->CreateDescriptorSet"),
              std::string::npos);
    EXPECT_NE(source.find("ctx.SetDescriptorSet(0, inputs->descriptorSet.Get())"),
              std::string::npos);
    EXPECT_NE(source.find("BuildIndexedIndirectGroupSubmission"), std::string::npos);
    EXPECT_NE(header.find("RHIIndexedIndirectExecutionDesc"), std::string::npos);
    EXPECT_EQ(source.find("ctx.DrawIndexedIndirectCount"), std::string::npos);
    EXPECT_NE(source.find("GPUCulling.ActiveRowsBuffer"), std::string::npos);
    EXPECT_NE(source.find("GPUCullingActiveRow"), std::string::npos);
    EXPECT_NE(source.find("(static_cast<uint64>(m_config.maxInstances) + 1u)"),
              std::string::npos);
    EXPECT_NE(source.find("const uint32 clearThreadCount = std::max("),
              std::string::npos);
    EXPECT_NE(source.find("m_incrementalDiagnostics.activeRowCount"),
              std::string::npos);
    EXPECT_NE(source.find("GetCompactDispatchDimensions"),
              std::string::npos);
    EXPECT_NE(source.find("constants.counts[2] = compactDispatch.width"),
              std::string::npos);
    EXPECT_NE(source.find("BuildGpuIndirectCommandPrefill"),
              std::string::npos);
    EXPECT_NE(source.find("const auto insertCullUAVBarriers"), std::string::npos);
    EXPECT_NE(source.find("ctx.BufferBarrier(m_visibilityBuffer.Get(), computeUAVAccess, computeUAVAccess)"),
              std::string::npos);
    EXPECT_NE(source.find("ctx.BufferBarrier(m_indirectBuffer.Get(), computeUAVAccess, computeUAVAccess)"),
              std::string::npos);
    EXPECT_NE(source.find("ctx.BufferBarrier(m_drawCountBuffer.Get(), computeUAVAccess, computeUAVAccess)"),
              std::string::npos);
    EXPECT_NE(source.find("FrustumRejectRelativeTolerance"),
              std::string::npos);
    EXPECT_NE(source.find("IsConservativelyOutsideFrustumPlane"),
              std::string::npos);
    EXPECT_EQ(source.find("planes[i] /= len"), std::string::npos);
    const size_t frustumDispatch = source.find("ctx.Dispatch(cullGroupCount, 1, 1);");
    const size_t cullBarriers = source.find("insertCullUAVBarriers();", frustumDispatch);
    const size_t compactDispatch = source.find("// Compact visible instances into draw commands", cullBarriers);
    ASSERT_NE(frustumDispatch, std::string::npos);
    ASSERT_NE(cullBarriers, std::string::npos);
    ASSERT_NE(compactDispatch, std::string::npos);
    EXPECT_LT(frustumDispatch, cullBarriers);
    EXPECT_LT(cullBarriers, compactDispatch);

    EXPECT_NE(rhiCommandContext.find("DrawIndexedIndirectCount"), std::string::npos);
    EXPECT_NE(shader.find("void CSFrustumCull"), std::string::npos);
    EXPECT_NE(shader.find("void CSCompactDraws"), std::string::npos);
    EXPECT_NE(shader.find("gDrawCount[activeIndex] = 0"), std::string::npos);
    EXPECT_NE(shader.find("uint drawGroupCount = Counts.y"),
              std::string::npos);
    EXPECT_NE(shader.find("void CSFinalizeDrawGroups"), std::string::npos);
    EXPECT_NE(shader.find("uint3 groupId : SV_GroupID"),
              std::string::npos);
    EXPECT_NE(shader.find("groupId.y * Counts.z"), std::string::npos);
    EXPECT_NE(shader.find("blockStart += 64u"), std::string::npos);
    EXPECT_NE(shader.find("gDrawCount[drawGroupIndex + 1u] = gCompactVisibleBase"),
              std::string::npos);
    EXPECT_NE(shader.find("command.instanceCount = visibleCount"),
              std::string::npos);
    EXPECT_NE(shader.find("const IndirectDrawIndexedCommand command"),
              std::string::npos);
    const size_t compactBody = shader.find("void CSCompactDraws");
    const size_t finalizeBody = shader.find("void CSFinalizeDrawGroups");
    ASSERT_NE(compactBody, std::string::npos);
    ASSERT_NE(finalizeBody, std::string::npos);
    EXPECT_EQ(shader.substr(compactBody, finalizeBody - compactBody)
                  .find("InterlockedAdd"),
              std::string::npos);
    EXPECT_NE(shader.find("RVX_FRUSTUM_REJECT_RELATIVE_TOLERANCE"),
              std::string::npos);
    EXPECT_NE(shader.find("ConservativeFrustumRejectTolerance"),
              std::string::npos);
}

TEST_F(GPUDrivenValidationFixture, OpaquePassDeclaresGPUDrivenDefaultLitIndirectContracts)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::string defaultLit =
        ReadTextFile(root / "Render" / "Shaders" / "DefaultLit.hlsl");
    const std::string depthOnly =
        ReadTextFile(root / "Render" / "Shaders" / "DepthOnly.hlsl");
    const std::string cullingShader =
        ReadTextFile(root / "Render" / "Shaders" / "GPUDriven" / "GPUCulling.hlsl");
    const std::string sharedInstance =
        ReadTextFile(root / "Render" / "Shaders" / "Include" / "GPUInstanceData.hlsli");
    const std::string cullingHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "GPUDriven" / "GPUCulling.h");
    const std::string rasterInstanceHeader = ReadTextFile(
        root / "Render" / "Include" / "Render" / "Submission" /
        "RasterInstanceStream.h");
    const std::string pipelineHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "PipelineCache.h");
    const std::string pipelineSource =
        ReadTextFile(root / "Render" / "Private" / "PipelineCache.cpp");
    const std::string opaqueHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Passes" / "OpaquePass.h");
    const std::string opaqueSource =
        ReadTextFile(root / "Render" / "Private" / "Passes" / "OpaquePass.cpp");
    // The GPU-driven showcase is the current pure-ECS production sample.  Keep
    // this source contract tied to the engine-owned policy/readiness path so a
    // deleted legacy ModelViewer entry point cannot become a hidden dependency.
    const std::string gpuDrivenSample = ReadTextFile(
        root / "Samples" / "RenderVerseSamples" / "Scenes" /
        "GPUDrivenShowcaseSample.cpp");
    const std::string testsCMake = ReadTextFile(root / "Tests" / "CMakeLists.txt");
    ASSERT_FALSE(defaultLit.empty());
    ASSERT_FALSE(depthOnly.empty());
    ASSERT_FALSE(cullingShader.empty());
    ASSERT_FALSE(sharedInstance.empty());
    ASSERT_FALSE(cullingHeader.empty());
    ASSERT_FALSE(rasterInstanceHeader.empty());
    ASSERT_FALSE(pipelineHeader.empty());
    ASSERT_FALSE(pipelineSource.empty());
    ASSERT_FALSE(opaqueHeader.empty());
    ASSERT_FALSE(opaqueSource.empty());
    ASSERT_FALSE(gpuDrivenSample.empty());
    ASSERT_FALSE(testsCMake.empty());

    EXPECT_EQ(opaqueSource.find("EnsureIndirectDrawCapacity"),
              std::string::npos);
    EXPECT_EQ(opaqueSource.find("OpaquePass.IndirectDrawBuffer"),
              std::string::npos);
    EXPECT_EQ(opaqueHeader.find("SetIndirectBatchingEnabled"),
              std::string::npos);

    EXPECT_NE(defaultLit.find("StructuredBuffer<GPUInstanceData> GPUDrivenInstances"), std::string::npos);
    EXPECT_NE(defaultLit.find("PSInput VSMainGPUDriven"), std::string::npos);
    EXPECT_NE(defaultLit.find("instance.normalMatrix"), std::string::npos);
    EXPECT_NE(defaultLit.find("uint InstanceIndex : INSTANCE_INDEX"), std::string::npos);
    EXPECT_NE(defaultLit.find("GPUDrivenInstances[input.InstanceIndex]"), std::string::npos);
    EXPECT_EQ(defaultLit.find("GPUDrivenInstances[instanceId]"), std::string::npos);
    EXPECT_NE(depthOnly.find("uint InstanceIndex : INSTANCE_INDEX"), std::string::npos);
    EXPECT_NE(depthOnly.find("GPUDrivenInstances[input.InstanceIndex]"), std::string::npos);
    EXPECT_EQ(depthOnly.find("GPUDrivenInstances[instanceId]"), std::string::npos);
    EXPECT_NE(defaultLit.find("#include \"Include/GPUInstanceData.hlsli\""),
              std::string::npos);
    EXPECT_NE(depthOnly.find("#include \"Include/GPUInstanceData.hlsli\""),
              std::string::npos);
    EXPECT_NE(cullingShader.find("#include \"../Include/GPUInstanceData.hlsli\""),
              std::string::npos);
    EXPECT_EQ(defaultLit.find("struct GPUInstanceData"), std::string::npos);
    EXPECT_EQ(depthOnly.find("struct GPUInstanceData"), std::string::npos);
    EXPECT_EQ(cullingShader.find("struct GPUInstanceData"), std::string::npos);
    EXPECT_NE(sharedInstance.find("uint candidateIndex;"), std::string::npos);
    EXPECT_NE(sharedInstance.find("uint forceVisible;"), std::string::npos);
    EXPECT_NE(sharedInstance.find("uint2 padding;"), std::string::npos);
    EXPECT_NE(rasterInstanceHeader.find("struct alignas(16) GPUInstanceData"),
              std::string::npos);
    EXPECT_NE(rasterInstanceHeader.find("sizeof(GPUInstanceData) == 224"),
              std::string::npos);
    EXPECT_NE(rasterInstanceHeader.find("alignof(GPUInstanceData) == 16"),
              std::string::npos);
    EXPECT_NE(rasterInstanceHeader.find("offsetof(GPUInstanceData, forceVisible) == 212"),
              std::string::npos);
    EXPECT_NE(rasterInstanceHeader.find("offsetof(GPUInstanceData, padding) == 216"),
              std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetGPUDrivenPipelineForVariant"), std::string::npos);
    EXPECT_NE(pipelineSource.find("GPUDrivenOpaquePipeline"), std::string::npos);
    EXPECT_NE(pipelineSource.find("RVX_PIPELINE_PURPOSE_GPU_DRIVEN_DEFAULT"), std::string::npos);
    EXPECT_NE(pipelineSource.find("AddElement(\"INSTANCE_INDEX\", RHIFormat::R32_UINT, 6)"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("instanceIndexElement.perInstance = true"), std::string::npos);
    EXPECT_NE(pipelineSource.find("instanceIndexElement.instanceDataStepRate = 1"), std::string::npos);
    EXPECT_EQ(opaqueHeader.find("SetGPUDriven" "CullingSource"),
              std::string::npos);
    EXPECT_EQ(opaqueHeader.find("SetGPUDriven" "RenderGraphResources"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("TryDrawGPUDrivenIndirect"), std::string::npos);
    EXPECT_NE(opaqueSource.find("BuildIndexedIndirectGroupSubmission"), std::string::npos);
    EXPECT_NE(opaqueSource.find("IndexedIndirectRenderSubmissionStrategy"), std::string::npos);
    EXPECT_NE(opaqueSource.find("ctx.SetVertexBuffer(6, m_gpuCulling->GetVisibleInstanceBuffer())"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("DeclareMaterialTextureGraphReads"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("gpuCulling->GetDrawGroups()"),
              std::string::npos);
    EXPECT_EQ(opaqueSource.find("TransitionGPUDrivenGroupMaterialTextures"),
              std::string::npos);
    EXPECT_EQ(opaqueSource.find("TransitionVisibleMaterialTextures"),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find("#include \"Scene/ECS/RenderFragments.h\""),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find(
                  "context.renderSettings.gpuCulling.mode ="),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find("RenderGPUDrivenMode::ForceEnabled"),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find("RenderGPUDrivenMode::ForceDisabled"),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find(
                  "context.sceneLifetime.CreateAndAdoptWithFragments("),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find("context.models.RequestByAssetId("),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find(
                  "bool GPUDrivenShowcaseSample::IsGPUDrivenReady("),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find(
                  "bool GPUDrivenShowcaseSample::IsDirectReady("),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find(
                  "diagnostics.gpuDrivenOpaqueIndirectRequested"),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find(
                  "diagnostics.gpuDrivenOpaqueIndirectEligible"),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find(
                  "diagnostics.gpuDrivenOpaqueIndirectSubmitted"),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find(
                  "diagnostics.gpuDrivenOpaqueIndirectBatchCount > 0"),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find(
                  "diagnostics.gpuDrivenOpaqueIndirectDrawUpperBound > 0"),
              std::string::npos);
    EXPECT_NE(gpuDrivenSample.find(
                  "diagnostics.opaqueInstancingPreflightSucceeded"),
              std::string::npos);
    EXPECT_EQ(gpuDrivenSample.find("GPUCulling"), std::string::npos);
    EXPECT_EQ(gpuDrivenSample.find("RenderGraph"), std::string::npos);
    EXPECT_NE(testsCMake.find("rvx_add_renderverse_gpu_driven_parity"),
              std::string::npos);
    EXPECT_NE(testsCMake.find("--sample gpu-driven"), std::string::npos);
    EXPECT_NE(testsCMake.find("--require-gpu-driven-execution"),
              std::string::npos);
    EXPECT_NE(testsCMake.find("--require-direct-execution"),
              std::string::npos);
    EXPECT_NE(testsCMake.find("GPUDrivenPathRequested"), std::string::npos);
    EXPECT_NE(testsCMake.find("DirectPathRequested"), std::string::npos);
    EXPECT_NE(testsCMake.find("--tolerance 0.0"), std::string::npos);
    EXPECT_NE(testsCMake.find("--max-different-pixels 0"), std::string::npos);
}

TEST_F(GPUDrivenValidationFixture, GPUCullingComputeShaderEntriesCompileForDX12)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::filesystem::path shaderPath =
        root / "Render" / "Shaders" / "GPUDriven" / "GPUCulling.hlsl";
    const std::string shader = ReadTextFile(shaderPath);
    ASSERT_FALSE(shader.empty());

    std::unique_ptr<IShaderCompiler> compiler = CreateShaderCompiler();
    ASSERT_NE(nullptr, compiler);

    const std::string shaderPathString = shaderPath.string();
    ShaderCompileOptions options;
    options.stage = RHIShaderStage::Compute;
    options.sourceCode = shader.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RHIBackendType::DX12;
    options.targetProfile = "cs_6_0";
    options.enableDebugInfo = false;
    options.enableOptimization = true;

    const ShaderCompileSupport support = compiler->QuerySupport(options);
    if (!support.IsSupported())
    {
        GTEST_SKIP() << support.reason;
    }

    options.entryPoint = "CSFrustumCull";
    ShaderCompileResult frustumResult = compiler->Compile(options);
    ASSERT_TRUE(frustumResult.success) << frustumResult.errorMessage;
    EXPECT_FALSE(frustumResult.bytecode.empty());

    options.entryPoint = "CSCompactDraws";
    ShaderCompileResult compactResult = compiler->Compile(options);
    ASSERT_TRUE(compactResult.success) << compactResult.errorMessage;
    EXPECT_FALSE(compactResult.bytecode.empty());

    options.entryPoint = "CSFinalizeDrawGroups";
    ShaderCompileResult finalizeResult = compiler->Compile(options);
    ASSERT_TRUE(finalizeResult.success) << finalizeResult.errorMessage;
    EXPECT_FALSE(finalizeResult.bytecode.empty());
}

TEST_F(GPUDrivenValidationFixture,
       GPUCullingComputeShaderEntriesCompileForVulkanWithExpectedLayout)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::filesystem::path shaderPath =
        root / "Render" / "Shaders" / "GPUDriven" / "GPUCulling.hlsl";
    const std::string shader = ReadTextFile(shaderPath);
    ASSERT_FALSE(shader.empty());

    std::unique_ptr<IShaderCompiler> compiler = CreateShaderCompiler();
    ASSERT_NE(nullptr, compiler);

    const std::string shaderPathString = shaderPath.string();
    ShaderCompileOptions options;
    options.stage = RHIShaderStage::Compute;
    options.sourceCode = shader.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RHIBackendType::Vulkan;
    options.targetProfile = "cs_6_0";
    options.enableDebugInfo = false;
    options.enableOptimization = true;

    const ShaderCompileSupport support = compiler->QuerySupport(options);
    if (!support.IsSupported())
    {
        GTEST_SKIP() << support.reason;
    }

    options.entryPoint = "CSFrustumCull";
    ShaderCompileResult frustumResult = compiler->Compile(options);
    ASSERT_TRUE(frustumResult.success) << frustumResult.errorMessage;
    ASSERT_FALSE(frustumResult.bytecode.empty());
    ASSERT_TRUE(frustumResult.reflection.valid);
    ASSERT_EQ(frustumResult.bytecode.size() % sizeof(uint32), 0u);

    std::vector<uint32> spirvWords(
        frustumResult.bytecode.size() / sizeof(uint32));
    std::memcpy(spirvWords.data(), frustumResult.bytecode.data(),
                frustumResult.bytecode.size());
    ASSERT_GE(spirvWords.size(), 5u);
    ASSERT_EQ(spirvWords[0], 0x07230203u);

    bool hasInstanceArrayStride224 = false;
    bool hasStaleInstanceArrayStride216 = false;
    for (size_t wordIndex = 5; wordIndex < spirvWords.size();)
    {
        const uint16 wordCount = static_cast<uint16>(spirvWords[wordIndex] >> 16u);
        const uint16 opcode = static_cast<uint16>(spirvWords[wordIndex] & 0xFFFFu);
        ASSERT_NE(wordCount, 0u);
        ASSERT_LE(wordIndex + wordCount, spirvWords.size());

        // OpDecorate <target-id> ArrayStride <literal>.
        if (opcode == 71u && wordCount >= 4u && spirvWords[wordIndex + 2] == 6u)
        {
            hasInstanceArrayStride224 |= spirvWords[wordIndex + 3] == 224u;
            hasStaleInstanceArrayStride216 |= spirvWords[wordIndex + 3] == 216u;
        }
        wordIndex += wordCount;
    }
    EXPECT_TRUE(hasInstanceArrayStride224);
    EXPECT_FALSE(hasStaleInstanceArrayStride216);
    const auto hasBinding = [](const ShaderCompileResult& result,
                               uint32 binding,
                               RHIBindingType type)
    {
        return std::any_of(
            result.reflection.resources.begin(),
            result.reflection.resources.end(),
            [binding, type](const ShaderReflection::ResourceBinding& resource)
            {
                return resource.set == 0 && resource.binding == binding &&
                       resource.type == type;
            });
    };
    EXPECT_TRUE(hasBinding(frustumResult, 0, RHIBindingType::UniformBuffer));
    EXPECT_TRUE(hasBinding(frustumResult, 1, RHIBindingType::ShaderResourceBuffer));
    EXPECT_TRUE(hasBinding(frustumResult, 2, RHIBindingType::ShaderResourceBuffer));
    EXPECT_TRUE(hasBinding(frustumResult, 3, RHIBindingType::StorageBuffer));
    EXPECT_TRUE(hasBinding(frustumResult, 6, RHIBindingType::StorageBuffer));

    options.entryPoint = "CSCompactDraws";
    ShaderCompileResult compactResult = compiler->Compile(options);
    ASSERT_TRUE(compactResult.success) << compactResult.errorMessage;
    EXPECT_FALSE(compactResult.bytecode.empty());
    EXPECT_TRUE(compactResult.reflection.valid);
    EXPECT_TRUE(hasBinding(compactResult, 0, RHIBindingType::UniformBuffer));
    // Deterministic compaction consumes dense active rows and the command
    // prefill; it no longer needs to read the sparse instance payload.
    EXPECT_FALSE(hasBinding(compactResult, 1, RHIBindingType::ShaderResourceBuffer));
    EXPECT_TRUE(hasBinding(compactResult, 2, RHIBindingType::ShaderResourceBuffer));
    EXPECT_TRUE(hasBinding(compactResult, 3, RHIBindingType::StorageBuffer));
    EXPECT_TRUE(hasBinding(compactResult, 4, RHIBindingType::StorageBuffer));
    EXPECT_TRUE(hasBinding(compactResult, 5, RHIBindingType::StorageBuffer));
    EXPECT_TRUE(hasBinding(compactResult, 6, RHIBindingType::StorageBuffer));

    options.entryPoint = "CSFinalizeDrawGroups";
    ShaderCompileResult finalizeResult = compiler->Compile(options);
    ASSERT_TRUE(finalizeResult.success) << finalizeResult.errorMessage;
    EXPECT_FALSE(finalizeResult.bytecode.empty());
}

TEST_F(GPUDrivenValidationFixture,
       GPUSceneCandidateAbiAndSealedInputPlumbingRemainRendererPrivate)
{
    EXPECT_EQ(48u, sizeof(GPUSceneCullingCandidate));

    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);
    EXPECT_EQ(nullptr, culling.GetGPUSceneCandidateBuffer());
    ASSERT_TRUE(culling.IsGPUSceneExecutionReady());

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));

    GPUSceneCullingCandidate candidate;
    candidate.primitiveSlot = 1u;
    candidate.primitiveGeneration = 7u;
    candidate.drawSlot = 3u;
    candidate.drawGeneration = 7u;
    candidate.objectIdLow = 42u;
    candidate.requiredPassMask = 2u;
    candidate.drawGroupIndex = 0u;
    candidate.drawGroupVisibleOffset = 0u;
    candidate.rasterInstanceIndex = 0u;
    candidate.materialParameterSlot = 3u;
    GPUSceneCullingCandidate multiPassCandidate = candidate;
    multiPassCandidate.requiredPassMask = 3u;
    EXPECT_FALSE(culling.AddGPUSceneCandidate(multiPassCandidate, 19u));
    EXPECT_TRUE(culling.AddGPUSceneCandidate(candidate, 19u));

    ASSERT_EQ(1u, culling.AddInstance(
        MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 36)));
    GPUSceneCullingCandidate secondCandidate = candidate;
    secondCandidate.drawSlot = 4u;
    secondCandidate.rasterInstanceIndex = 1u;
    // This candidate has passed all positional/group checks and can fail only
    // at the committed-version lock.
    EXPECT_FALSE(culling.AddGPUSceneCandidate(secondCandidate, 20u));
    EXPECT_FALSE(culling.HasCompleteGPUSceneCandidates());
    EXPECT_TRUE(culling.AddGPUSceneCandidate(secondCandidate, 19u));
    EXPECT_TRUE(culling.HasCompleteGPUSceneCandidates());
    const uint32 tier1InstanceCount = culling.GetInstanceCount();
    const size_t tier1GroupCount = culling.GetDrawGroups().size();
    EXPECT_FALSE(culling.HasCompleteGPUSceneCandidates(18u));
    EXPECT_FALSE(culling.HasCompleteGPUSceneCandidates(20u));
    EXPECT_TRUE(culling.HasCompleteGPUSceneCandidates(19u));
    EXPECT_EQ(tier1InstanceCount, culling.GetInstanceCount());
    EXPECT_EQ(tier1GroupCount, culling.GetDrawGroups().size());

    culling.EndFrame();
    const GPUCullingRecordingIdentity identity{301u, 12u, 100u, 0u, 7u};
    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    GPUSceneResidentGraphLease lease = MakeGPUSceneLease(device, 19u, capacities);
    ASSERT_TRUE(lease.IsValid());

    const std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGPUSceneGraph(identity, lease);
    ASSERT_NE(nullptr, recorded);
    EXPECT_EQ(0U,
              culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(2U * sizeof(GPUSceneCullingCandidate),
              culling.GetLastGPUSceneCandidateUploadBytes());
    // A renderer attempt can switch to Direct or omit this lane without
    // calling BeginFrame on this owner. The diagnostic reset must therefore
    // be independent from collection state.
    culling.ResetFrameUploadDiagnostics();
    EXPECT_EQ(0U, culling.GetLastInstanceUploadBytes());
    EXPECT_EQ(0U, culling.GetLastGPUSceneCandidateUploadBytes());
    EXPECT_TRUE(culling.HasCompleteGPUSceneCandidates());
    EXPECT_TRUE(culling.HasCompleteGPUSceneCandidates(19u));
    EXPECT_TRUE(recorded->GetCulling().HasCompleteGPUSceneCandidates());
    EXPECT_TRUE(recorded->GetCulling().HasCompleteGPUSceneCandidates(19u));
    EXPECT_FALSE(recorded->GetCulling().HasCompleteGPUSceneCandidates(20u));

    const FakeBuffer* candidateBuffer = static_cast<const FakeBuffer*>(
        recorded->GetCulling().GetGPUSceneCandidateBuffer());
    ASSERT_NE(nullptr, candidateBuffer);
    EXPECT_EQ(MakeCanonicalGPUSceneCandidate(candidate, 0u),
              ReadBufferValue<GPUSceneCullingCandidate>(*candidateBuffer, 0));
    EXPECT_EQ(MakeCanonicalGPUSceneCandidate(secondCandidate, 1u),
              ReadBufferValue<GPUSceneCullingCandidate>(*candidateBuffer, 1));
    EXPECT_EQ(RHIContentValidity::Valid,
              recorded->GetAccessSnapshots().gpuSceneCandidates
                  .uniformAccess.contentValidity);

    const FakeBuffer* constantsBuffer = static_cast<const FakeBuffer*>(
        recorded->GetCulling().GetCullingConstantsBuffer());
    ASSERT_NE(nullptr, constantsBuffer);
    const GPUCullingConstants constants =
        ReadBufferValue<GPUCullingConstants>(*constantsBuffer);
    ASSERT_EQ(1U, constantsBuffer->GetMappedRanges().size());
    EXPECT_EQ(0U, constantsBuffer->GetMappedRanges()[0].first);
    EXPECT_EQ(sizeof(GPUCullingConstants),
              constantsBuffer->GetMappedRanges()[0].second);
    EXPECT_EQ(2u, constants.counts[0]);
    EXPECT_EQ(1u, constants.counts[1]);
    EXPECT_EQ(1u, constants.counts[2]);
    EXPECT_EQ(0u, constants.counts[3]);
    for (uint32 tableIndex = 0; tableIndex < 4; ++tableIndex)
    {
        EXPECT_EQ(capacities[tableIndex], constants.gpuSceneTableCounts0[tableIndex]);
    }
    EXPECT_EQ(capacities[4], constants.gpuSceneTableCounts1[0]);
    EXPECT_EQ(capacities[5], constants.gpuSceneTableCounts1[1]);
    EXPECT_EQ(0u, constants.gpuSceneTableCounts1[2]);
    EXPECT_EQ(0u, constants.gpuSceneTableCounts1[3]);
    EXPECT_EQ(RHIContentValidity::Valid,
              recorded->GetAccessSnapshots().constants.uniformAccess.contentValidity);

    const size_t bufferCountAfterFirstSeal = device.createdBuffers.size();
    const uint32 descriptorCountAfterFirstSeal =
        device.createDescriptorSetCalls;
    const std::shared_ptr<GPUCullingRecordedState> repeatedRecording =
        culling.SealForGPUSceneGraph(identity, lease);
    ASSERT_NE(nullptr, repeatedRecording);
    EXPECT_EQ(bufferCountAfterFirstSeal, device.createdBuffers.size());
    EXPECT_EQ(descriptorCountAfterFirstSeal,
              device.createDescriptorSetCalls);
    EXPECT_EQ(recorded->GetCulling().GetGPUSceneCandidateBuffer(),
              repeatedRecording->GetCulling().GetGPUSceneCandidateBuffer());
    EXPECT_EQ(recorded->GetCulling().GetIndirectBuffer(),
              repeatedRecording->GetCulling().GetIndirectBuffer());

    const GPUSceneRasterResourceSnapshot rasterResources =
        recorded->GetGPUSceneRasterResourceSnapshot();
    ASSERT_TRUE(rasterResources.IsValid());
    EXPECT_EQ(candidateBuffer, rasterResources.GetCandidates());
    EXPECT_EQ(lease.buffers[static_cast<uint32>(GPUSceneResidentTable::Primitives)].Get(),
              rasterResources.GetPrimitives());
    EXPECT_EQ(lease.buffers[static_cast<uint32>(GPUSceneResidentTable::Transforms)].Get(),
              rasterResources.GetTransforms());
    EXPECT_EQ(2u, rasterResources.GetCandidateCount());
    EXPECT_EQ(config.maxInstances, rasterResources.GetCandidateCapacity());
    EXPECT_EQ(capacities[static_cast<uint32>(GPUSceneResidentTable::Primitives)],
              rasterResources.GetPrimitiveCapacity());
    EXPECT_EQ(capacities[static_cast<uint32>(GPUSceneResidentTable::Transforms)],
              rasterResources.GetTransformCapacity());
    EXPECT_EQ(19u, rasterResources.GetLeaseVersion());

    GPUSceneResidentGraphLease staleLease = lease;
    staleLease.version = 18u;
    EXPECT_TRUE(staleLease.IsValid());
    EXPECT_EQ(nullptr, culling.SealForGPUSceneGraph(identity, staleLease));

    GPUSceneResidentGraphLease zeroCapacityLease = lease;
    zeroCapacityLease.capacities[5] = 0u;
    EXPECT_TRUE(zeroCapacityLease.IsValid());
    EXPECT_EQ(nullptr, culling.SealForGPUSceneGraph(identity, zeroCapacityLease));

    GPUSceneResidentGraphLease oversizedLease = lease;
    oversizedLease.capacities[0] += 1u;
    EXPECT_TRUE(oversizedLease.IsValid());
    EXPECT_EQ(nullptr, culling.SealForGPUSceneGraph(identity, oversizedLease));

    GPUSceneResidentGraphLease wrongStrideLease = lease;
    RHIBufferDesc wrongStrideDesc;
    wrongStrideDesc.size =
        static_cast<uint64>(capacities[0]) * sizeof(GPUScenePrimitiveRow);
    wrongStrideDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    wrongStrideDesc.memoryType = RHIMemoryType::Upload;
    wrongStrideDesc.stride = sizeof(uint32);
    wrongStrideDesc.debugName = "GPUDrivenValidation.GPUSceneWrongStride";
    wrongStrideLease.buffers[0] = device.CreateBuffer(wrongStrideDesc);
    EXPECT_TRUE(wrongStrideLease.IsValid());
    EXPECT_EQ(nullptr, culling.SealForGPUSceneGraph(identity, wrongStrideLease));

    GPUSceneResidentGraphLease invalidLease = lease;
    invalidLease.buffers[0].Reset();
    EXPECT_FALSE(invalidLease.IsValid());
    EXPECT_EQ(nullptr, culling.SealForGPUSceneGraph(identity, invalidLease));

    const std::shared_ptr<GPUCullingRecordedState> normalRecorded =
        culling.SealForGraph(identity);
    ASSERT_NE(nullptr, normalRecorded);
    EXPECT_FALSE(normalRecorded->GetGPUSceneRasterResourceSnapshot().IsValid());
    RenderSubmissionResourceBatch normalBatch;
    ASSERT_TRUE(normalRecorded->RetainSubmissionResources(normalBatch));
    const uint32 normalResourceCount = normalBatch.GetRetainedObjectCount();

    // Clearing the caller's refs before retention proves the sealed state
    // itself owns every table from the exact lease.
    lease.buffers = {};
    RenderSubmissionResourceBatch gpuSceneBatch;
    ASSERT_TRUE(recorded->RetainSubmissionResources(gpuSceneBatch));
    // Tier 2 intentionally excludes both the Tier 1 GPUInstanceData stream
    // and its normal descriptor binding.
    constexpr uint32 gpuSceneOnlyRetainedObjectCount = 14u;
    EXPECT_EQ(normalResourceCount + gpuSceneOnlyRetainedObjectCount,
              gpuSceneBatch.GetRetainedObjectCount());
    normalBatch.ReleaseUnsubmitted(retirement);
    gpuSceneBatch.ReleaseUnsubmitted(retirement);

    culling.InvalidateGPUSceneCandidates();
    EXPECT_FALSE(culling.HasCompleteGPUSceneCandidates());
    EXPECT_EQ(2u, culling.GetInstanceCount());
    EXPECT_TRUE(recorded->GetCulling().HasCompleteGPUSceneCandidates());
    EXPECT_TRUE(rasterResources.IsValid());
}

TEST_F(GPUDrivenValidationFixture,
       GPUSceneCullingDispatchesTwoComputePassesWithoutImplicitFallback)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);
    ASSERT_TRUE(culling.IsGPUSceneExecutionReady());

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    GPUSceneCullingCandidate candidate;
    candidate.primitiveSlot = 1u;
    candidate.primitiveGeneration = 7u;
    candidate.drawSlot = 1u;
    candidate.drawGeneration = 7u;
    candidate.objectIdLow = 42u;
    candidate.requiredPassMask = 2u;
    candidate.drawGroupIndex = 0u;
    candidate.drawGroupVisibleOffset = 0u;
    candidate.rasterInstanceIndex = 0u;
    candidate.materialParameterSlot = 3u;
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 29u));
    culling.EndFrame();

    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    const GPUCullingRecordingIdentity identity{401u, 13u, 101u, 0u, 8u};
    const std::shared_ptr<GPUCullingRecordedState> gpuSceneRecorded =
        culling.SealForGPUSceneGraph(
            identity, MakeGPUSceneLease(device, 29u, capacities));
    ASSERT_NE(nullptr, gpuSceneRecorded);

    const Mat4 view = TestView();
    const Mat4 projection = TestProjection();
    FakeCommandContext gpuSceneContext;
    EXPECT_TRUE(gpuSceneRecorded->CullGPUScene(
        gpuSceneContext, view, projection));
    ASSERT_EQ(3u, gpuSceneContext.dispatches.size());
    EXPECT_EQ((std::array<uint32, 3>{1u, 1u, 1u}),
              gpuSceneContext.dispatches[0]);
    EXPECT_EQ((std::array<uint32, 3>{1u, 1u, 1u}),
              gpuSceneContext.dispatches[1]);
    EXPECT_EQ((std::array<uint32, 3>{1u, 1u, 1u}),
              gpuSceneContext.dispatches[2]);
    EXPECT_EQ(7u, gpuSceneContext.bufferBarriers.size());
    ASSERT_EQ(3u, gpuSceneContext.pipelines.size());
    ASSERT_EQ(3u, gpuSceneContext.descriptorSets.size());
    EXPECT_NE(gpuSceneContext.pipelines[0], gpuSceneContext.pipelines[1]);
    EXPECT_NE(gpuSceneContext.pipelines[1], gpuSceneContext.pipelines[2]);
    EXPECT_EQ(gpuSceneContext.descriptorSets[0],
              gpuSceneContext.descriptorSets[1]);
    EXPECT_EQ(gpuSceneContext.descriptorSets[1],
              gpuSceneContext.descriptorSets[2]);
    EXPECT_TRUE(gpuSceneRecorded->GetCulling().WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(gpuSceneRecorded->GetCulling().WasCpuFallbackUsedLastCull());
    EXPECT_TRUE(gpuSceneRecorded->GetCulling().GetVisibleInstanceIndices().empty());

    const FakeBuffer* constantsBuffer = static_cast<const FakeBuffer*>(
        gpuSceneRecorded->GetCulling().GetCullingConstantsBuffer());
    ASSERT_NE(nullptr, constantsBuffer);
    const GPUCullingConstants constants =
        ReadBufferValue<GPUCullingConstants>(*constantsBuffer);
    const Mat4 expectedViewProjection = projection * view;
    for (uint32 column = 0; column < 4; ++column)
    {
        for (uint32 row = 0; row < 4; ++row)
        {
            EXPECT_FLOAT_EQ(expectedViewProjection[column][row],
                            constants.viewProj[column][row]);
        }
    }
    EXPECT_FLOAT_EQ(config.maxDrawDistance, constants.params.x);
    EXPECT_FLOAT_EQ(1.0f, constants.params.z);
    EXPECT_FLOAT_EQ(1.0f, constants.params.w);
    EXPECT_EQ(1u, constants.counts[0]);
    EXPECT_EQ(1u, constants.counts[1]);
    for (uint32 tableIndex = 0; tableIndex < 4; ++tableIndex)
    {
        EXPECT_EQ(capacities[tableIndex], constants.gpuSceneTableCounts0[tableIndex]);
    }
    EXPECT_EQ(capacities[4], constants.gpuSceneTableCounts1[0]);
    EXPECT_EQ(capacities[5], constants.gpuSceneTableCounts1[1]);

    auto* mutableConstantsBuffer = static_cast<FakeBuffer*>(
        gpuSceneRecorded->GetCulling().GetCullingConstantsBuffer());
    ASSERT_NE(nullptr, mutableConstantsBuffer);
    mutableConstantsBuffer->SetMapSucceeds(false);
    device.failTransientUploadMap = true;
    FakeCommandContext failedGPUSceneContext;
    EXPECT_FALSE(gpuSceneRecorded->CullGPUScene(
        failedGPUSceneContext, view, projection));
    EXPECT_TRUE(failedGPUSceneContext.dispatches.empty());
    EXPECT_FALSE(gpuSceneRecorded->GetCulling().WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(gpuSceneRecorded->GetCulling().WasCpuFallbackUsedLastCull());
    device.failTransientUploadMap = false;
    mutableConstantsBuffer->SetMapSucceeds(true);

    const std::shared_ptr<GPUCullingRecordedState> normalRecorded =
        culling.SealForGraph(identity);
    ASSERT_NE(nullptr, normalRecorded);
    FakeCommandContext rejectedGPUSceneContext;
    EXPECT_FALSE(normalRecorded->CullGPUScene(
        rejectedGPUSceneContext, view, projection));
    EXPECT_TRUE(rejectedGPUSceneContext.dispatches.empty());
    EXPECT_FALSE(normalRecorded->GetCulling().WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(normalRecorded->GetCulling().WasCpuFallbackUsedLastCull());

    // The caller-selected normal seal remains available as the per-pass
    // fallback; the failed GPU-scene recording never invoked it implicitly.
    FakeCommandContext normalContext;
    normalRecorded->Cull(normalContext, view, projection);
    EXPECT_EQ(3u, normalContext.dispatches.size());
    ASSERT_EQ(3u, normalContext.pipelines.size());
    ASSERT_EQ(3u, normalContext.descriptorSets.size());
    EXPECT_NE(gpuSceneContext.pipelines[0], normalContext.pipelines[0]);
    EXPECT_NE(gpuSceneContext.pipelines[1], normalContext.pipelines[1]);
    EXPECT_NE(gpuSceneContext.descriptorSets[0], normalContext.descriptorSets[0]);
    EXPECT_TRUE(normalRecorded->GetCulling().WasGpuExecutionUsedLastCull());
}

TEST_F(GPUDrivenValidationFixture,
       GPUSceneQualificationIsOptInAndRejectsForeignOrStaleCompletionEvidence)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));

    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config, 2u);
    ASSERT_TRUE(culling.SetFrameSlot(1u));
    ASSERT_TRUE(culling.IsGPUSceneExecutionReady());

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    GPUSceneCullingCandidate candidate{};
    candidate.primitiveSlot = 1u;
    candidate.primitiveGeneration = 7u;
    candidate.drawSlot = 1u;
    candidate.drawGeneration = 7u;
    candidate.objectIdLow = 42u;
    candidate.requiredPassMask = 2u;
    candidate.drawGroupIndex = 0u;
    candidate.drawGroupVisibleOffset = 0u;
    candidate.rasterInstanceIndex = 0u;
    candidate.materialParameterSlot = 3u;
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 83u));
    culling.EndDrawGroup();
    ASSERT_EQ(1u, culling.BeginDrawGroup(12u, 22u));
    ASSERT_EQ(1u, culling.AddInstance(
        MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 36)));
    candidate.primitiveSlot = 2u;
    candidate.drawSlot = 2u;
    candidate.objectIdLow = 43u;
    candidate.drawGroupIndex = 1u;
    candidate.drawGroupVisibleOffset = 1u;
    candidate.rasterInstanceIndex = 1u;
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 83u));
    culling.EndDrawGroup();
    culling.EndFrame();

    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    const GPUSceneResidentGraphLease lease =
        MakeGPUSceneLease(device, 83u, capacities);
    const GPUCullingRecordingIdentity identity{801u, 23u, 601u, 0u, 17u};

    const std::shared_ptr<GPUCullingRecordedState> normalRecording =
        culling.SealForGPUSceneGraph(identity, lease);
    ASSERT_NE(nullptr, normalRecording);
    FakeCommandContext normalContext;
    ASSERT_TRUE(normalRecording->CullGPUScene(
        normalContext, TestView(), TestProjection()));
    const uint32 normalCopyBufferCalls = normalContext.copyBufferCalls;
    EXPECT_EQ(1u, normalCopyBufferCalls);
    EXPECT_EQ(nullptr, device.FindBuffer(
        "GPUCulling.GPUSceneQualificationVisibilityReadback"));
    EXPECT_FALSE(culling.GetGPUSceneQualificationDiagnostics().requested);

    ASSERT_TRUE(culling.ArmGPUSceneQualificationCapture());
    const std::array<uint64, 2> qualificationIdentities{0x801u, 0x802u};
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::ConfigureInputCoverage(
        culling, qualificationIdentities, qualificationIdentities));
    culling.PublishGPUSceneQualificationRequiredLane(
        GPUDrivenTier::GPUResidentScene);
    const std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGPUSceneGraph(identity, lease);
    ASSERT_NE(nullptr, recorded);

    FakeCommandContext context;
    ASSERT_TRUE(recorded->CullGPUScene(context, TestView(), TestProjection()));
    EXPECT_EQ(normalCopyBufferCalls + 4u, context.copyBufferCalls);
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::WriteExpectedReadback(
        recorded->GetCulling()));

    GPUCompletionToken staleCompletion;
    const GPUCompletionPoint stalePoint = tracker.Submit(&context);
    ASSERT_TRUE(InsertGPUCompletionPoint(staleCompletion, stalePoint));
    GPUCompletionToken currentCompletion;
    const GPUCompletionPoint currentPoint = tracker.Submit(&context);
    ASSERT_TRUE(InsertGPUCompletionPoint(currentCompletion, currentPoint));

    GPUCullingRecordingIdentity foreignIdentity = identity;
    ++foreignIdentity.recordEpoch;
    EXPECT_FALSE(RVX::GPUCullingQualificationTestAccess::NotifyWithIdentity(
        culling,
        recorded->GetCulling(),
        foreignIdentity,
        currentCompletion,
        tracker));
    EXPECT_EQ(GPUSceneCullingQualificationMismatch::CompletionRejected,
              culling.GetGPUSceneQualificationDiagnostics().mismatch);

    EXPECT_FALSE(recorded->NotifyGPUSceneQualificationSubmission(
        culling, staleCompletion, tracker));
    EXPECT_EQ(GPUSceneCullingQualificationMismatch::CompletionRejected,
              culling.GetGPUSceneQualificationDiagnostics().mismatch);

    EXPECT_TRUE(recorded->NotifyGPUSceneQualificationSubmission(
        culling, currentCompletion, tracker));
    const GPUSceneCullingQualificationDiagnostics accepted =
        culling.GetGPUSceneQualificationDiagnostics();
    EXPECT_TRUE(accepted.requested);
    EXPECT_TRUE(accepted.readbackAllocated);
    EXPECT_TRUE(accepted.copyRecorded);
    EXPECT_TRUE(accepted.submissionAccepted);
    EXPECT_EQ(GPUSceneCullingQualificationMismatch::CompletionPending,
              accepted.mismatch);
    EXPECT_EQ(identity.frameSequence, accepted.frameSequence);
    EXPECT_EQ(identity.recordEpoch, accepted.recordEpoch);
    EXPECT_EQ(83u, accepted.gpuSceneLeaseVersion);
    EXPECT_EQ(1u, accepted.candidateVersion);
    EXPECT_EQ(1u, accepted.activeRowVersion);
    EXPECT_EQ(currentPoint.value, accepted.completionValue);

    // Rebuild a distinct next-frame topology before A completes. The pending
    // capture must remain self-contained: comparison is against A's frozen
    // command/visible ranges, never this owner's mutable B draw groups.
    ASSERT_TRUE(culling.SetFrameSlot(0u));
    culling.BeginFrame();
    ASSERT_NE(RVX_INVALID_INDEX, culling.BeginDrawGroup(102u, 202u));
    ASSERT_NE(RVX_INVALID_INDEX, culling.AddInstance(
        MakeInstance(Vec3(-1.0f, 0.0f, -5.0f), 1.0f, 36)));
    ASSERT_NE(RVX_INVALID_INDEX, culling.AddInstance(
        MakeInstance(Vec3(-2.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndDrawGroup();
    ASSERT_NE(RVX_INVALID_INDEX, culling.BeginDrawGroup(101u, 201u));
    ASSERT_NE(RVX_INVALID_INDEX, culling.AddInstance(
        MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndDrawGroup();
    culling.EndFrame();

    FakeBuffer* const visibilityReadback = device.FindBuffer(
        "GPUCulling.GPUSceneQualificationVisibilityReadback");
    ASSERT_NE(nullptr, visibilityReadback);
    const uint32 mapsBeforeCompletion = visibilityReadback->GetMapCallCount();
    // Completion is owner-polled: the active owner is now on B's slot 0 and
    // never needs A's source slot 1 to be reselected before it can observe A.
    EXPECT_FALSE(culling.PollGPUSceneQualificationCapture(tracker));
    EXPECT_EQ(mapsBeforeCompletion, visibilityReadback->GetMapCallCount());
    EXPECT_FALSE(culling.GetGPUSceneQualificationDiagnostics().completionObserved);

    FakeFence* const graphicsFence = device.GetFence(0);
    ASSERT_NE(nullptr, graphicsFence);
    graphicsFence->Complete(currentPoint.value);
    EXPECT_TRUE(culling.PollGPUSceneQualificationCapture(tracker));

    const GPUSceneCullingQualificationDiagnostics completed =
        culling.GetGPUSceneQualificationDiagnostics();
    EXPECT_TRUE(completed.completionObserved);
    EXPECT_TRUE(completed.compared);
    EXPECT_TRUE(completed.matched);
    EXPECT_EQ(GPUSceneCullingQualificationMismatch::None,
              completed.mismatch);
    EXPECT_EQ(2u, completed.expectedVisibleInstanceCount);
    EXPECT_EQ(2u, completed.observedVisibleInstanceCount);
    EXPECT_EQ(2u, completed.expectedSubmittedDrawCount);
    EXPECT_EQ(2u, completed.observedSubmittedDrawCount);
    EXPECT_EQ(2u, completed.activeRowCount);
    EXPECT_EQ(2u, completed.drawGroupCount);
    EXPECT_TRUE(completed.inputCoverageCompared);
    EXPECT_TRUE(completed.inputCoverageMatched);
    EXPECT_TRUE(completed.cullOutputsCompared);
    EXPECT_TRUE(completed.cullOutputsMatched);
    EXPECT_EQ(GPUDrivenTier::GPUResidentScene, completed.capturedTier);
}

TEST_F(GPUDrivenValidationFixture,
       GPUSceneQualificationClassifiesCompactedResidentRowMismatch)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));

    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    GPUSceneCullingCandidate candidate{};
    candidate.primitiveSlot = 1u;
    candidate.primitiveGeneration = 7u;
    candidate.drawSlot = 1u;
    candidate.drawGeneration = 7u;
    candidate.objectIdLow = 42u;
    candidate.requiredPassMask = 2u;
    candidate.drawGroupIndex = 0u;
    candidate.drawGroupVisibleOffset = 0u;
    candidate.rasterInstanceIndex = 0u;
    candidate.materialParameterSlot = 3u;
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 89u));
    culling.EndFrame();

    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    const GPUCullingRecordingIdentity identity{802u, 24u, 602u, 0u, 18u};
    ASSERT_TRUE(culling.ArmGPUSceneQualificationCapture());
    const std::array<uint64, 1> qualificationIdentities{0x802u};
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::ConfigureInputCoverage(
        culling, qualificationIdentities, qualificationIdentities));
    culling.PublishGPUSceneQualificationRequiredLane(
        GPUDrivenTier::GPUResidentScene);
    const std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGPUSceneGraph(
            identity, MakeGPUSceneLease(device, 89u, capacities));
    ASSERT_NE(nullptr, recorded);

    FakeCommandContext context;
    ASSERT_TRUE(recorded->CullGPUScene(context, TestView(), TestProjection()));
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::WriteExpectedReadback(
        recorded->GetCulling()));
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::CorruptFirstVisibleResidentRow(
        recorded->GetCulling()));

    GPUCompletionToken completion;
    const GPUCompletionPoint point = tracker.Submit(&context);
    ASSERT_TRUE(InsertGPUCompletionPoint(completion, point));
    ASSERT_TRUE(recorded->NotifyGPUSceneQualificationSubmission(
        culling, completion, tracker));
    FakeFence* const graphicsFence = device.GetFence(0);
    ASSERT_NE(nullptr, graphicsFence);
    graphicsFence->Complete(point.value);

    EXPECT_FALSE(culling.CompleteGPUSceneQualificationCapture(0u, tracker));
    const GPUSceneCullingQualificationDiagnostics diagnostics =
        culling.GetGPUSceneQualificationDiagnostics();
    EXPECT_TRUE(diagnostics.completionObserved);
    EXPECT_TRUE(diagnostics.compared);
    EXPECT_FALSE(diagnostics.matched);
    EXPECT_EQ(GPUSceneCullingQualificationMismatch::CompactedResidentRows,
              diagnostics.mismatch);
    EXPECT_EQ(RVX_INVALID_INDEX, diagnostics.firstMismatchActiveRow);
    EXPECT_EQ(0u, diagnostics.firstMismatchDrawGroup);
    EXPECT_EQ(0u, diagnostics.expectedValue);
    EXPECT_EQ(1u, diagnostics.observedValue);
}

TEST_F(GPUDrivenValidationFixture,
       GPUSceneQualificationClassifiesMissingDuplicateAndDriftedInputCoverage)
{
    const auto runCoverageMismatch = [](std::span<const uint64> expected,
                                        std::span<const uint64> observed)
    {
        FakeDevice device;
        device.EnableTimelineRetirement();
        device.EnableGPUScenePipelineObjects();
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));

        GPUCullingConfig config;
        config.maxInstances = 4;
        GPUCulling culling;
        culling.Initialize(&device, config);
        culling.BeginFrame();
        ASSERT_EQ(0u, culling.AddInstance(
            MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
        GPUSceneCullingCandidate candidate{};
        candidate.primitiveSlot = 1u;
        candidate.primitiveGeneration = 7u;
        candidate.drawSlot = 1u;
        candidate.drawGeneration = 7u;
        candidate.objectIdLow = 42u;
        candidate.requiredPassMask = 2u;
        candidate.drawGroupIndex = 0u;
        candidate.drawGroupVisibleOffset = 0u;
        candidate.rasterInstanceIndex = 0u;
        candidate.materialParameterSlot = 3u;
        ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 97u));
        culling.EndFrame();

        const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
            8u, 9u, 10u, 11u, 12u, 13u};
        const GPUCullingRecordingIdentity identity{803u, 25u, 603u, 0u, 19u};
        ASSERT_TRUE(culling.ArmGPUSceneQualificationCapture());
        ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::ConfigureInputCoverage(
            culling, expected, observed));
        culling.PublishGPUSceneQualificationRequiredLane(
            GPUDrivenTier::GPUResidentScene);
        const std::shared_ptr<GPUCullingRecordedState> recorded =
            culling.SealForGPUSceneGraph(
                identity, MakeGPUSceneLease(device, 97u, capacities));
        ASSERT_NE(nullptr, recorded);

        FakeCommandContext context;
        ASSERT_TRUE(recorded->CullGPUScene(
            context, TestView(), TestProjection()));
        ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::WriteExpectedReadback(
            recorded->GetCulling()));
        GPUCompletionToken completion;
        const GPUCompletionPoint point = tracker.Submit(&context);
        ASSERT_TRUE(InsertGPUCompletionPoint(completion, point));
        ASSERT_TRUE(recorded->NotifyGPUSceneQualificationSubmission(
            culling, completion, tracker));
        FakeFence* const graphicsFence = device.GetFence(0);
        ASSERT_NE(nullptr, graphicsFence);
        graphicsFence->Complete(point.value);

        EXPECT_FALSE(culling.CompleteGPUSceneQualificationCapture(0u, tracker));
        const GPUSceneCullingQualificationDiagnostics diagnostics =
            culling.GetGPUSceneQualificationDiagnostics();
        EXPECT_TRUE(diagnostics.completionObserved);
        EXPECT_TRUE(diagnostics.inputCoverageCompared);
        EXPECT_FALSE(diagnostics.inputCoverageMatched);
        EXPECT_TRUE(diagnostics.cullOutputsCompared);
        EXPECT_TRUE(diagnostics.cullOutputsMatched);
        EXPECT_TRUE(diagnostics.compared);
        EXPECT_FALSE(diagnostics.matched);
        EXPECT_EQ(GPUSceneCullingQualificationMismatch::InputCoverage,
                  diagnostics.mismatch);
    };

    const std::array<uint64, 1> expected{0x803u};
    const std::array<uint64, 0> missing{};
    const std::array<uint64, 2> duplicate{0x803u, 0x803u};
    const std::array<uint64, 1> drifted{0x804u};
    runCoverageMismatch(expected, missing);
    runCoverageMismatch(expected, duplicate);
    runCoverageMismatch(expected, drifted);
}

TEST_F(GPUDrivenValidationFixture,
       TierOneQualificationIsOptInAndCompletesAfterTheIndirectFinalizer)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));

    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();
    const GPUCullingRecordingIdentity identity{804u, 26u, 604u, 0u, 20u};

    const std::shared_ptr<GPUCullingRecordedState> ordinary =
        culling.SealForGraph(identity);
    ASSERT_NE(nullptr, ordinary);
    FakeCommandContext ordinaryContext;
    ordinary->Cull(ordinaryContext, TestView(), TestProjection());
    EXPECT_EQ(1u, ordinaryContext.copyBufferCalls);
    EXPECT_EQ(nullptr, device.FindBuffer(
        "GPUCulling.GPUSceneQualificationVisibilityReadback"));

    ASSERT_TRUE(culling.ArmGPUSceneQualificationCapture());
    const std::array<uint64, 1> qualificationIdentities{0x804u};
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::
        SetCollectedRasterSemanticIdentities(culling, qualificationIdentities));
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::ConfigureInputCoverage(
        culling, qualificationIdentities, qualificationIdentities));
    culling.PublishGPUSceneQualificationRequiredLane(
        GPUDrivenTier::IndirectGrouped);
    const std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGraph(identity);
    ASSERT_NE(nullptr, recorded);

    FakeCommandContext context;
    recorded->Cull(context, TestView(), TestProjection());
    EXPECT_EQ(ordinaryContext.copyBufferCalls + 5u, context.copyBufferCalls);
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::WriteExpectedReadback(
        recorded->GetCulling()));
    GPUCompletionToken completion;
    const GPUCompletionPoint point = tracker.Submit(&context);
    ASSERT_TRUE(InsertGPUCompletionPoint(completion, point));
    ASSERT_TRUE(recorded->NotifyGPUSceneQualificationSubmission(
        culling, completion, tracker));
    FakeFence* const graphicsFence = device.GetFence(0);
    ASSERT_NE(nullptr, graphicsFence);
    graphicsFence->Complete(point.value);

    EXPECT_TRUE(culling.CompleteGPUSceneQualificationCapture(0u, tracker));
    const GPUSceneCullingQualificationDiagnostics diagnostics =
        culling.GetGPUSceneQualificationDiagnostics();
    EXPECT_TRUE(diagnostics.required);
    EXPECT_EQ(GPUDrivenTier::IndirectGrouped, diagnostics.capturedTier);
    EXPECT_TRUE(diagnostics.inputCoverageCompared);
    EXPECT_TRUE(diagnostics.inputCoverageMatched);
    EXPECT_TRUE(diagnostics.cullOutputsCompared);
    EXPECT_TRUE(diagnostics.cullOutputsMatched);
    EXPECT_TRUE(diagnostics.tierOneRasterTranscriptCompared);
    EXPECT_TRUE(diagnostics.tierOneRasterTranscriptMatched);
    EXPECT_TRUE(diagnostics.tierOneRasterTranscript.available);
    EXPECT_TRUE(diagnostics.tierOneRasterTranscriptReference.available);
    EXPECT_EQ(1u, diagnostics.tierOneRasterTranscript.entryCount);
    EXPECT_TRUE(diagnostics.matched);
    EXPECT_GT(diagnostics.cpuPayloadBytes, 0u);
}

TEST_F(GPUDrivenValidationFixture,
       TierOneQualificationComparesRasterPayloadIndirectArgumentsAndDirectCoverage)
{
    const std::array<uint64, 1> identities{0x805u};
    const auto runCapture = [&identities](
                                std::span<const uint64> directVisible,
                                const std::function<void(GPUCulling&)>& corrupt)
        -> GPUSceneCullingQualificationDiagnostics
    {
        FakeDevice device;
        device.EnableTimelineRetirement();
        device.EnableGPUScenePipelineObjects();
        RenderSubmissionTracker tracker;
        EXPECT_TRUE(tracker.Initialize(&device));

        GPUCullingConfig config;
        config.maxInstances = 4;
        GPUCulling culling;
        culling.Initialize(&device, config);
        culling.BeginFrame();
        EXPECT_EQ(0u, culling.AddInstance(
            MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
        culling.EndFrame();
        EXPECT_TRUE(RVX::GPUCullingQualificationTestAccess::
            SetCollectedRasterSemanticIdentities(culling, identities));
        EXPECT_TRUE(culling.ArmGPUSceneQualificationCapture());
        EXPECT_TRUE(RVX::GPUCullingQualificationTestAccess::ConfigureInputCoverage(
            culling, identities, identities, 0u, 0u, directVisible));
        culling.PublishGPUSceneQualificationRequiredLane(
            GPUDrivenTier::IndirectGrouped);
        const GPUCullingRecordingIdentity identity{805u, 27u, 605u, 0u, 21u};
        const std::shared_ptr<GPUCullingRecordedState> recorded =
            culling.SealForGraph(identity);
        EXPECT_NE(nullptr, recorded);
        if (!recorded)
        {
            return culling.GetGPUSceneQualificationDiagnostics();
        }
        FakeCommandContext context;
        recorded->Cull(context, TestView(), TestProjection());
        EXPECT_TRUE(RVX::GPUCullingQualificationTestAccess::WriteExpectedReadback(
            recorded->GetCulling()));
        corrupt(recorded->GetCulling());
        GPUCompletionToken completion;
        const GPUCompletionPoint point = tracker.Submit(&context);
        EXPECT_TRUE(InsertGPUCompletionPoint(completion, point));
        EXPECT_TRUE(recorded->NotifyGPUSceneQualificationSubmission(
            culling, completion, tracker));
        FakeFence* const fence = device.GetFence(0);
        EXPECT_NE(nullptr, fence);
        if (fence)
        {
            fence->Complete(point.value);
        }
        static_cast<void>(culling.PollGPUSceneQualificationCapture(tracker));
        return culling.GetGPUSceneQualificationDiagnostics();
    };

    const GPUSceneCullingQualificationDiagnostics payloadMismatch = runCapture(
        {}, [](GPUCulling& culling)
        {
            EXPECT_TRUE(RVX::GPUCullingQualificationTestAccess::CorruptFirstRasterPayload(
                culling));
        });
    EXPECT_TRUE(payloadMismatch.rasterPayloadCompared);
    EXPECT_FALSE(payloadMismatch.rasterPayloadMatched);
    EXPECT_EQ(GPUSceneCullingQualificationMismatch::RasterPayload,
              payloadMismatch.mismatch);
    EXPECT_NE(RVX_INVALID_INDEX, payloadMismatch.firstMismatchResidentRow);
    EXPECT_TRUE(payloadMismatch.tierOneRasterTranscriptCompared);
    EXPECT_FALSE(payloadMismatch.tierOneRasterTranscriptMatched);
    EXPECT_NE(RVX_INVALID_INDEX,
              payloadMismatch.firstRasterTranscriptMismatchEntry);
    EXPECT_NE(payloadMismatch.expectedRasterTranscriptPayloadHash,
              payloadMismatch.observedRasterTranscriptPayloadHash);

    for (uint32 fieldIndex = 0u; fieldIndex < 5u; ++fieldIndex)
    {
        const GPUSceneCullingQualificationDiagnostics indirectMismatch =
            runCapture({}, [fieldIndex](GPUCulling& culling)
            {
                EXPECT_TRUE(
                    RVX::GPUCullingQualificationTestAccess::CorruptFirstIndirectArgument(
                        culling, fieldIndex));
            });
        EXPECT_TRUE(indirectMismatch.indirectArgumentsCompared);
        EXPECT_FALSE(indirectMismatch.indirectArgumentsMatched);
        EXPECT_TRUE(indirectMismatch.tierOneRasterTranscriptCompared);
        EXPECT_FALSE(indirectMismatch.tierOneRasterTranscriptMatched);
        EXPECT_EQ(GPUSceneCullingQualificationMismatch::IndirectArguments,
                  indirectMismatch.mismatch);
    }

    const GPUSceneCullingQualificationDiagnostics directMissing = runCapture(
        identities, [](GPUCulling& culling)
        {
            EXPECT_TRUE(RVX::GPUCullingQualificationTestAccess::SetFirstVisibility(
                culling, 0u));
        });
    EXPECT_TRUE(directMissing.directVisibilityCoverageCompared);
    EXPECT_FALSE(directMissing.directVisibilityCoverageMatched);
    EXPECT_EQ(1u, directMissing.missingDirectVisiblePacketCount);
    EXPECT_EQ(GPUSceneCullingQualificationMismatch::DirectVisibilityCoverage,
              directMissing.mismatch);

    const GPUSceneCullingQualificationDiagnostics conservativeExtra = runCapture(
        {}, [](GPUCulling&) {});
    EXPECT_TRUE(conservativeExtra.directVisibilityCoverageCompared);
    EXPECT_TRUE(conservativeExtra.directVisibilityCoverageMatched);
    EXPECT_EQ(0u, conservativeExtra.missingDirectVisiblePacketCount);
    EXPECT_EQ(1u, conservativeExtra.gpuOnlyVisiblePacketCount);
    EXPECT_TRUE(conservativeExtra.tierOneRasterTranscriptCompared);
    EXPECT_TRUE(conservativeExtra.tierOneRasterTranscriptMatched);
    EXPECT_TRUE(conservativeExtra.matched);
}

TEST_F(GPUDrivenValidationFixture,
       TierOneQualificationRejectsStaleCanonicalResidentPayloadAgainstFreshActivePacket)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));

    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    // Establish the stable resident row and upload its old canonical bytes.
    culling.BeginFrame();
    GPUInstanceData staleInstance =
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36u);
    staleInstance.worldMatrix[3] = Vec4(0.0f, 0.0f, -5.0f, 1.0f);
    ASSERT_EQ(0u, culling.AddInstance(staleInstance));
    culling.EndFrame();
    ASSERT_NE(nullptr, culling.SealForGraph(
        GPUCullingRecordingIdentity{806u, 28u, 606u, 0u, 22u}));

    // This frame's direct/active packet is fresh. Deliberately make the
    // sparse resident side table stale before its upload so the GPU readback
    // equals canonical-old bytes. The qualification expected value must be
    // derived from the active packet, not that upload side table.
    culling.BeginFrame();
    GPUInstanceData freshInstance = staleInstance;
    freshInstance.worldMatrix[3] = Vec4(5.0f, 0.0f, -5.0f, 1.0f);
    ASSERT_EQ(0u, culling.AddInstance(freshInstance));
    culling.EndFrame();
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::
        OverwriteFirstCanonicalInstance(culling, staleInstance));

    const std::array<uint64, 1> identities{0x806u};
    ASSERT_TRUE(culling.ArmGPUSceneQualificationCapture());
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::ConfigureInputCoverage(
        culling, identities, identities));
    culling.PublishGPUSceneQualificationRequiredLane(
        GPUDrivenTier::IndirectGrouped);
    const GPUCullingRecordingIdentity identity{806u, 29u, 607u, 0u, 23u};
    const std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGraph(identity);
    ASSERT_NE(nullptr, recorded);

    EXPECT_FALSE(RVX::GPUCullingQualificationTestAccess::
        ExpectedRasterPayloadMatchesResidentBuffer(recorded->GetCulling()));

    FakeCommandContext context;
    recorded->Cull(context, TestView(), TestProjection());
    // Cull has copied the actual resident GPU instance buffer to its
    // readback. Fill only the simulated cull outputs, preserving the stale
    // resident payload exactly as raster would consume it.
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::WriteExpectedReadback(
        recorded->GetCulling(), false));
    GPUCompletionToken completion;
    const GPUCompletionPoint point = tracker.Submit(&context);
    ASSERT_TRUE(InsertGPUCompletionPoint(completion, point));
    ASSERT_TRUE(recorded->NotifyGPUSceneQualificationSubmission(
        culling, completion, tracker));
    FakeFence* const fence = device.GetFence(0);
    ASSERT_NE(nullptr, fence);
    fence->Complete(point.value);

    EXPECT_FALSE(culling.PollGPUSceneQualificationCapture(tracker));
    const GPUSceneCullingQualificationDiagnostics diagnostics =
        culling.GetGPUSceneQualificationDiagnostics();
    EXPECT_TRUE(diagnostics.rasterPayloadCompared);
    EXPECT_FALSE(diagnostics.rasterPayloadMatched);
    EXPECT_EQ(GPUSceneCullingQualificationMismatch::RasterPayload,
              diagnostics.mismatch);
    EXPECT_EQ(0u, diagnostics.firstMismatchResidentRow);
}

TEST_F(GPUDrivenValidationFixture,
       DirectCoverageBindsFrozenActiveRowIdentitiesToVisibilityIndices)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));

    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36u)));
    ASSERT_EQ(1u, culling.AddInstance(
        MakeInstance(Vec3(1000.0f, 0.0f, -5.0f), 1.0f, 36u)));
    culling.EndFrame();

    const std::array<uint64, 2> identities{0x8071u, 0x8072u};
    const std::array<uint64, 1> directVisible{identities.front()};
    ASSERT_TRUE(culling.ArmGPUSceneQualificationCapture());
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::ConfigureInputCoverage(
        culling, identities, identities, 0u, 0u, directVisible));
    culling.PublishGPUSceneQualificationRequiredLane(
        GPUDrivenTier::IndirectGrouped);
    const GPUCullingRecordingIdentity identity{807u, 30u, 608u, 0u, 24u};
    const std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGraph(identity);
    ASSERT_NE(nullptr, recorded);

    // Keep unordered owner-input accounting valid while swapping the frozen
    // active-row identities. With only row zero visible, Direct coverage must
    // reject the positional mismatch rather than treating the identity set as
    // sufficient evidence.
    const std::array<uint64, 2> swapped{identities[1], identities[0]};
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::ReplaceActiveRowIdentities(
        recorded->GetCulling(), swapped));
    FakeCommandContext context;
    recorded->Cull(context, TestView(), TestProjection());
    ASSERT_TRUE(RVX::GPUCullingQualificationTestAccess::WriteExpectedReadback(
        recorded->GetCulling()));
    GPUCompletionToken completion;
    const GPUCompletionPoint point = tracker.Submit(&context);
    ASSERT_TRUE(InsertGPUCompletionPoint(completion, point));
    ASSERT_TRUE(recorded->NotifyGPUSceneQualificationSubmission(
        culling, completion, tracker));
    FakeFence* const fence = device.GetFence(0);
    ASSERT_NE(nullptr, fence);
    fence->Complete(point.value);

    EXPECT_FALSE(culling.PollGPUSceneQualificationCapture(tracker));
    const GPUSceneCullingQualificationDiagnostics diagnostics =
        culling.GetGPUSceneQualificationDiagnostics();
    EXPECT_TRUE(diagnostics.inputCoverageCompared);
    EXPECT_TRUE(diagnostics.inputCoverageMatched);
    EXPECT_TRUE(diagnostics.directVisibilityCoverageCompared);
    EXPECT_FALSE(diagnostics.directVisibilityCoverageMatched);
    EXPECT_EQ(1u, diagnostics.missingDirectVisiblePacketCount);
    EXPECT_EQ(GPUSceneCullingQualificationMismatch::DirectVisibilityCoverage,
              diagnostics.mismatch);
}

TEST_F(GPUDrivenValidationFixture,
       QualificationEvidenceFailureDoesNotAlterTierOneOwnerExecution)
{
    FakeDevice device;
    device.EnableGPUScenePipelineObjects();

    GPUCullingConfig config;
    config.maxInstances = 4;
    const GPUCullingRecordingIdentity identity{805u, 27u, 605u, 0u, 21u};
    const auto collectOneTierOneInstance = [&config, &device]()
    {
        auto owner = std::make_unique<GPUCulling>();
        owner->Initialize(&device, config);
        owner->BeginFrame();
        EXPECT_EQ(0u, owner->AddInstance(
            MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
        owner->EndFrame();
        return owner;
    };

    std::unique_ptr<GPUCulling> baseline = collectOneTierOneInstance();
    ASSERT_NE(nullptr, baseline);
    const std::shared_ptr<GPUCullingRecordedState> baselineRecorded =
        baseline->SealForGraph(identity);
    ASSERT_NE(nullptr, baselineRecorded);
    FakeCommandContext baselineContext;
    baselineRecorded->Cull(baselineContext, TestView(), TestProjection());

    std::unique_ptr<GPUCulling> qualified = std::make_unique<GPUCulling>();
    qualified->Initialize(&device, config);
    ASSERT_TRUE(qualified->ArmGPUSceneQualificationCapture());
    GPUCullingQualificationInputPlan invalidPlan;
    invalidPlan.expectedPacketCount = 1u;
    invalidPlan.expectedGPUInputPacketCount = 1u;
    invalidPlan.planPacketIdentityHash = 0x805u;
    invalidPlan.expectedGPUInputIdentityHashes.push_back(0x805u);
    // This deliberately fails exactly-once evidence before collection.  It
    // must not change the owner stream that is subsequently collected.
    invalidPlan.exactlyOncePartitioned = false;
    EXPECT_FALSE(qualified->BeginGPUSceneQualificationInputCoverage(invalidPlan));
    EXPECT_FALSE(qualified->ObserveGPUSceneQualificationInputIdentity(0x805u));
    qualified->PublishGPUSceneQualificationRequiredLane(
        GPUDrivenTier::IndirectGrouped);
    qualified->BeginFrame();
    EXPECT_EQ(0u, qualified->AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    qualified->EndFrame();

    const std::shared_ptr<GPUCullingRecordedState> qualifiedRecorded =
        qualified->SealForGraph(identity);
    ASSERT_NE(nullptr, qualifiedRecorded);
    FakeCommandContext qualifiedContext;
    qualifiedRecorded->Cull(qualifiedContext, TestView(), TestProjection());

    EXPECT_EQ(baselineRecorded->GetCulling().GetInstanceCount(),
              qualifiedRecorded->GetCulling().GetInstanceCount());
    EXPECT_EQ(baselineRecorded->GetCulling().GetDrawGroups().size(),
              qualifiedRecorded->GetCulling().GetDrawGroups().size());
    EXPECT_EQ(baselineRecorded->GetCulling().GetDrawCount(),
              qualifiedRecorded->GetCulling().GetDrawCount());
    EXPECT_EQ(baselineContext.copyBufferCalls, qualifiedContext.copyBufferCalls);

    const GPUSceneCullingQualificationDiagnostics diagnostics =
        qualified->GetGPUSceneQualificationDiagnostics();
    EXPECT_TRUE(diagnostics.requested);
    EXPECT_TRUE(diagnostics.required);
    EXPECT_FALSE(diagnostics.readbackAllocated);
    EXPECT_FALSE(diagnostics.compared);
    EXPECT_FALSE(diagnostics.matched);
    EXPECT_EQ(GPUSceneCullingQualificationMismatch::InputCoverage,
              diagnostics.mismatch);
}

TEST_F(GPUDrivenValidationFixture,
       SceneRendererGPUSceneLeaseWiringUsesOneAcquireAndPreflightsBeforeGraphMutation)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());
    const std::string source = ReadTextFile(
        root / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
    const std::string uploaderHeader = ReadTextFile(
        root / "Render" / "Private" / "GPUScene" / "GPUSceneUploader.h");
    const std::string subsystemSource = ReadTextFile(
        root / "Render" / "Private" / "RenderSubsystem.cpp");
    const std::string sampleRunnerSource = ReadTextFile(
        root / "Samples" / "Common" / "Private" / "SampleRunner.cpp");
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(uploaderHeader.empty());
    ASSERT_FALSE(subsystemSource.empty());
    ASSERT_FALSE(sampleRunnerSource.empty());

    size_t acquireCount = 0;
    size_t acquireOffset = source.find("AcquireCurrentGraphLease(");
    while (acquireOffset != std::string::npos)
    {
        ++acquireCount;
        acquireOffset = source.find("AcquireCurrentGraphLease(", acquireOffset + 1);
    }
    EXPECT_EQ(1u, acquireCount);
    EXPECT_NE(source.find("m_gpuSceneUploader->BuildRenderGraph("),
              std::string::npos);
    EXPECT_NE(source.find("requestedGPUSceneTier"), std::string::npos);
    EXPECT_NE(source.find("useGPUSceneTier"), std::string::npos);
    EXPECT_NE(source.find("preflightGPUScenePass"), std::string::npos);
    EXPECT_NE(source.find("owner->SealForGPUSceneGraph("), std::string::npos);
    EXPECT_NE(source.find("HasCompleteGPUSceneCandidates(requiredResidentVersion)"),
              std::string::npos);
    EXPECT_NE(source.find("owner->SealForGraph(cullingIdentity)"),
              std::string::npos);
    EXPECT_NE(source.find("PollGPUSceneCullingQualificationCompletion"),
              std::string::npos);
    EXPECT_NE(source.find("HasPendingGPUSceneCullingQualificationCompletion"),
              std::string::npos);
    EXPECT_NE(subsystemSource.find(
                  "m_sceneRenderer->PollGPUSceneCullingQualificationCompletion"),
              std::string::npos);
    EXPECT_NE(subsystemSource.find(
                  "frame.gpuDrivenCullingStats.gpuSceneDepthQualification"),
              std::string::npos);

    // The final evidence request is issued immediately before its carrying
    // Tick. A wait may issue an Engine tick only until that target publishes;
    // RuntimeFrameDriver then switches to Render-only progress rather than
    // publishing a replacement. This covers ordinary screenshots and the
    // no-screenshot final-drain path without invoking a graphics backend.
    const size_t qualificationArm = sampleRunnerSource.find(
        "render->RequestGPUSceneCullingQualificationCapture()", 0u);
    const size_t carryingTick = sampleRunnerSource.find(
        "diagnostics = frameDriver.TickOnce(SampleDeltaTime);", qualificationArm);
    ASSERT_NE(std::string::npos, qualificationArm);
    ASSERT_NE(std::string::npos, carryingTick);
    EXPECT_LT(qualificationArm, carryingTick);
    EXPECT_NE(sampleRunnerSource.find("request.advanceEngine = true"),
              std::string::npos);

    // Qualification input accounting is diagnostic-only.  Keep an explicit
    // source-contract guard against returning from pass collection or marking
    // the renderer frame failed when its evidence record is malformed.
    const size_t coverageBegin = source.find(
        "if (owner->IsGPUSceneQualificationCaptureArmed())");
    const size_t coverageEnd = source.find(
        "// Complete structural preflight occurs", coverageBegin);
    ASSERT_NE(std::string::npos, coverageBegin);
    ASSERT_NE(std::string::npos, coverageEnd);
    const std::string coverageBlock = source.substr(
        coverageBegin, coverageEnd - coverageBegin);
    EXPECT_EQ(std::string::npos,
              coverageBlock.find("MarkGPUDrivenFrameFailure("));
    EXPECT_EQ(std::string::npos, coverageBlock.find("return;"));

    const size_t observeBegin = source.find(
        "if (owner->IsGPUSceneQualificationCaptureArmed() &&\n"
        "                    ownerCandidateMapped)", coverageEnd);
    const size_t observeEnd = source.find("++plannedOffset;", observeBegin);
    ASSERT_NE(std::string::npos, observeBegin);
    ASSERT_NE(std::string::npos, observeEnd);
    const std::string observeBlock = source.substr(
        observeBegin, observeEnd - observeBegin);
    EXPECT_EQ(std::string::npos,
              observeBlock.find("MarkGPUDrivenFrameFailure("));
    EXPECT_EQ(std::string::npos, observeBlock.find("return;"));
    EXPECT_NE(source.find("m_gpuSceneUploader->CancelCurrentGraphLease()"),
              std::string::npos);
    EXPECT_NE(source.find("data.gpuSceneCandidates = builder.Read("),
              std::string::npos);
    EXPECT_NE(source.find("data.gpuSceneTables[tableIndex] = builder.Read("),
              std::string::npos);
    EXPECT_NE(source.find("recordedState->CullGPUScene("), std::string::npos);
    EXPECT_NE(source.find("if (!recordedState->CullGPUScene("),
              std::string::npos);
    EXPECT_NE(source.find("m_gpuSceneCullingCommandRecordingFailed"),
              std::string::npos);
    EXPECT_NE(source.find("GPU-scene culling command recording failed"),
              std::string::npos);
    EXPECT_NE(source.find("m_gpuSceneUpdate->ResolveAcceptedDraw("),
              std::string::npos);
    EXPECT_NE(source.find("owner->InvalidateGPUSceneCandidates()"),
              std::string::npos);
    EXPECT_NE(uploaderHeader.find("CancelCurrentGraphLease"),
              std::string::npos);

    const size_t renderStart = source.find("void SceneRenderer::Render()");
    const size_t graphPrepare = source.find(
        "graphExecutor.Prepare(compiledPlan, executionEnvironment);", renderStart);
    const size_t graphAdopt = source.find(
        "m_renderContext->AdoptRenderGraphExecution(", graphPrepare);
    const size_t failedFrameGate = source.find(
        "graphExecuted = !m_gpuSceneCullingCommandRecordingFailed &&", graphPrepare);
    const size_t uploaderCommit = source.find(
        "m_gpuSceneUploader->CommitRealizedAccess(*m_renderGraph);", graphPrepare);
    const size_t cullingCommit = source.find(
        "CommitGPUDrivenAccessSnapshots();", graphPrepare);
    ASSERT_NE(std::string::npos, renderStart);
    ASSERT_NE(std::string::npos, graphPrepare);
    ASSERT_NE(std::string::npos, graphAdopt);
    ASSERT_NE(std::string::npos, failedFrameGate);
    ASSERT_NE(std::string::npos, uploaderCommit);
    ASSERT_NE(std::string::npos, cullingCommit);
    EXPECT_LT(graphPrepare, graphAdopt);
    EXPECT_LT(graphAdopt, failedFrameGate);
    EXPECT_LT(failedFrameGate, uploaderCommit);
    EXPECT_LT(failedFrameGate, cullingCommit);
    const size_t commitSuccessGuard = source.rfind(
        "if (graphExecuted)", uploaderCommit);
    ASSERT_NE(std::string::npos, commitSuccessGuard);
    EXPECT_LT(failedFrameGate, commitSuccessGuard);
    EXPECT_LT(commitSuccessGuard, uploaderCommit);

    const size_t acceptedRendererFrame = source.find(
        "RenderFrameExecutionResult SceneRenderer::RenderAcceptedFrame()");
    const size_t rendererRenderCall = source.find("Render();", acceptedRendererFrame);
    const size_t rendererFailureReturn = source.find(
        "if (HasSubmissionFailure())", rendererRenderCall);
    const size_t cullingRetain = source.find(
        "m_depthGPUCulling->RetainSubmissionResources", rendererRenderCall);
    ASSERT_NE(std::string::npos, acceptedRendererFrame);
    ASSERT_NE(std::string::npos, rendererRenderCall);
    ASSERT_NE(std::string::npos, rendererFailureReturn);
    ASSERT_NE(std::string::npos, cullingRetain);
    EXPECT_LT(rendererRenderCall, rendererFailureReturn);
    EXPECT_LT(rendererFailureReturn, cullingRetain);

    const size_t acceptedFrame = subsystemSource.find("m_sceneRenderer->RenderAcceptedFrame()");
    const size_t releaseUnsubmitted = subsystemSource.find(
        "m_sceneRenderer->ReleaseUnsubmittedFrame();", acceptedFrame);
    const size_t abortFrame = subsystemSource.find("m_context->AbortFrame();", acceptedFrame);
    const size_t submitFrame = subsystemSource.find("m_context->EndFrame();", acceptedFrame);
    ASSERT_NE(std::string::npos, acceptedFrame);
    ASSERT_NE(std::string::npos, releaseUnsubmitted);
    ASSERT_NE(std::string::npos, abortFrame);
    ASSERT_NE(std::string::npos, submitFrame);
    EXPECT_LT(acceptedFrame, releaseUnsubmitted);
    EXPECT_LT(abortFrame, releaseUnsubmitted);
    EXPECT_LT(releaseUnsubmitted, submitFrame);
}

TEST_F(GPUDrivenValidationFixture,
       GPUSceneRasterPassesUseSealedBindingsAndSupportedDepthInputs)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());
    const std::string renderer = ReadTextFile(
        root / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
    const std::string recordContext = ReadTextFile(
        root / "Render" / "Include" / "Render" / "Passes" /
        "RenderPassRecordContext.h");
    const std::string depthPass = ReadTextFile(
        root / "Render" / "Private" / "Passes" / "DepthPrepass.cpp");
    const std::string opaquePass = ReadTextFile(
        root / "Render" / "Private" / "Passes" / "OpaquePass.cpp");
    ASSERT_FALSE(renderer.empty());
    ASSERT_FALSE(recordContext.empty());
    ASSERT_FALSE(depthPass.empty());
    ASSERT_FALSE(opaquePass.empty());

    const size_t gpuSceneSeal = renderer.find("owner->SealForGPUSceneGraph(");
    const size_t bindingCreate = renderer.find(
        "CreateGPUSceneRasterBindingSnapshot(", gpuSceneSeal);
    const size_t graphImport = renderer.find("m_renderGraph->ImportBuffer(", gpuSceneSeal);
    ASSERT_NE(std::string::npos, gpuSceneSeal);
    ASSERT_NE(std::string::npos, bindingCreate);
    ASSERT_NE(std::string::npos, graphImport);
    EXPECT_LT(gpuSceneSeal, bindingCreate);
    EXPECT_LT(bindingCreate, graphImport);
    EXPECT_NE(std::string::npos,
              renderer.find("retainedBinding->RetainSubmissionResources"));
    EXPECT_NE(std::string::npos,
              renderer.find("m_gpuSceneRasterCommandRecordingFailed"));

    EXPECT_NE(std::string::npos,
              recordContext.find("gpuSceneCandidates"));
    EXPECT_NE(std::string::npos,
              recordContext.find("gpuScenePrimitives"));
    EXPECT_NE(std::string::npos,
              recordContext.find("gpuSceneTransforms"));
    EXPECT_NE(std::string::npos,
              recordContext.find("gpuSceneRasterBinding"));
    EXPECT_NE(std::string::npos,
              recordContext.find("gpuSceneLeaseVersion"));

    for (const std::string* pass : {&depthPass, &opaquePass})
    {
        EXPECT_NE(std::string::npos,
                  pass->find("builder.Read(m_gpuSceneCandidateHandle, RHIShaderStage::Vertex)"));
        EXPECT_NE(std::string::npos,
                  pass->find("builder.Read(m_gpuScenePrimitiveHandle, RHIShaderStage::Vertex)"));
        EXPECT_NE(std::string::npos,
                  pass->find("builder.Read(m_gpuSceneTransformHandle, RHIShaderStage::Vertex)"));
        EXPECT_NE(std::string::npos,
                  pass->find("gpuSceneObjectOffsets{0u}"));
    }

    EXPECT_NE(std::string::npos,
              depthPass.find("group.pipelineVariant != MaterialPipelineVariant::Opaque"));
}

TEST_F(GPUDrivenValidationFixture,
       GPUSceneCullingShaderUsesSharedSixTableAbiAndExactGenerationValidation)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::string header = ReadTextFile(
        root / "Render" / "Include" / "Render" / "GPUDriven" / "GPUCulling.h");
    const std::string source = ReadTextFile(
        root / "Render" / "Private" / "GPUDriven" / "GPUCulling.cpp");
    const std::string sharedShader = ReadTextFile(
        root / "Render" / "Shaders" / "GPUDriven" / "GPUSceneCulling.hlsli");
    const std::string shader = ReadTextFile(
        root / "Render" / "Shaders" / "GPUDriven" / "GPUSceneCulling.hlsl");
    const std::string normalShader = ReadTextFile(
        root / "Render" / "Shaders" / "GPUDriven" / "GPUCulling.hlsl");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(sharedShader.empty());
    ASSERT_FALSE(shader.empty());
    ASSERT_FALSE(normalShader.empty());

    EXPECT_NE(header.find("sizeof(GPUSceneCullingCandidate) == 48"),
              std::string::npos);
    EXPECT_NE(header.find("RVX_GPU_SCENE_CULLING_TABLE_COUNT = 6"),
              std::string::npos);
    EXPECT_NE(header.find("SealForGPUSceneGraph"), std::string::npos);
    EXPECT_NE(source.find("ConfigureGPUSceneRecording"), std::string::npos);
    EXPECT_NE(source.find("FindGPUSceneCullingShaderPath"), std::string::npos);
    EXPECT_NE(source.find("GPUCulling.GPUSceneFrameSlotDescriptorSet"),
              std::string::npos);
    EXPECT_NE(source.find("inputs->gpuSceneTableBuffers = lease.buffers"),
              std::string::npos);
    EXPECT_NE(source.find("inputs->gpuSceneLeaseVersion = lease.version"),
              std::string::npos);

    EXPECT_NE(sharedShader.find("struct GPUScenePrimitiveRow"), std::string::npos);
    EXPECT_NE(sharedShader.find("struct GPUSceneBoundsRow"), std::string::npos);
    EXPECT_NE(sharedShader.find("struct GPUSceneTransformRow"), std::string::npos);
    EXPECT_NE(sharedShader.find("struct GPUSceneMaterialRow"), std::string::npos);
    EXPECT_NE(sharedShader.find("struct GPUSceneGeometryRow"), std::string::npos);
    EXPECT_NE(sharedShader.find("struct GPUSceneDrawMetadataRow"), std::string::npos);
    EXPECT_NE(sharedShader.find("GPUSceneValidateCandidateRows"), std::string::npos);
    EXPECT_NE(sharedShader.find("drawRef.y != primitive.firstDraw.y"),
              std::string::npos);
    EXPECT_NE(sharedShader.find("draw.instanceCount != 1u"), std::string::npos);
    EXPECT_NE(sharedShader.find("draw.firstInstance != 0u"), std::string::npos);
    EXPECT_NE(sharedShader.find("candidate.requiredPassMask &"),
              std::string::npos);
    EXPECT_NE(sharedShader.find("RVX_GPU_SCENE_ROW_FLAG_TOMBSTONE"),
              std::string::npos);
    EXPECT_NE(shader.find("#include \"GPUSceneCulling.hlsli\""),
              std::string::npos);
    EXPECT_NE(shader.find("void CSGPUSceneFrustumCull"), std::string::npos);
    EXPECT_NE(shader.find("RVX_FRUSTUM_REJECT_RELATIVE_TOLERANCE"),
              std::string::npos);
    EXPECT_NE(shader.find("ConservativeFrustumRejectTolerance"),
              std::string::npos);
    EXPECT_NE(shader.find(
                  "signedDistance < -projectedRadius -"),
              std::string::npos);
    EXPECT_NE(shader.find("void CSGPUSceneCompactDraws"), std::string::npos);
    EXPECT_NE(shader.find("void CSGPUSceneFinalizeDrawGroups"),
              std::string::npos);
    EXPECT_NE(shader.find("uint3 groupId : SV_GroupID"),
              std::string::npos);
    EXPECT_NE(shader.find("groupId.y * Counts.z"), std::string::npos);
    EXPECT_NE(shader.find("blockStart += 64u"), std::string::npos);
    const size_t compactBody = shader.find("void CSGPUSceneCompactDraws");
    const size_t finalizeBody = shader.find("void CSGPUSceneFinalizeDrawGroups");
    ASSERT_NE(compactBody, std::string::npos);
    ASSERT_NE(finalizeBody, std::string::npos);
    EXPECT_EQ(shader.substr(compactBody, finalizeBody - compactBody)
                  .find("InterlockedAdd"),
              std::string::npos);
    EXPECT_NE(shader.find("uint4 Counts"), std::string::npos);
    EXPECT_EQ(shader.find("float4 Counts"), std::string::npos);
    EXPECT_EQ(shader.find("(uint)Counts"), std::string::npos);
    EXPECT_NE(normalShader.find("uint4 Counts"), std::string::npos);
    EXPECT_EQ(normalShader.find("float4 Counts"), std::string::npos);
}

TEST_F(GPUDrivenValidationFixture,
       NormalCullingWritesTheCompleteFixedConstantsAbiWithoutLeaseResidue)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());

    const FakeBuffer* constantsBuffer = static_cast<const FakeBuffer*>(
        culling.GetCullingConstantsBuffer());
    ASSERT_NE(nullptr, constantsBuffer);
    const GPUCullingConstants constants =
        ReadBufferValue<GPUCullingConstants>(*constantsBuffer);
    EXPECT_EQ(1u, constants.counts[0]);
    EXPECT_EQ(1u, constants.counts[1]);
    EXPECT_EQ(1u, constants.counts[2]);
    EXPECT_EQ(0u, constants.counts[3]);
    for (uint32 tableIndex = 0; tableIndex < 4; ++tableIndex)
    {
        EXPECT_EQ(0u, constants.gpuSceneTableCounts0[tableIndex]);
        EXPECT_EQ(0u, constants.gpuSceneTableCounts1[tableIndex]);
    }
    EXPECT_EQ(RHIContentValidity::Valid,
              culling.GetAccessSnapshots().constants.uniformAccess.contentValidity);
}

TEST_F(GPUDrivenValidationFixture, GPUSceneCullingShaderEntriesCompileForDX12)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::filesystem::path shaderPath =
        root / "Render" / "Shaders" / "GPUDriven" / "GPUSceneCulling.hlsl";
    const std::string shader = ReadTextFile(shaderPath);
    ASSERT_FALSE(shader.empty());

    std::unique_ptr<IShaderCompiler> compiler = CreateShaderCompiler();
    ASSERT_NE(nullptr, compiler);

    const std::string shaderPathString = shaderPath.string();
    ShaderCompileOptions options;
    options.stage = RHIShaderStage::Compute;
    options.sourceCode = shader.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RHIBackendType::DX12;
    options.targetProfile = "cs_6_0";
    options.enableDebugInfo = false;
    options.enableOptimization = true;

    const ShaderCompileSupport support = compiler->QuerySupport(options);
    if (!support.IsSupported())
    {
        GTEST_SKIP() << support.reason;
    }

    options.entryPoint = "CSGPUSceneFrustumCull";
    ShaderCompileResult frustumResult = compiler->Compile(options);
    ASSERT_TRUE(frustumResult.success) << frustumResult.errorMessage;
    EXPECT_FALSE(frustumResult.bytecode.empty());

    options.entryPoint = "CSGPUSceneCompactDraws";
    ShaderCompileResult compactResult = compiler->Compile(options);
    ASSERT_TRUE(compactResult.success) << compactResult.errorMessage;
    EXPECT_FALSE(compactResult.bytecode.empty());

    options.entryPoint = "CSGPUSceneFinalizeDrawGroups";
    ShaderCompileResult finalizeResult = compiler->Compile(options);
    ASSERT_TRUE(finalizeResult.success) << finalizeResult.errorMessage;
    EXPECT_FALSE(finalizeResult.bytecode.empty());
}

TEST_F(GPUDrivenValidationFixture, GPUDrivenVertexShaderEntriesCompileForDX12SM60)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    std::unique_ptr<IShaderCompiler> compiler = CreateShaderCompiler();
    ASSERT_NE(nullptr, compiler);

    const auto compileVertexEntry = [&compiler](const std::filesystem::path& shaderPath,
                                                const char* entryPoint)
    {
        const std::string shader = ReadTextFile(shaderPath);
        ASSERT_FALSE(shader.empty());

        const std::string shaderPathString = shaderPath.string();
        ShaderCompileOptions options;
        options.stage = RHIShaderStage::Vertex;
        options.sourceCode = shader.c_str();
        options.sourcePath = shaderPathString.c_str();
        options.targetBackend = RHIBackendType::DX12;
        options.targetProfile = "vs_6_0";
        options.entryPoint = entryPoint;
        options.enableDebugInfo = false;
        options.enableOptimization = true;

        const ShaderCompileSupport support = compiler->QuerySupport(options);
        if (!support.IsSupported())
        {
            GTEST_SKIP() << support.reason;
        }

        const ShaderCompileResult result = compiler->Compile(options);
        ASSERT_TRUE(result.success) << result.errorMessage;
        EXPECT_FALSE(result.bytecode.empty());
    };

    compileVertexEntry(
        root / "Render" / "Shaders" / "DefaultLit.hlsl", "VSMainRigid");
    compileVertexEntry(
        root / "Render" / "Shaders" / "DefaultLit.hlsl", "VSMainGPUDriven");
    compileVertexEntry(
        root / "Render" / "Shaders" / "DepthOnly.hlsl", "VSMainRigid");
    compileVertexEntry(
        root / "Render" / "Shaders" / "DepthOnly.hlsl", "VSMainGPUDriven");

    const auto compileGPUSceneVertexEntry = [&compiler](
        const std::filesystem::path& shaderPath,
        const char* entryPoint)
    {
        const std::string shader = ReadTextFile(shaderPath);
        ASSERT_FALSE(shader.empty());

        const std::string shaderPathString = shaderPath.string();
        ShaderCompileOptions options;
        options.stage = RHIShaderStage::Vertex;
        options.sourceCode = shader.c_str();
        options.sourcePath = shaderPathString.c_str();
        options.targetBackend = RHIBackendType::DX12;
        options.targetProfile = "vs_6_0";
        options.entryPoint = entryPoint;
        options.defines.push_back({"RVX_GPU_SCENE_RASTER", "1"});
        options.enableDebugInfo = false;
        options.enableOptimization = true;

        const ShaderCompileSupport support = compiler->QuerySupport(options);
        if (!support.IsSupported())
        {
            GTEST_SKIP() << support.reason;
        }

        const ShaderCompileResult result = compiler->Compile(options);
        ASSERT_TRUE(result.success) << result.errorMessage;
        EXPECT_FALSE(result.bytecode.empty());
        EXPECT_FALSE(result.sourceInfo.IsEmpty());

        const auto containsInclude = [&result](const char* filename)
        {
            return std::any_of(
                result.sourceInfo.includeFiles.begin(), result.sourceInfo.includeFiles.end(),
                [filename](const std::string& includePath)
                {
                    return std::filesystem::path(includePath).filename() == filename;
                });
        };
        EXPECT_TRUE(containsInclude("GPUSceneRaster.hlsli"));
        EXPECT_TRUE(containsInclude("GPUSceneCulling.hlsli"));
    };

    compileGPUSceneVertexEntry(
        root / "Render" / "Shaders" / "DefaultLit.hlsl", "VSMainGPUScene");
    compileGPUSceneVertexEntry(
        root / "Render" / "Shaders" / "DefaultLit.hlsl",
        "VSMainGPUSceneInstancedMaterial");
    compileGPUSceneVertexEntry(
        root / "Render" / "Shaders" / "DepthOnly.hlsl", "VSMainGPUScene");
}

TEST_F(GPUDrivenValidationFixture,
       RigidVertexShaderEntriesCompileForVulkanSM60)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    std::unique_ptr<IShaderCompiler> compiler = CreateShaderCompiler();
    ASSERT_NE(nullptr, compiler);

    const auto compileVertexEntry = [&compiler](const std::filesystem::path& shaderPath,
                                                const char* entryPoint,
                                                bool gpuSceneRaster)
    {
        const std::string shader = ReadTextFile(shaderPath);
        ASSERT_FALSE(shader.empty());

        const std::string shaderPathString = shaderPath.string();
        ShaderCompileOptions options;
        options.stage = RHIShaderStage::Vertex;
        options.sourceCode = shader.c_str();
        options.sourcePath = shaderPathString.c_str();
        options.targetBackend = RHIBackendType::Vulkan;
        options.targetProfile = "vs_6_0";
        options.entryPoint = entryPoint;
        if (gpuSceneRaster)
        {
            options.defines.push_back({"RVX_GPU_SCENE_RASTER", "1"});
        }
        options.enableDebugInfo = false;
        options.enableOptimization = true;

        const ShaderCompileSupport support = compiler->QuerySupport(options);
        if (!support.IsSupported())
        {
            GTEST_SKIP() << support.reason;
        }

        const ShaderCompileResult result = compiler->Compile(options);
        ASSERT_TRUE(result.success) << result.errorMessage;
        EXPECT_FALSE(result.bytecode.empty());
    };

    compileVertexEntry(
        root / "Render" / "Shaders" / "DefaultLit.hlsl", "VSMainRigid", false);
    compileVertexEntry(
        root / "Render" / "Shaders" / "DefaultLit.hlsl", "VSMainGPUScene", true);
    compileVertexEntry(
        root / "Render" / "Shaders" / "DepthOnly.hlsl", "VSMainRigid", false);
    compileVertexEntry(
        root / "Render" / "Shaders" / "DepthOnly.hlsl", "VSMainGPUScene", true);
}

TEST_F(GPUDrivenValidationFixture,
       GPUSceneRasterShaderUsesSharedAffineTransformAndFailClosedResolution)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());
    const std::string rasterInclude = ReadTextFile(
        root / "Render" / "Shaders" / "GPUDriven" / "GPUSceneRaster.hlsli");
    const std::string sharedSchema = ReadTextFile(
        root / "Render" / "Shaders" / "GPUDriven" / "GPUSceneCulling.hlsli");
    const std::string cpuSchema = ReadTextFile(
        root / "Render" / "Include" / "Render" / "GPUScene" /
        "GPUSceneSchema.h");
    const std::string defaultLit = ReadTextFile(
        root / "Render" / "Shaders" / "DefaultLit.hlsl");
    const std::string depthOnly = ReadTextFile(
        root / "Render" / "Shaders" / "DepthOnly.hlsl");
    ASSERT_FALSE(rasterInclude.empty());
    ASSERT_FALSE(sharedSchema.empty());
    ASSERT_FALSE(cpuSchema.empty());
    ASSERT_FALSE(defaultLit.empty());
    ASSERT_FALSE(depthOnly.empty());

    EXPECT_NE(rasterInclude.find("candidate.rasterInstanceIndex != rasterInstanceIndex"),
              std::string::npos);
    EXPECT_NE(rasterInclude.find("GPUSceneIsLiveHeader(primitive.header"),
              std::string::npos);
    EXPECT_NE(rasterInclude.find("GPUSceneIsLiveHeader(transform.header"),
              std::string::npos);
    EXPECT_NE(sharedSchema.find("RVX_GPU_SCENE_ROW_FLAG_TOMBSTONE"),
              std::string::npos);
    EXPECT_NE(rasterInclude.find("float4 RVXTransformRigidAffinePosition("),
              std::string::npos);
    EXPECT_NE(rasterInclude.find("dot(worldFromLocalRow0, localPosition)"),
              std::string::npos);
    EXPECT_NE(rasterInclude.find("return RVXTransformRigidAffinePosition("),
              std::string::npos);
    EXPECT_NE(rasterInclude.find("dot(transform.normalFromLocal[0].xyz"),
              std::string::npos);
    EXPECT_NE(rasterInclude.find("GPUSceneInvalidClipPosition"), std::string::npos);
    EXPECT_NE(rasterInclude.find("out uint primitiveFlags"), std::string::npos);
    EXPECT_NE(rasterInclude.find("RVX_GPU_SCENE_PRIMITIVE_RECEIVES_SHADOW (1u << 2u)"),
              std::string::npos);
    EXPECT_NE(cpuSchema.find("enum class GPUScenePrimitiveFlags : uint32"),
              std::string::npos);
    EXPECT_NE(cpuSchema.find("ReceivesShadow = 1U << 2U"), std::string::npos);
    EXPECT_NE(cpuSchema.find("HasGPUScenePrimitiveFlag"), std::string::npos);
    EXPECT_NE(defaultLit.find("PSInput VSMainGPUScene"), std::string::npos);
    EXPECT_NE(depthOnly.find("VSOutput VSMainGPUScene"), std::string::npos);
    EXPECT_NE(defaultLit.find("output.WorldTangent = float4(0.0f"), std::string::npos);
    EXPECT_NE(defaultLit.find("GPUSceneTransformNormal"), std::string::npos);
    EXPECT_NE(defaultLit.find("GPUSceneTransformTangent"), std::string::npos);
    EXPECT_NE(defaultLit.find("nointerpolation float ReceivesShadowValue : TEXCOORD4"),
              std::string::npos);
    EXPECT_NE(defaultLit.find("primitiveFlags & RVX_GPU_SCENE_PRIMITIVE_RECEIVES_SHADOW"),
              std::string::npos);
    EXPECT_NE(defaultLit.find("input.ReceivesShadowValue > 0.5"),
              std::string::npos);

    constexpr uint32 receivesShadow =
        static_cast<uint32>(GPUScenePrimitiveFlags::ReceivesShadow);
    const uint32 mixedPrimitiveFlags[] = {receivesShadow, 0u};
    EXPECT_TRUE(HasGPUScenePrimitiveFlag(
        mixedPrimitiveFlags[0], GPUScenePrimitiveFlags::ReceivesShadow));
    EXPECT_FALSE(HasGPUScenePrimitiveFlag(
        mixedPrimitiveFlags[1], GPUScenePrimitiveFlags::ReceivesShadow));

    const auto assertSharedAffineHelperIsAvailableToRigidPaths =
        [](const std::string& shaderSource)
    {
        const size_t helperInclude = shaderSource.find(
            "#include \"GPUDriven/GPUSceneRaster.hlsli\"");
        ASSERT_NE(std::string::npos, helperInclude);

        const size_t rigidEntry = shaderSource.find("VSMainRigid");
        ASSERT_NE(std::string::npos, rigidEntry);
        EXPECT_LT(helperInclude, rigidEntry);

        const size_t gpuSceneEntry = shaderSource.find("VSMainGPUScene");
        ASSERT_NE(std::string::npos, gpuSceneEntry);
        const size_t entryGuard = shaderSource.rfind(
            "#if defined(RVX_GPU_SCENE_RASTER)", gpuSceneEntry);
        ASSERT_NE(std::string::npos, entryGuard);
        EXPECT_LT(entryGuard, gpuSceneEntry);
        EXPECT_EQ(std::string::npos,
                  shaderSource.substr(0, entryGuard).find(
                      "GPUSceneResolveRasterTransform"));
    };
    assertSharedAffineHelperIsAvailableToRigidPaths(defaultLit);
    assertSharedAffineHelperIsAvailableToRigidPaths(depthOnly);

    EXPECT_NE(defaultLit.find("RVXTransformRigidAffinePosition("),
              std::string::npos);
    EXPECT_NE(depthOnly.find("RVXTransformRigidAffinePosition("),
              std::string::npos);
    EXPECT_NE(defaultLit.find("World[0], World[1], World[2], input.Position"),
              std::string::npos);
    EXPECT_NE(depthOnly.find("World[0], World[1], World[2], input.Position"),
              std::string::npos);
    EXPECT_NE(defaultLit.find("instance.worldMatrix[0]"), std::string::npos);
    EXPECT_NE(depthOnly.find("world[0], world[1], world[2], input.Position"),
              std::string::npos);
    EXPECT_EQ(defaultLit.find("mul(World, float4(input.Position, 1.0f))"),
              std::string::npos);
    EXPECT_EQ(depthOnly.find("mul(World, float4(input.Position, 1.0f))"),
              std::string::npos);
    EXPECT_EQ(defaultLit.find("mul(instance.worldMatrix, float4(input.Position, 1.0))"),
              std::string::npos);
    EXPECT_EQ(depthOnly.find("mul(world, float4(input.Position, 1.0))"),
              std::string::npos);

    // GPUSceneUpdate packs GLM's column-major Mat4 into explicit rows.  The
    // row-dot contract below therefore matches Tier1's matrix * vector path,
    // including a non-uniform normal transform.
    Mat4 world = Mat4Identity();
    world[0][0] = 2.0f;
    world[1][1] = 3.0f;
    world[2][2] = 4.0f;
    world[3][0] = 5.0f;
    world[3][1] = -2.0f;
    world[3][2] = 7.0f;
    const Vec4 position(1.5f, -2.0f, 0.25f, 1.0f);
    const Vec4 tierOnePosition = world * position;
    const Vec4 gpuScenePosition(
        world[0][0] * position.x + world[1][0] * position.y +
            world[2][0] * position.z + world[3][0] * position.w,
        world[0][1] * position.x + world[1][1] * position.y +
            world[2][1] * position.z + world[3][1] * position.w,
        world[0][2] * position.x + world[1][2] * position.y +
            world[2][2] * position.z + world[3][2] * position.w,
        1.0f);
    EXPECT_FLOAT_EQ(tierOnePosition.x, gpuScenePosition.x);
    EXPECT_FLOAT_EQ(tierOnePosition.y, gpuScenePosition.y);
    EXPECT_FLOAT_EQ(tierOnePosition.z, gpuScenePosition.z);
    EXPECT_FLOAT_EQ(tierOnePosition.w, gpuScenePosition.w);

    Mat4 normal = Mat4Identity();
    normal[0][0] = 0.5f;
    normal[1][1] = 1.0f / 3.0f;
    normal[2][2] = 0.25f;
    const Vec4 inputNormal(0.25f, 0.5f, -0.75f, 0.0f);
    const Vec4 tierOneNormal = normal * inputNormal;
    const Vec3 gpuSceneNormal(
        normal[0][0] * inputNormal.x + normal[1][0] * inputNormal.y +
            normal[2][0] * inputNormal.z,
        normal[0][1] * inputNormal.x + normal[1][1] * inputNormal.y +
            normal[2][1] * inputNormal.z,
        normal[0][2] * inputNormal.x + normal[1][2] * inputNormal.y +
            normal[2][2] * inputNormal.z);
    EXPECT_FLOAT_EQ(tierOneNormal.x, gpuSceneNormal.x);
    EXPECT_FLOAT_EQ(tierOneNormal.y, gpuSceneNormal.y);
    EXPECT_FLOAT_EQ(tierOneNormal.z, gpuSceneNormal.z);
}
