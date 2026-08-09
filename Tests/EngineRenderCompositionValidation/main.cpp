#include "RenderRuntimeComposition.h"

#include "RenderContracts/RenderFramePacketV5.h"
#include "World/World.h"

#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace RVX;

namespace
{
    NativeSurfaceDesc MakeSurface(uint64 generation = 1,
                                  uint32 width = 1280,
                                  uint32 height = 720)
    {
        NativeSurfaceDesc surface;
        surface.platform = NativeSurfacePlatform::GLFW;
        surface.backendWindow = 1;
        surface.width = width;
        surface.height = height;
        surface.generation = generation;
        return surface;
    }

    std::unique_ptr<const RenderFramePacketV5> MakeCompletePacket(
        const RenderRuntimeCompositionFrameInput& input)
    {
        RenderFrameHeaderV5 header;
        header.sequence = input.sequence;
        header.requiredSceneRevision = input.sequence;
        header.worldRevision = input.worldRevision;
        header.temporalEpoch = input.temporalEpoch;
        header.explicitDiscontinuity = input.explicitDiscontinuity;

        RenderViewSnapshot view;
        view.viewportWidth = input.view.viewportWidth;
        view.viewportHeight = input.view.viewportHeight;
        view.absoluteTime = input.view.absoluteTime;
        view.deltaTime = input.view.deltaTime;

        RenderExtractionDiagnostics diagnostics;
        diagnostics.complete = true;
        return RenderFramePacketV5::Create(
            std::move(header),
            std::move(view),
            input.settings,
            input.captureRequest,
            std::move(diagnostics));
    }

    struct PublishedFrame
    {
        RenderFrameHeaderV5 header{};
        RenderFrameCaptureRequest capture{};
        RenderFrameSettings settings{};
    };

    class FakeCompositionServices final
        : public IRenderRuntimeCompositionServices
    {
    public:
        void InjectRenderResourceGateway() override
        {
            events.emplace_back("inject-resource-gateway");
            gatewayInjected = true;
        }

        bool IsWindowInitialized() const noexcept override
        {
            return windowInitialized;
        }

        NativeSurfaceDesc CaptureRenderSurface() override
        {
            events.emplace_back("capture-surface");
            ++surfaceCaptureCount;
            return surface;
        }

        void ReleaseGraphicsContextFromUpdateThread() override
        {
            events.emplace_back("release-graphics-context");
            contextReleased = true;
        }

        void ConfigureRender(const RenderRuntimeConfig&,
                             const NativeSurfaceDesc& configuredSurface) override
        {
            events.emplace_back("configure-render");
            configured = true;
            lastConfiguredSurface = configuredSurface;
        }

        bool IsRenderReady() const noexcept override
        {
            return renderReady;
        }

        RenderFrameExtractionResult ExtractFrame(
            const RenderRuntimeCompositionFrameInput& input) override
        {
            events.emplace_back("extract-frame");
            extractionInputs.push_back(input);

            RenderFrameExtractionResult result;
            if (!extractionCodes.empty())
            {
                result.code = extractionCodes.front();
                extractionCodes.pop_front();
                if (result.code != RenderFrameExtractionResultCode::Complete)
                {
                    return result;
                }
            }

            result.frameV5 = MakeCompletePacket(input);
            result.code = result.frameV5 != nullptr
                              ? RenderFrameExtractionResultCode::Complete
                              : RenderFrameExtractionResultCode::SealFailed;
            return result;
        }

        RenderFramePublishResult PublishExtractedFrame(
            RenderFrameExtractionResult extraction) override
        {
            events.emplace_back("publish-frame");
            if (extraction.frameV5 == nullptr)
            {
                return {};
            }

            publishedFrames.push_back(
                {extraction.frameV5->GetHeader(),
                 extraction.frameV5->GetCaptureRequest(),
                 extraction.frameV5->GetSettings()});
            RenderFramePublishResult result;
            result.code = publishCodes.empty()
                              ? RenderFramePublishCode::Accepted
                              : publishCodes.front();
            if (!publishCodes.empty())
            {
                publishCodes.pop_front();
            }
            result.resultClass = ClassifyRenderFramePublishCode(result.code);
            result.sequence = extraction.frameV5->GetHeader().sequence;
            return result;
        }

        RenderDiagnosticsSnapshot GetRenderDiagnostics() const override
        {
            return diagnostics;
        }

        RenderResizeResult RequestResize(
            const NativeSurfaceDesc& requestedSurface) override
        {
            events.emplace_back("request-resize");
            resizedSurfaces.push_back(requestedSurface);
            RenderResizeResult result;
            result.code = resizeCode;
            result.resultClass = ClassifyRenderResizeCode(result.code);
            result.generation = requestedSurface.generation;
            return result;
        }

        void BeginResourceShutdown() override
        {
            events.emplace_back("begin-resource-shutdown");
        }

        RenderShutdownResult StopRender() override
        {
            events.emplace_back("stop-render");
            return shutdownResult;
        }

        void DrainTerminalRenderRequests() override
        {
            events.emplace_back("drain-terminal-requests");
        }

        bool gatewayInjected = false;
        bool windowInitialized = true;
        bool contextReleased = false;
        bool configured = false;
        bool renderReady = true;
        uint32 surfaceCaptureCount = 0;
        NativeSurfaceDesc surface = MakeSurface();
        NativeSurfaceDesc lastConfiguredSurface{};
        RenderResizeCode resizeCode = RenderResizeCode::Accepted;
        RenderShutdownResult shutdownResult{};
        RenderDiagnosticsSnapshot diagnostics{};
        std::deque<RenderFrameExtractionResultCode> extractionCodes;
        std::deque<RenderFramePublishCode> publishCodes;
        std::vector<RenderRuntimeCompositionFrameInput> extractionInputs;
        std::vector<PublishedFrame> publishedFrames;
        std::vector<NativeSurfaceDesc> resizedSurfaces;
        std::vector<std::string> events;
    };

    struct CompositionFixture
    {
        explicit CompositionFixture(
            RHIBackendType backend = RHIBackendType::Vulkan)
        {
            RenderRuntimeConfig config;
            config.backendType = backend;
            auto ownedServices = std::make_unique<FakeCompositionServices>();
            services = ownedServices.get();
            composition = std::make_unique<RenderRuntimeComposition>(
                config,
                RenderFrameSettings{},
                std::move(ownedServices));
        }

        void PrepareAndConfigure()
        {
            ASSERT_TRUE(composition->PrepareBeforeSubsystemInitialization());
            composition->BeforeRenderSubsystemInitialize();
        }

        FakeCompositionServices* services = nullptr;
        std::unique_ptr<RenderRuntimeComposition> composition;
    };

    std::string ReadSource(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        std::ostringstream contents;
        contents << stream.rdbuf();
        return contents.str();
    }
} // namespace

TEST(EngineRenderCompositionValidation,
     InjectsGatewayBeforeStagedWindowCaptureAndRenderConfiguration)
{
    CompositionFixture fixture;
    fixture.services->windowInitialized = false;

    ASSERT_TRUE(fixture.composition->PrepareBeforeSubsystemInitialization());
    EXPECT_TRUE(fixture.services->gatewayInjected);
    EXPECT_THROW(
        fixture.composition->BeforeRenderSubsystemInitialize(),
        std::runtime_error);
    EXPECT_FALSE(fixture.services->configured);

    fixture.services->windowInitialized = true;
    fixture.composition->BeforeRenderSubsystemInitialize();
    EXPECT_TRUE(fixture.services->configured);
    EXPECT_FALSE(fixture.services->contextReleased);
    ASSERT_GE(fixture.services->events.size(), 3U);
    EXPECT_EQ(fixture.services->events[0], "inject-resource-gateway");
    EXPECT_EQ(fixture.services->events[fixture.services->events.size() - 2],
              "capture-surface");
    EXPECT_EQ(fixture.services->events.back(), "configure-render");
}

TEST(EngineRenderCompositionValidation,
     ReleasesOpenGLContextBeforeRenderConfiguration)
{
    CompositionFixture fixture(RHIBackendType::OpenGL);
    fixture.PrepareAndConfigure();

    EXPECT_TRUE(fixture.services->contextReleased);
    const std::vector<std::string> expected{
        "inject-resource-gateway",
        "capture-surface",
        "release-graphics-context",
        "configure-render"};
    EXPECT_EQ(fixture.services->events, expected);
}

TEST(EngineRenderCompositionValidation,
     RejectsInvalidInitialAndRuntimeFrameSettings)
{
    RenderFrameSettings invalidSettings;
    invalidSettings.rayTracing.enableShadows = true;

    RenderRuntimeConfig config;
    config.backendType = RHIBackendType::Vulkan;
    auto ownedServices = std::make_unique<FakeCompositionServices>();
    FakeCompositionServices* services = ownedServices.get();
    RenderRuntimeComposition invalidComposition(
        config,
        invalidSettings,
        std::move(ownedServices));
    EXPECT_FALSE(
        invalidComposition.PrepareBeforeSubsystemInitialization());
    EXPECT_FALSE(services->gatewayInjected);

    CompositionFixture fixture;
    fixture.PrepareAndConfigure();
    EXPECT_FALSE(fixture.composition->SetFrameSettings(invalidSettings));

    RenderFrameSettings validSettings;
    validSettings.renderScale = 0.75f;
    EXPECT_TRUE(fixture.composition->SetFrameSettings(validSettings));
    EXPECT_FLOAT_EQ(fixture.composition->GetFrameSettings().renderScale,
                    0.75f);
}

TEST(EngineRenderCompositionValidation,
     IncompleteExtractionIsNotPublishedAndSequenceIsNotRetried)
{
    CompositionFixture fixture;
    fixture.PrepareAndConfigure();
    fixture.services->extractionCodes.push_back(
        RenderFrameExtractionResultCode::MissingCamera);
    World world;

    fixture.composition->TickAfterWorlds(&world, 1.0f / 60.0f, 1.0f);
    EXPECT_TRUE(fixture.services->publishedFrames.empty());
    EXPECT_EQ(fixture.composition->GetStats().lastAttemptedSequence, 1U);

    fixture.composition->TickAfterWorlds(&world, 1.0f / 60.0f, 2.0f);
    ASSERT_EQ(fixture.services->publishedFrames.size(), 1U);
    EXPECT_EQ(fixture.services->publishedFrames.front().header.sequence, 2U);
    EXPECT_EQ(fixture.composition->GetStats().extractionAttempts, 2U);
    EXPECT_EQ(fixture.composition->GetStats().extractionCompleted, 1U);
}

TEST(EngineRenderCompositionValidation,
     PublishPressurePreservesOneShotValuesUntilRenderAcknowledgesThem)
{
    CompositionFixture fixture;
    fixture.PrepareAndConfigure();
    fixture.services->publishCodes.push_back(
        RenderFramePublishCode::NotRunning);
    fixture.services->publishCodes.push_back(
        RenderFramePublishCode::ReplacedOlder);
    fixture.services->publishCodes.push_back(
        RenderFramePublishCode::Accepted);

    RenderFrameCaptureRequest capture;
    capture.requestId = 41;
    capture.kind = RenderFrameCaptureKind::Color;
    capture.width = 320;
    capture.height = 180;
    ASSERT_TRUE(fixture.composition->QueueCapture(capture));
    fixture.composition->OnActiveWorldChanged();
    const uint64 temporalEpoch =
        fixture.composition->RequestTemporalReset();
    World world;

    fixture.composition->TickAfterWorlds(&world, 0.01f, 1.0f);
    fixture.composition->TickAfterWorlds(&world, 0.01f, 2.0f);
    fixture.composition->TickAfterWorlds(&world, 0.01f, 3.0f);

    fixture.services->diagnostics.lastAppliedFrameSequence = 3;
    fixture.services->diagnostics.lastCapture.requestId = 41;
    fixture.services->diagnostics.lastCapture.frameSequence = 3;
    fixture.services->diagnostics.lastCapture.code =
        RenderFrameCaptureResultCode::Completed;
    fixture.services->diagnostics.lastCapture.kind =
        RenderFrameCaptureKind::Color;
    fixture.services->diagnostics.lastCapture.width = 320;
    fixture.services->diagnostics.lastCapture.height = 180;
    fixture.services->diagnostics.lastCapture.rowPitch = 1280;
    fixture.services->diagnostics.lastCapture.bytesPerPixel = 4;
    fixture.services->diagnostics.lastCapture.bytes.resize(1280U * 180U);
    fixture.composition->TickAfterWorlds(&world, 0.01f, 4.0f);

    ASSERT_EQ(fixture.services->publishedFrames.size(), 4U);
    EXPECT_EQ(fixture.services->publishedFrames[0].header.sequence, 1U);
    EXPECT_EQ(fixture.services->publishedFrames[1].header.sequence, 2U);
    EXPECT_EQ(fixture.services->publishedFrames[2].header.sequence, 3U);
    EXPECT_EQ(fixture.services->publishedFrames[0].capture.requestId, 41U);
    EXPECT_EQ(fixture.services->publishedFrames[1].capture.requestId, 41U);
    EXPECT_EQ(fixture.services->publishedFrames[2].capture.requestId, 41U);
    EXPECT_EQ(fixture.services->publishedFrames[3].capture.requestId, 0U);
    EXPECT_TRUE(fixture.services->publishedFrames[0]
                    .header.explicitDiscontinuity);
    EXPECT_TRUE(fixture.services->publishedFrames[1]
                    .header.explicitDiscontinuity);
    EXPECT_TRUE(fixture.services->publishedFrames[2]
                     .header.explicitDiscontinuity);
    EXPECT_FALSE(fixture.services->publishedFrames[3]
                     .header.explicitDiscontinuity);
    EXPECT_EQ(fixture.services->publishedFrames[0].header.temporalEpoch,
              temporalEpoch);
    EXPECT_EQ(fixture.composition->GetStats().publicationRejected, 1U);
    EXPECT_EQ(fixture.composition->GetStats().publicationAccepted, 3U);
}

TEST(EngineRenderCompositionValidation,
     RoutesEachNewSurfaceGenerationOnlyAfterResizeAcceptance)
{
    CompositionFixture fixture;
    fixture.PrepareAndConfigure();

    fixture.composition->TickAfterWorlds(nullptr, 0.0f, 0.0f);
    EXPECT_TRUE(fixture.services->resizedSurfaces.empty());

    fixture.services->surface = MakeSurface(2, 1920, 1080);
    fixture.composition->TickAfterWorlds(nullptr, 0.0f, 0.0f);
    fixture.composition->TickAfterWorlds(nullptr, 0.0f, 0.0f);

    ASSERT_EQ(fixture.services->resizedSurfaces.size(), 1U);
    EXPECT_EQ(fixture.services->resizedSurfaces.front().generation, 2U);
    EXPECT_EQ(fixture.composition->GetStats().surfaceGeneration, 2U);
    EXPECT_EQ(fixture.composition->GetStats().resizeRequests, 1U);
}

TEST(EngineRenderCompositionValidation,
     ExplicitResizePublishesNextGenerationAndTemporalDiscontinuity)
{
    CompositionFixture fixture;
    fixture.PrepareAndConfigure();

    EXPECT_FALSE(fixture.composition->RequestSurfaceResize(0, 720));
    EXPECT_TRUE(fixture.services->resizedSurfaces.empty());

    fixture.services->resizeCode = RenderResizeCode::StaleGeneration;
    EXPECT_FALSE(fixture.composition->RequestSurfaceResize(800, 600));
    ASSERT_EQ(fixture.services->resizedSurfaces.size(), 1U);
    EXPECT_EQ(fixture.composition->GetStats().surfaceGeneration, 1U);

    fixture.services->resizeCode = RenderResizeCode::Accepted;
    ASSERT_TRUE(fixture.composition->RequestSurfaceResize(800, 600));
    ASSERT_EQ(fixture.services->resizedSurfaces.size(), 2U);
    EXPECT_EQ(fixture.services->resizedSurfaces.back().generation, 2U);
    EXPECT_EQ(fixture.services->resizedSurfaces.back().width, 800U);
    EXPECT_EQ(fixture.services->resizedSurfaces.back().height, 600U);
    EXPECT_EQ(fixture.composition->GetStats().surfaceGeneration, 2U);

    World world;
    fixture.composition->TickAfterWorlds(&world, 0.01f, 1.0f);
    ASSERT_EQ(fixture.services->publishedFrames.size(), 1U);
    EXPECT_TRUE(
        fixture.services->publishedFrames.front().header.explicitDiscontinuity);
    EXPECT_EQ(fixture.services->extractionInputs.front().view.viewportWidth,
              800U);
    EXPECT_EQ(fixture.services->extractionInputs.front().view.viewportHeight,
              600U);
}

TEST(EngineRenderCompositionValidation,
     ShutdownSealsResourcesStopsRenderAndDrainsExactlyOnce)
{
    CompositionFixture fixture;
    fixture.PrepareAndConfigure();
    fixture.services->events.clear();
    fixture.services->shutdownResult.code = RenderShutdownCode::Completed;
    fixture.services->shutdownResult.lifecycle = RenderLifecycleState::Stopped;

    const RenderShutdownResult first = fixture.composition->Shutdown();
    const RenderShutdownResult second = fixture.composition->Shutdown();

    EXPECT_EQ(first.code, RenderShutdownCode::Completed);
    EXPECT_EQ(second.code, RenderShutdownCode::Completed);
    const std::vector<std::string> expected{
        "begin-resource-shutdown",
        "stop-render",
        "drain-terminal-requests"};
    EXPECT_EQ(fixture.services->events, expected);
}

TEST(EngineRenderCompositionValidation,
     EngineSourceUsesValueCompositionAndOrderedShutdownBoundary)
{
    const std::filesystem::path sourceRoot{RVX_SOURCE_DIR};
    const std::string source =
        ReadSource(sourceRoot / "Engine" / "Private" / "Engine.cpp");
    ASSERT_FALSE(source.empty());

    for (const std::string forbidden : {
             "ProcessGPUUploads(",
             "BeginFrame(",
             "RenderFrame(",
             "EndFrame(",
             "Present("})
    {
        EXPECT_EQ(source.find(forbidden), std::string::npos)
            << "Engine must not invoke legacy render work: " << forbidden;
    }

    const size_t renderShutdown = source.find("ShutdownRenderRuntime();");
    const size_t compositionShutdown =
        source.find("m_renderComposition->Shutdown()");
    const size_t worldShutdown = source.find("ShutdownWorlds();");
    const size_t subsystemShutdown = source.find("ShutdownSubsystems();");
    ASSERT_NE(renderShutdown, std::string::npos);
    ASSERT_NE(compositionShutdown, std::string::npos);
    ASSERT_NE(worldShutdown, std::string::npos);
    ASSERT_NE(subsystemShutdown, std::string::npos);
    EXPECT_LT(renderShutdown, worldShutdown);
    EXPECT_LT(worldShutdown, subsystemShutdown);
}

TEST(EngineRenderCompositionValidation,
     EngineRegistersTypedRenderCompositionDependenciesBeforeInitialization)
{
    const std::filesystem::path sourceRoot{RVX_SOURCE_DIR};
    const std::string engine =
        ReadSource(sourceRoot / "Engine" / "Private" / "Engine.cpp");
    const std::string renderHeader =
        ReadSource(
            sourceRoot / "Render" / "Include" / "Render" /
            "RenderSubsystem.h");
    ASSERT_FALSE(engine.empty());
    ASSERT_FALSE(renderHeader.empty());

    const size_t windowResult =
        engine.find("const auto windowDependency");
    const size_t windowDependency = engine.find(
        "m_subsystems.AddInitializationDependency<",
        windowResult);
    const size_t windowPrerequisite =
        engine.find("WindowSubsystem>()", windowDependency);
    const size_t resourceResult =
        engine.find("const auto resourceDependency", windowPrerequisite);
    const size_t resourceDependency = engine.find(
        "m_subsystems.AddInitializationDependency<",
        resourceResult);
    const size_t resourcePrerequisite =
        engine.find(
            "Resource::ResourceSubsystem>()",
            resourceDependency);
    const size_t prepareComposition =
        engine.find("CreateEngineRenderRuntimeCompositionServices(");
    const size_t initialize =
        engine.find("return m_subsystems.InitializeAll(");

    ASSERT_NE(windowResult, std::string::npos);
    ASSERT_NE(windowDependency, std::string::npos);
    ASSERT_NE(windowPrerequisite, std::string::npos);
    ASSERT_NE(resourceResult, std::string::npos);
    ASSERT_NE(resourceDependency, std::string::npos);
    ASSERT_NE(resourcePrerequisite, std::string::npos);
    ASSERT_NE(prepareComposition, std::string::npos);
    ASSERT_NE(initialize, std::string::npos);
    EXPECT_LT(windowPrerequisite, prepareComposition);
    EXPECT_LT(resourcePrerequisite, prepareComposition);
    EXPECT_LT(prepareComposition, initialize);

    EXPECT_EQ(
        renderHeader.find("Runtime/Window/WindowSubsystem.h"),
        std::string::npos);
    EXPECT_EQ(
        renderHeader.find("WindowSubsystem*"),
        std::string::npos);
    EXPECT_EQ(
        renderHeader.find("RVX_SUBSYSTEM_DEPENDENCIES(WindowSubsystem"),
        std::string::npos);
    EXPECT_EQ(
        renderHeader.find("MakeDependencies<WindowSubsystem"),
        std::string::npos);
}

TEST(EngineRenderCompositionValidation,
     SampleRunnerStopsRenderBeforeReleasingSampleOwnedResources)
{
    const std::filesystem::path sourceRoot{RVX_SOURCE_DIR};
    const std::string source = ReadSource(
        sourceRoot / "Samples" / "Common" / "Private" /
        "SampleRunner.cpp");
    ASSERT_FALSE(source.empty());

    const size_t renderShutdown =
        source.find("engine.ShutdownRenderRuntime();");
    const size_t sampleShutdown =
        source.find("sample->Shutdown(context);");
    ASSERT_NE(renderShutdown, std::string::npos);
    ASSERT_NE(sampleShutdown, std::string::npos);
    EXPECT_LT(renderShutdown, sampleShutdown);
}

TEST(EngineRenderCompositionValidation,
     WindowSurfaceGenerationChangesOnResizeRatherThanCapture)
{
    const std::filesystem::path sourceRoot{RVX_SOURCE_DIR};
    const std::string source = ReadSource(
        sourceRoot / "Runtime" / "Private" / "Window" /
        "WindowSubsystem.cpp");
    ASSERT_FALSE(source.empty());

    const size_t tick = source.find("void WindowSubsystem::Tick(");
    const size_t capture = source.find(
        "NativeSurfaceDesc WindowSubsystem::CaptureRenderSurface(");
    ASSERT_NE(tick, std::string::npos);
    ASSERT_NE(capture, std::string::npos);
    ASSERT_LT(tick, capture);

    const size_t increment = source.find("++m_surfaceGeneration", tick);
    ASSERT_NE(increment, std::string::npos);
    EXPECT_LT(increment, capture);
    EXPECT_EQ(source.find("++m_surfaceGeneration", capture),
              std::string::npos);
    EXPECT_NE(source.find(
                  "surface.generation = m_surfaceGeneration", capture),
              std::string::npos);
}
