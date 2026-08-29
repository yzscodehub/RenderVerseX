#pragma once

/**
 * @file LocalShadowPass.h
 * @brief RenderGraph recording primitive for the immutable local-shadow atlas.
 */

#include "Render/Graph/RenderGraph.h"
#include "Render/Lighting/LocalShadowAtlas.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    /** Result of registering a local-shadow atlas with the current graph. */
    enum class LocalShadowPassCode : uint8
    {
        Ready = 0,
        NoWork,
        InvalidRequest,
        StaleAtlasPlan,
        GraphResourceCreationFailed,
    };

    /**
     * Optional geometry recorder invoked inside an already-open depth pass.
     *
     * The recorder must bind only resources that were prepared before graph
     * execution and retained by the caller. It must not allocate RHI resources
     * or wait for the GPU. An empty recorder is an honest depth-clear-only
     * page, useful when the sealed view contains no eligible casters.
     */
    using LocalShadowCasterRecorder = std::function<bool(
        RHICommandContext& commands,
        const LocalShadowAtlasPage& page,
        uint32& outDrawCount,
        std::string& outDiagnostic)>;

    /** Immutable request pinned to one CPU atlas/matrix plan generation. */
    struct LocalShadowPassRequest
    {
        std::shared_ptr<const LocalShadowAtlasPlan> atlasPlan{};
        /** Optional non-zero stale-plan pin. */
        uint64 expectedAtlasPlanIdentity = 0;
        /** Optional non-zero stale-resource-generation pin. */
        uint64 expectedAtlasGeneration = 0;
        LocalShadowCasterRecorder recordCasters{};
    };

    /**
     * Immutable graph output consumed by the lighting path in the same frame.
     *
     * `depthAtlas` is exported as ShaderResource after every page writes. The
     * page handles carry exact one-layer ranges so a consumer can validate the
     * point six-face/spot one-face mapping without depending on physical order.
     * This value reports registration only; execution evidence is deliberately
     * obtained after `FinalizeGraphExecution()`.
     */
    struct LocalShadowPassOutput
    {
        LocalShadowPassCode code = LocalShadowPassCode::InvalidRequest;
        RGTextureHandle depthAtlas{};
        std::vector<RGTextureHandle> pageDepthLayers{};
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        uint64 lightingPlanIdentity = 0;
        uint64 localShadowPlanIdentity = 0;
        uint64 atlasPlanIdentity = 0;
        uint64 atlasGeneration = 0;
        uint32 pageResolution = 0;
        uint32 pageCount = 0;
        std::string diagnostic{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            return code == LocalShadowPassCode::Ready && depthAtlas.IsValid() &&
                   pageCount != 0 && pageDepthLayers.size() == pageCount &&
                   frameIdentity.IsValid() && viewIdentity.IsValid() &&
                   lightingPlanIdentity != 0 && localShadowPlanIdentity != 0 &&
                   atlasPlanIdentity != 0 && atlasGeneration != 0 &&
                   pageResolution != 0;
        }
    };

    /** Exact post-execution evidence; never inferred from graph registration. */
    struct LocalShadowPassExecutionEvidence
    {
        uint64 atlasPlanIdentity = 0;
        uint64 atlasGeneration = 0;
        uint32 expectedPageCount = 0;
        uint32 realizedPageCount = 0;
        uint32 recordedPageCount = 0;
        uint32 casterDrawCount = 0;
        bool cpuPlanReady = false;
        bool gpuResourceCreated = false;
        bool gpuCommandsRecorded = false;
        bool gpuExecuted = false;
        std::string diagnostic{};

        [[nodiscard]] bool HasExactGpuExecution() const noexcept
        {
            return cpuPlanReady && gpuResourceCreated && gpuCommandsRecorded &&
                   gpuExecuted && expectedPageCount != 0 &&
                   realizedPageCount == expectedPageCount &&
                   recordedPageCount == expectedPageCount && diagnostic.empty();
        }
    };

    /**
     * Produces a Texture2DArray D32 atlas and one RenderGraph graphics pass per
     * immutable atlas page. Caster drawing is a narrow composition-time
     * callback because the existing draw plans/pipelines are owned outside this
     * low-level atlas primitive.
     */
    class LocalShadowPass final
    {
    public:
        /** Opaque execution bookkeeping retained only until graph finalization. */
        struct ExecutionState;

        LocalShadowPass() = default;
        ~LocalShadowPass();

        LocalShadowPass(const LocalShadowPass&) = delete;
        LocalShadowPass& operator=(const LocalShadowPass&) = delete;

        [[nodiscard]] LocalShadowPassOutput AddToGraph(
            RenderGraph& graph,
            const LocalShadowPassRequest& request);

        /**
         * Publishes execution truth only after the owning graph accepted this
         * exact recording. A rejected or incomplete graph leaves GPU evidence
         * false, even though resources may have been scheduled.
         */
        void FinalizeGraphExecution(bool executionAccepted) noexcept;

        [[nodiscard]] LocalShadowPassExecutionEvidence
            GetLastExecutionEvidence() const;

    private:
        std::shared_ptr<ExecutionState> m_pendingExecutionState{};
        LocalShadowPassExecutionEvidence m_lastExecutionEvidence{};
    };

    [[nodiscard]] const char* GetLocalShadowPassCodeName(
        LocalShadowPassCode code) noexcept;
} // namespace RVX
