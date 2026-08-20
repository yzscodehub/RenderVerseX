/** @file main.cpp @brief Cooked-only Shipping product lifecycle smoke. */

#include "Core/Hash/SHA256.h"
#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "Engine/Engine.h"
#include "Engine/ShippingRuntimeComposition.h"
#include "Render/RenderSubsystem.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/ModelResource.h"
#include "Runtime/Window/WindowSubsystem.h"
#include "World/World.h"

#include <charconv>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

static_assert(!std::is_default_constructible_v<RVX::Engine>);
static_assert(!std::is_copy_constructible_v<RVX::EngineRuntimeDependencies>);
static_assert(
    std::is_nothrow_move_constructible_v<RVX::EngineRuntimeDependencies>);

namespace
{
    constexpr RVX::uint32 RequiredFrames = 8;
    constexpr RVX::uint32 RequiredWidth = 320;
    constexpr RVX::uint32 RequiredHeight = 180;
    constexpr float FixedDeltaSeconds = 1.0f / 60.0f;

    struct Options
    {
        RVX::RHIBackendType backend = RVX::RHIBackendType::None;
        std::string backendName;
        std::filesystem::path cookedRoot;
        std::filesystem::path shaderRoot;
        std::string model;
        std::filesystem::path fixtureReceipt;
        std::string commit;
        std::string preset;
        RVX::uint32 frames = 0;
        RVX::uint32 width = 0;
        RVX::uint32 height = 0;
        std::filesystem::path report;
    };

    class LogLifetime final
    {
    public:
        LogLifetime() { RVX::Log::Initialize(); }
        ~LogLifetime() { RVX::Log::Shutdown(); }

        LogLifetime(const LogLifetime&) = delete;
        LogLifetime& operator=(const LogLifetime&) = delete;
    };

    [[nodiscard]] bool ParseUnsigned(std::string_view text, RVX::uint32& value)
    {
        const char* const first = text.data();
        const char* const last = text.data() + text.size();
        const auto parsed = std::from_chars(first, last, value);
        return parsed.ec == std::errc{} && parsed.ptr == last;
    }

    [[nodiscard]] std::optional<RVX::RHIBackendType> ParseBackend(
        std::string_view value)
    {
        if (value == "DX12")
            return RVX::RHIBackendType::DX12;
        if (value == "Vulkan")
            return RVX::RHIBackendType::Vulkan;
        if (value == "Metal")
            return RVX::RHIBackendType::Metal;
        return std::nullopt;
    }

    [[nodiscard]] bool EnvironmentFlagEnabled(const char* name)
    {
#if defined(_WIN32)
        char* allocatedValue = nullptr;
        size_t valueLength = 0;
        if (_dupenv_s(&allocatedValue, &valueLength, name) != 0 ||
            allocatedValue == nullptr)
        {
            return false;
        }
        const std::string text = allocatedValue;
        std::free(allocatedValue);
#else
        const char* const value = std::getenv(name);
        if (value == nullptr)
            return false;
        const std::string_view text = value;
#endif
        return text == "1" || text == "true" || text == "TRUE" ||
               text == "on" || text == "ON";
    }

    [[nodiscard]] bool IsSoftwareAdapterName(std::string adapterName)
    {
        for (char& character : adapterName)
        {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));
        }
        constexpr const char* softwareTokens[] = {
            "software",
            "warp",
            "swiftshader",
            "lavapipe",
            "llvmpipe",
            "microsoft basic",
        };
        for (const char* token : softwareTokens)
        {
            if (adapterName.find(token) != std::string::npos)
                return true;
        }
        return false;
    }

    [[nodiscard]] const char* BackendName(RVX::RHIBackendType backend)
    {
        switch (backend)
        {
            case RVX::RHIBackendType::DX12: return "DX12";
            case RVX::RHIBackendType::Vulkan: return "Vulkan";
            case RVX::RHIBackendType::Metal: return "Metal";
            case RVX::RHIBackendType::DX11: return "DX11";
            case RVX::RHIBackendType::OpenGL: return "OpenGL";
            default: return "None";
        }
    }

    [[nodiscard]] bool ReadValue(int argc,
                                 char* argv[],
                                 int& index,
                                 std::string_view option,
                                 std::string& value,
                                 std::string& error)
    {
        if (index + 1 >= argc)
        {
            error = std::string(option) + " requires a value";
            return false;
        }
        value = argv[++index];
        if (value.empty())
        {
            error = std::string(option) + " requires a non-empty value";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ParseOptions(int argc,
                                    char* argv[],
                                    Options& options,
                                    std::string& error)
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view option = argv[index];
            std::string value;
            if (!ReadValue(argc, argv, index, option, value, error))
                return false;

            if (option == "--backend")
            {
                const auto backend = ParseBackend(value);
                if (!backend.has_value())
                {
                    error = "--backend must be DX12, Vulkan, or Metal";
                    return false;
                }
                options.backend = *backend;
                options.backendName = std::move(value);
            }
            else if (option == "--cooked-root")
                options.cookedRoot = std::move(value);
            else if (option == "--shader-root")
                options.shaderRoot = std::move(value);
            else if (option == "--model")
                options.model = std::move(value);
            else if (option == "--fixture-receipt")
                options.fixtureReceipt = std::move(value);
            else if (option == "--commit")
                options.commit = std::move(value);
            else if (option == "--preset")
                options.preset = std::move(value);
            else if (option == "--frames")
            {
                if (!ParseUnsigned(value, options.frames))
                {
                    error = "--frames must be an unsigned integer";
                    return false;
                }
            }
            else if (option == "--width")
            {
                if (!ParseUnsigned(value, options.width))
                {
                    error = "--width must be an unsigned integer";
                    return false;
                }
            }
            else if (option == "--height")
            {
                if (!ParseUnsigned(value, options.height))
                {
                    error = "--height must be an unsigned integer";
                    return false;
                }
            }
            else if (option == "--report")
                options.report = std::move(value);
            else
            {
                error = "Unknown option: " + std::string(option);
                return false;
            }
        }

        if (options.backend == RVX::RHIBackendType::None ||
            options.cookedRoot.empty() || options.shaderRoot.empty() ||
            options.model.empty() || options.fixtureReceipt.empty() ||
            options.commit.empty() || options.preset.empty() ||
            options.report.empty())
        {
            error = "All Shipping smoke options are required";
            return false;
        }
        if (options.frames != RequiredFrames || options.width != RequiredWidth ||
            options.height != RequiredHeight)
        {
            error = "Shipping smoke is fixed to 8 frames at 320x180";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ReadFile(const std::filesystem::path& path,
                                std::string& contents,
                                std::string& error)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            error = "Could not open file: " + path.generic_string();
            return false;
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        if (!stream.good() && !stream.eof())
        {
            error = "Could not read file: " + path.generic_string();
            return false;
        }
        contents = buffer.str();
        return true;
    }

    [[nodiscard]] std::optional<std::string> ExtractJsonString(
        std::string_view json,
        std::string_view field)
    {
        const std::string token = "\"" + std::string(field) + "\"";
        size_t position = json.find(token);
        if (position == std::string_view::npos)
            return std::nullopt;
        position = json.find(':', position + token.size());
        if (position == std::string_view::npos)
            return std::nullopt;
        position = json.find('"', position + 1);
        if (position == std::string_view::npos)
            return std::nullopt;
        const size_t end = json.find('"', position + 1);
        if (end == std::string_view::npos)
            return std::nullopt;
        return std::string(json.substr(position + 1, end - position - 1));
    }

    [[nodiscard]] std::string JsonEscape(std::string_view value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (const char character : value)
        {
            switch (character)
            {
                case '\\': escaped += "\\\\"; break;
                case '"': escaped += "\\\""; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default: escaped += character; break;
            }
        }
        return escaped;
    }

    [[nodiscard]] bool WriteEvidence(
        const Options& options,
        std::string_view actualBackend,
        std::string_view adapterName,
        bool softwareAdapterAllowed,
        bool softwareAdapter,
        bool cookedLoadSucceeded,
        bool sourceLoadRejected,
        bool shutdownClean,
        RVX::uint32 verifiedFrames,
        std::string_view receiptSha256,
        std::string_view artifactPayloadHash,
        std::string& error)
    {
        std::error_code directoryError;
        const std::filesystem::path parent = options.report.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, directoryError);
        if (directoryError)
        {
            error = "Could not create report directory: " + directoryError.message();
            return false;
        }

        std::ofstream stream(options.report, std::ios::trunc);
        if (!stream)
        {
            error = "Could not open Shipping report: " +
                    options.report.generic_string();
            return false;
        }

        const bool fallbackUsed = actualBackend != options.backendName;
        stream << "{\n"
               << "  \"schema\": \"RVX_SHIPPING_EVIDENCE\",\n"
               << "  \"schemaVersion\": 2,\n"
               << "  \"commit\": \"" << JsonEscape(options.commit) << "\",\n"
               << "  \"preset\": \"" << JsonEscape(options.preset) << "\",\n"
               << "  \"frames\": " << verifiedFrames << ",\n"
               << "  \"width\": " << options.width << ",\n"
               << "  \"height\": " << options.height << ",\n"
               << "  \"quality\": \"Low\",\n"
               << "  \"renderPath\": \"Direct\",\n"
               << "  \"requestedBackend\": \""
               << JsonEscape(options.backendName) << "\",\n"
               << "  \"actualBackend\": \""
               << JsonEscape(actualBackend) << "\",\n"
               << "  \"fallbackUsed\": "
               << (fallbackUsed ? "true" : "false") << ",\n"
               << "  \"fallbackReason\": \"\",\n"
               << "  \"softwareAdapterAllowed\": "
               << (softwareAdapterAllowed ? "true" : "false") << ",\n"
               << "  \"softwareAdapter\": "
               << (softwareAdapter ? "true" : "false") << ",\n"
               << "  \"adapterName\": \""
               << JsonEscape(adapterName) << "\",\n"
               << "  \"cookedLoadSucceeded\": "
               << (cookedLoadSucceeded ? "true" : "false") << ",\n"
               << "  \"sourceLoadRejected\": "
               << (sourceLoadRejected ? "true" : "false") << ",\n"
               << "  \"shutdownClean\": "
               << (shutdownClean ? "true" : "false") << ",\n"
               << "  \"fixtureReceiptSha256\": \""
               << JsonEscape(receiptSha256) << "\",\n"
               << "  \"fixtureArtifactPayloadHash\": \""
               << JsonEscape(artifactPayloadHash) << "\"\n"
               << "}\n";
        if (!stream.good())
        {
            error = "Could not write Shipping report: " +
                    options.report.generic_string();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool WaitForPresentation(RVX::RenderSubsystem& render,
                                           RVX::uint64 minimumSequence)
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (render.GetDiagnosticsSnapshot().lastPresentedFrameSequence >=
                minimumSequence)
            {
                return true;
            }
            static_cast<void>(render.RequestCompletionPoll());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    [[nodiscard]] bool DrainEngine(RVX::Engine& engine,
                                   const std::string& worldName)
    {
        engine.DestroyWorld(worldName);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(30);
        while (engine.GetWorld(worldName) != nullptr &&
               std::chrono::steady_clock::now() < deadline)
        {
            engine.Tick(FixedDeltaSeconds);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (engine.GetWorld(worldName) != nullptr)
            return false;

        do
        {
            engine.Shutdown();
            if (engine.IsInitialized())
            {
                engine.Tick(FixedDeltaSeconds);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } while (engine.IsInitialized() &&
                 std::chrono::steady_clock::now() < deadline);
        return !engine.IsInitialized() &&
               engine.GetLastShutdownDiagnostics().available &&
               engine.GetLastShutdownDiagnostics().clean;
    }

    [[nodiscard]] int Run(const Options& options, std::string& error)
    {
        std::string receiptContents;
        if (!ReadFile(options.fixtureReceipt, receiptContents, error))
            return 1;
        RVX::Hash::SHA256Hasher receiptHasher;
        receiptHasher.Update(receiptContents);
        const std::string receiptSha256 = receiptHasher.FinalizeHex();
        const auto artifactPayloadHash =
            ExtractJsonString(receiptContents, "artifactPayloadHash");
        if (!artifactPayloadHash.has_value() || artifactPayloadHash->size() != 64)
        {
            error = "Shipping fixture receipt has no valid artifactPayloadHash";
            return 1;
        }

        LogLifetime logLifetime;
        RVX::Engine engine(RVX::CreateShippingEngineRuntimeDependencies(
            options.shaderRoot));
        RVX::EngineConfig engineConfig;
        constexpr const char* ApplicationName = "RenderVerseX Shipping Smoke";
        engineConfig.appName = ApplicationName;
        engineConfig.windowWidth = options.width;
        engineConfig.windowHeight = options.height;
        engineConfig.enableJobSystem = true;
        engineConfig.renderRuntime.backendType = options.backend;
        engineConfig.renderRuntime.swapChain.vsync = false;
        engineConfig.renderRuntime.enableValidation = false;
        engineConfig.renderRuntime.enableGPUValidation = false;
        const bool softwareAdapterAllowed =
            EnvironmentFlagEnabled("RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER");
        engineConfig.renderRuntime.allowSoftwareAdapter = softwareAdapterAllowed;
        engineConfig.initialRenderFrameSettings.gpuCulling.mode =
            RVX::RenderGPUDrivenMode::ForceDisabled;
        engineConfig.initialRenderFrameSettings.shadows.enabled = false;
        engineConfig.initialRenderFrameSettings.rayTracing.enabled = false;
        engineConfig.resourceManager =
            RVX::CreateShippingResourceManagerConfig(options.cookedRoot);
        engine.SetConfig(engineConfig);

        auto* window = engine.AddSubsystem<RVX::WindowSubsystem>();
        RVX::WindowConfig windowConfig;
        windowConfig.title = ApplicationName;
        windowConfig.width = options.width;
        windowConfig.height = options.height;
        windowConfig.resizable = false;
        windowConfig.graphicsApi = RVX::WindowGraphicsApi::None;
        window->SetConfig(windowConfig);
        auto* resources =
            engine.AddSubsystem<RVX::Resource::ResourceSubsystem>();
        auto* render = engine.AddSubsystem<RVX::RenderSubsystem>();

        if (!engine.Initialize() || !engine.IsInitialized())
        {
            error = "Shipping Engine initialization failed";
            return 1;
        }

        RVX::World* world = engine.CreateWorld("ShippingSmoke");
        if (world == nullptr)
        {
            error = "Shipping World creation failed";
            engine.Shutdown();
            return 1;
        }
        engine.SetActiveWorld(world);
        auto& cameras = world->GetCameraService();
        const RVX::WorldECS::WorldEcsCameraRef camera =
            cameras.CreateMainCamera();
        if (!camera.IsValid() ||
            !cameras.SetPerspective(camera,
                                    RVX::radians(60.0f),
                                    static_cast<float>(options.width) /
                                        static_cast<float>(options.height),
                                    0.1f,
                                    100.0f) ||
            !cameras.SetPose(camera,
                             {{0.0f, 0.0f, 3.0f},
                              {1.0f, 0.0f, 0.0f, 0.0f}}) ||
            !cameras.LookAt(camera, {0.0f, 0.0f, 0.0f}))
        {
            error = "Shipping camera setup failed";
            static_cast<void>(DrainEngine(engine, "ShippingSmoke"));
            return 1;
        }

        RVX::Resource::ResourceHandle<RVX::Resource::ModelResource> model =
            resources->Load<RVX::Resource::ModelResource>(options.model);
        if (model && model.IsLoading())
            static_cast<void>(model.TryWaitForLoad(30000));
        const RVX::Resource::ResourceLoadDiagnostic cookedDiagnostic =
            resources->GetManager().GetLastLoadDiagnostic();
        const bool cookedLoadSucceeded =
            model && model.IsLoaded() && cookedDiagnostic.success &&
            cookedDiagnostic.domain ==
                RVX::Resource::ResourceLoadDomain::CookedArtifact &&
            cookedDiagnostic.cookedArtifactRead &&
            !cookedDiagnostic.sourceAssetRead;

        RVX::Resource::ResourceHandle<RVX::Resource::ModelResource> sourceProbe =
            resources->Load<RVX::Resource::ModelResource>(
                "source://R7Triangle.gltf");
        const RVX::Resource::ResourceLoadDiagnostic sourceDiagnostic =
            resources->GetManager().GetLastLoadDiagnostic();
        const bool sourceLoadRejected =
            !sourceProbe && sourceDiagnostic.attempted &&
            !sourceDiagnostic.success && sourceDiagnostic.sourceAssetRead &&
            sourceDiagnostic.domain ==
                RVX::Resource::ResourceLoadDomain::SourceAsset &&
            (sourceDiagnostic.failure ==
                 RVX::Resource::ResourceLoadFailureCode::CookedArtifactRequired ||
             sourceDiagnostic.failure ==
                 RVX::Resource::ResourceLoadFailureCode::SourceAssetsDisabled);
        if (!cookedLoadSucceeded || !sourceLoadRejected)
        {
            error = "Shipping cooked-only Resource policy proof failed";
            sourceProbe.Reset();
            model.Reset();
            static_cast<void>(DrainEngine(engine, "ShippingSmoke"));
            return 1;
        }

        RVX::EngineRenderRuntimeDiagnostics engineDiagnostics =
            engine.GetRenderRuntimeDiagnostics();
        if (!engineDiagnostics.available)
        {
            error = "Shipping Render diagnostics are unavailable before the frame proof";
            sourceProbe.Reset();
            model.Reset();
            static_cast<void>(DrainEngine(engine, "ShippingSmoke"));
            return 1;
        }

        const RVX::uint64 acceptedBaseline = engineDiagnostics
            .acceptedExtractionDiagnostics.acceptedPublicationCount;
        RVX::uint64 priorRequiredSequence =
            engineDiagnostics.requiredSceneFrameSequence;
        RVX::uint64 priorPresentedSequence =
            render->GetDiagnosticsSnapshot().lastPresentedFrameSequence;
        RVX::uint32 verifiedFrames = 0;
        for (RVX::uint32 frame = 0; frame < options.frames; ++frame)
        {
            engine.Tick(FixedDeltaSeconds);
            engineDiagnostics = engine.GetRenderRuntimeDiagnostics();
            const RVX::uint64 expectedAcceptedCount =
                acceptedBaseline + static_cast<RVX::uint64>(frame) + 1;
            if (!engineDiagnostics.available ||
                engineDiagnostics.acceptedExtractionDiagnostics
                        .acceptedPublicationCount != expectedAcceptedCount ||
                engineDiagnostics.requiredSceneFrameSequence <=
                    priorRequiredSequence ||
                !WaitForPresentation(
                    *render, engineDiagnostics.requiredSceneFrameSequence))
            {
                error = "Shipping Render did not accept and present each of the eight ECS frames";
                sourceProbe.Reset();
                model.Reset();
                static_cast<void>(DrainEngine(engine, "ShippingSmoke"));
                return 1;
            }
            const RVX::RenderDiagnosticsSnapshot frameDiagnostics =
                render->GetDiagnosticsSnapshot();
            if (frameDiagnostics.lastPresentedFrameSequence <=
                    priorPresentedSequence ||
                frameDiagnostics.lastPresentedFrameSequence <
                    engineDiagnostics.requiredSceneFrameSequence)
            {
                error = "Shipping Render presentation sequence did not advance exactly with the frame proof";
                sourceProbe.Reset();
                model.Reset();
                static_cast<void>(DrainEngine(engine, "ShippingSmoke"));
                return 1;
            }
            priorRequiredSequence =
                engineDiagnostics.requiredSceneFrameSequence;
            priorPresentedSequence =
                frameDiagnostics.lastPresentedFrameSequence;
            ++verifiedFrames;
        }

        const RVX::RenderDiagnosticsSnapshot renderDiagnostics =
            render->GetDiagnosticsSnapshot();
        const std::string actualBackend = BackendName(renderDiagnostics.backend);
        const bool softwareAdapter =
            IsSoftwareAdapterName(renderDiagnostics.adapterName);
        sourceProbe.Reset();
        model.Reset();
        const bool shutdownClean = DrainEngine(engine, "ShippingSmoke");
        if (!shutdownClean)
        {
            error = "Shipping Engine shutdown was not clean";
            return 1;
        }
        if (!WriteEvidence(options,
                           actualBackend,
                           renderDiagnostics.adapterName,
                           softwareAdapterAllowed,
                           softwareAdapter,
                           cookedLoadSucceeded,
                           sourceLoadRejected,
                           shutdownClean,
                           verifiedFrames,
                           receiptSha256,
                           *artifactPayloadHash,
                           error))
        {
            return 1;
        }
        if (actualBackend != options.backendName)
        {
            error = "Shipping backend fallback is forbidden";
            return 1;
        }
        if (softwareAdapter && !softwareAdapterAllowed)
        {
            error = "Shipping software adapter use requires explicit opt-in";
            return 1;
        }
        return 0;
    }
} // namespace

int main(int argc, char* argv[])
{
    Options options;
    std::string error;
    if (!ParseOptions(argc, argv, options, error))
    {
        std::cerr << "RVXShippingConsumer: " << error << '\n';
        return 2;
    }

    try
    {
        const int result = Run(options, error);
        if (result != 0)
            std::cerr << "RVXShippingConsumer: " << error << '\n';
        return result;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "RVXShippingConsumer: " << exception.what() << '\n';
    }
    catch (...)
    {
        std::cerr << "RVXShippingConsumer: unknown failure\n";
    }
    return 1;
}
