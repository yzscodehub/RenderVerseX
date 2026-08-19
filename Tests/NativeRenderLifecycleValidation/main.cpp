/**
 * @file main.cpp
 * @brief Thin real-backend lifecycle smoke for the M1 render runtime.
 */

#include "Common/GpuTestUtils.h"
#include "Render/RenderSubsystem.h"
#include "RenderContracts/RenderFramePacketV5.h"
#include "RenderContracts/RenderSceneUpdate.h"

#if defined(RVX_NATIVE_BACKEND_VULKAN)
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>

#if defined(RVX_NATIVE_BACKEND_DX12)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#elif defined(RVX_NATIVE_BACKEND_METAL)
#include "Apple/GLFWMetalLayerBridge.h"
#endif

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

namespace RVX
{
namespace
{
    using namespace std::chrono_literals;

    [[nodiscard]] constexpr RHIBackendType RequiredBackend() noexcept
    {
#if defined(RVX_NATIVE_BACKEND_DX12)
        return RHIBackendType::DX12;
#elif defined(RVX_NATIVE_BACKEND_VULKAN)
        return RHIBackendType::Vulkan;
#elif defined(RVX_NATIVE_BACKEND_METAL)
        return RHIBackendType::Metal;
#else
#error NativeRenderLifecycleValidation requires a platform backend definition
#endif
    }

    [[nodiscard]] std::string ReadEnvironmentVariable(const char* name)
    {
#if defined(_WIN32)
        char* value = nullptr;
        size_t length = 0;
        if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        {
            return {};
        }
        std::string result(value);
        std::free(value);
        return result;
#else
        const char* value = std::getenv(name);
        return value != nullptr ? std::string(value) : std::string{};
#endif
    }

    [[nodiscard]] bool IsEnabledEnvironmentFlag(const char* name)
    {
        const std::string value = ReadEnvironmentVariable(name);
        return value == "1" || value == "true" || value == "TRUE";
    }

    class HiddenNativeWindow final
    {
    public:
        HiddenNativeWindow() = default;

        ~HiddenNativeWindow()
        {
            Shutdown();
        }

        HiddenNativeWindow(const HiddenNativeWindow&) = delete;
        HiddenNativeWindow& operator=(const HiddenNativeWindow&) = delete;

        [[nodiscard]] bool Initialize(uint32 width, uint32 height)
        {
#if defined(RVX_NATIVE_BACKEND_VULKAN)
            // The CI loader is supplied by vcpkg and may not be discoverable
            // under the host's default dynamic-library search paths. GLFW
            // must receive the linked loader before its first initialization.
            glfwInitVulkanLoader(vkGetInstanceProcAddr);
#endif
            if (glfwInit() != GLFW_TRUE)
            {
                return false;
            }
            m_glfwInitialized = true;
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
            m_window = glfwCreateWindow(
                static_cast<int>(width),
                static_cast<int>(height),
                "RenderVerseX M1 Native Lifecycle",
                nullptr,
                nullptr);
            return m_window != nullptr;
        }

        [[nodiscard]] NativeSurfaceDesc Capture(uint64 generation)
        {
            NativeSurfaceDesc surface;
            if (m_window == nullptr)
            {
                return surface;
            }

            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(m_window, &width, &height);
            surface.width = static_cast<uint32>(width);
            surface.height = static_cast<uint32>(height);
            surface.contentScale = 1.0f;
            surface.preferredFormat = RHIFormat::BGRA8_UNORM;
            surface.vsync = false;
            surface.generation = generation;
            surface.backendWindow =
                reinterpret_cast<uintptr_t>(m_window);

#if defined(RVX_NATIVE_BACKEND_DX12)
            surface.platform = NativeSurfacePlatform::Win32;
            surface.nativeWindow = reinterpret_cast<uintptr_t>(
                glfwGetWin32Window(m_window));
#elif defined(RVX_NATIVE_BACKEND_VULKAN)
            surface.platform = NativeSurfacePlatform::GLFW;
#elif defined(RVX_NATIVE_BACKEND_METAL)
            surface.platform = NativeSurfacePlatform::Cocoa;
            if (m_nativeLayer == 0U)
            {
                m_nativeLayer = HAL::AttachGLFWMetalLayer(
                    m_window, m_nativeWindow);
            }
            surface.nativeWindow = m_nativeWindow;
            surface.nativeLayer = m_nativeLayer;
#endif
            return surface;
        }

        void Resize(uint32 width, uint32 height)
        {
            glfwSetWindowSize(m_window,
                              static_cast<int>(width),
                              static_cast<int>(height));
            glfwPollEvents();
        }

        void PollEvents()
        {
            glfwPollEvents();
        }

    private:
        void Shutdown()
        {
#if defined(RVX_NATIVE_BACKEND_METAL)
            if (m_window != nullptr && m_nativeLayer != 0U)
            {
                HAL::DetachGLFWMetalLayer(m_window);
                m_nativeLayer = 0;
                m_nativeWindow = 0;
            }
#endif
            if (m_window != nullptr)
            {
                glfwDestroyWindow(m_window);
                m_window = nullptr;
            }
            if (m_glfwInitialized)
            {
                glfwTerminate();
                m_glfwInitialized = false;
            }
        }

        GLFWwindow* m_window = nullptr;
        bool m_glfwInitialized = false;
#if defined(RVX_NATIVE_BACKEND_METAL)
        uintptr_t m_nativeWindow = 0;
        uintptr_t m_nativeLayer = 0;
#endif
    };

    struct NativeFrameSet
    {
        std::unique_ptr<const RenderSceneUpdateBatch> sceneUpdate;
        std::unique_ptr<const RenderFramePacketV5> frame;

        [[nodiscard]] bool IsComplete() const noexcept
        {
            return sceneUpdate != nullptr && frame != nullptr;
        }
    };

    [[nodiscard]] NativeFrameSet MakePacket(
        uint64 sequence,
        uint32 width,
        uint32 height)
    {
        RenderSceneMutationAccumulator accumulator;
        accumulator.Begin(0, true);
        accumulator.UpsertSky(RenderSkySnapshot{});
        accumulator.UpsertEnvironment(RenderEnvironmentSnapshot{});

        RenderFrameHeaderV5 header;
        header.sequence = sequence;
        header.requiredSceneRevision = sequence;
        RenderViewSnapshot view;
        view.viewportWidth = width;
        view.viewportHeight = height;
        RenderExtractionDiagnostics extraction;
        extraction.complete = true;
        NativeFrameSet result;
        result.sceneUpdate =
            std::make_unique<const RenderSceneUpdateBatch>(
                accumulator.Build(sequence));
        result.frame = RenderFramePacketV5::Create(
            header,
            view,
            RenderFrameSettings{},
            RenderFrameCaptureRequest{},
            extraction);
        return result;
    }

    template <typename Predicate>
    [[nodiscard]] bool WaitUntil(HiddenNativeWindow& window,
                                 Predicate&& predicate,
                                 std::chrono::seconds timeout = 30s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (std::invoke(predicate))
            {
                return true;
            }
            window.PollEvents();
            std::this_thread::sleep_for(1ms);
        }
        return std::invoke(predicate);
    }

    [[nodiscard]] std::string JsonEscape(const std::string& value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (const char character : value)
        {
            switch (character)
            {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '"':
                    escaped += "\\\"";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    escaped += character;
                    break;
            }
        }
        return escaped;
    }

    [[nodiscard]] bool WriteEvidence(
        const RenderDiagnosticsSnapshot& diagnostics,
        const RenderShutdownResult& shutdown,
        uint64 mainThreadIdentityHash,
        uint64 startupDurationMs,
        uint64 shutdownDurationMs)
    {
        const std::string reportPath =
            ReadEnvironmentVariable("RVX_NATIVE_LIFECYCLE_REPORT");
        if (reportPath.empty())
        {
            return true;
        }

        const std::filesystem::path path(reportPath);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            return false;
        }

        std::ofstream stream(path, std::ios::trunc);
        if (!stream)
        {
            return false;
        }
        stream << "{\n"
               << "  \"schema\": \"RVX.M1.NativeRenderLifecycle\",\n"
               << "  \"schemaVersion\": 1,\n"
               << "  \"status\": \"passed\",\n"
               << "  \"requiredBackend\": \""
               << JsonEscape(RVX_NATIVE_REQUIRED_BACKEND) << "\",\n"
               << "  \"adapterName\": \""
               << JsonEscape(diagnostics.adapterName) << "\",\n"
               << "  \"driverVersion\": \""
               << JsonEscape(diagnostics.driverVersion) << "\",\n"
               << "  \"mainThreadIdentityHash\": "
               << mainThreadIdentityHash << ",\n"
               << "  \"renderThreadIdentityHash\": "
               << diagnostics.renderThreadIdentityHash << ",\n"
               << "  \"surfaceGeneration\": "
               << diagnostics.surfaceGeneration << ",\n"
               << "  \"lastPresentedFrameSequence\": "
               << diagnostics.lastPresentedFrameSequence << ",\n"
               << "  \"resizeAcceptedCount\": "
               << diagnostics.resizeAcceptedCount << ",\n"
               << "  \"shutdownCode\": "
               << static_cast<uint32>(shutdown.code) << ",\n"
               << "  \"startupDurationMs\": " << startupDurationMs << ",\n"
               << "  \"shutdownDurationMs\": " << shutdownDurationMs << "\n"
               << "}\n";
        return stream.good();
    }

    TEST(NativeRenderLifecycleValidation,
         RequiredBackendPresentsResizesAndStops)
    {
        constexpr uint32 initialWidth = 80;
        constexpr uint32 initialHeight = 64;
        constexpr uint32 resizedWidth = 96;
        constexpr uint32 resizedHeight = 72;
        const uint64 mainThreadIdentityHash =
            static_cast<uint64>(
                std::hash<std::thread::id>{}(std::this_thread::get_id()));

        HiddenNativeWindow window;
        ASSERT_TRUE(window.Initialize(initialWidth, initialHeight));
        const NativeSurfaceDesc initialSurface = window.Capture(1);
        ASSERT_TRUE(initialSurface.IsValidFor(RequiredBackend()));

        RenderRuntimeConfig config;
        config.backendType = RequiredBackend();
        config.enableValidation = false;
        config.enableGPUValidation = false;
        config.allowSoftwareAdapter =
            IsEnabledEnvironmentFlag(
                "RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER");

        RenderSubsystem render;
        render.Configure(config, initialSurface);
        const auto startupBegin = std::chrono::steady_clock::now();
        ASSERT_NO_THROW(render.Initialize());
        const uint64 startupDurationMs = static_cast<uint64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startupBegin)
                .count());
        ASSERT_TRUE(render.IsReady());

        NativeFrameSet firstPacket =
            MakePacket(1, initialSurface.width, initialSurface.height);
        ASSERT_TRUE(firstPacket.IsComplete());
        ASSERT_EQ(render.TryPublishFrameSet(
                      std::move(firstPacket.sceneUpdate),
                      std::move(firstPacket.frame)).code,
                  RenderFramePublishCode::Accepted);
        ASSERT_TRUE(WaitUntil(window, [&]() {
            return render.GetDiagnosticsSnapshot()
                       .lastPresentedFrameSequence >= 1U;
        }));

        window.Resize(resizedWidth, resizedHeight);
        NativeSurfaceDesc resizedSurface;
        ASSERT_TRUE(WaitUntil(window, [&]() {
            resizedSurface = window.Capture(2);
            return resizedSurface.width > 0U &&
                   resizedSurface.height > 0U &&
                   (resizedSurface.width != initialSurface.width ||
                    resizedSurface.height != initialSurface.height);
        }));
        ASSERT_TRUE(resizedSurface.IsValidFor(RequiredBackend()));
        ASSERT_EQ(ClassifyNativeSurfaceUpdate(initialSurface, resizedSurface),
                  NativeSurfaceUpdateKind::Resize);
        const RenderResizeResult resize =
            render.RequestResize(resizedSurface);
        ASSERT_TRUE(resize.code == RenderResizeCode::Accepted ||
                    resize.code == RenderResizeCode::CoalescedOlder);
        ASSERT_TRUE(WaitUntil(window, [&]() {
            return render.GetDiagnosticsSnapshot().surfaceGeneration == 2U;
        }));

        NativeFrameSet secondPacket =
            MakePacket(2, resizedSurface.width, resizedSurface.height);
        ASSERT_TRUE(secondPacket.IsComplete());
        ASSERT_EQ(render.TryPublishFrameSet(
                      std::move(secondPacket.sceneUpdate),
                      std::move(secondPacket.frame)).code,
                  RenderFramePublishCode::Accepted);
        ASSERT_TRUE(WaitUntil(window, [&]() {
            return render.GetDiagnosticsSnapshot()
                       .lastPresentedFrameSequence >= 2U;
        }));

        const auto shutdownBegin = std::chrono::steady_clock::now();
        render.Deinitialize();
        const uint64 shutdownDurationMs = static_cast<uint64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - shutdownBegin)
                .count());
        const RenderShutdownResult shutdown =
            render.GetLastShutdownResult();
        const RenderDiagnosticsSnapshot diagnostics =
            render.GetDiagnosticsSnapshot();

        EXPECT_EQ(shutdown.code, RenderShutdownCode::Completed);
        EXPECT_EQ(diagnostics.lifecycle, RenderLifecycleState::Stopped);
        EXPECT_EQ(diagnostics.backend, RequiredBackend());
        EXPECT_FALSE(diagnostics.adapterName.empty());
        EXPECT_NE(diagnostics.renderThreadIdentityHash, 0U);
        EXPECT_NE(diagnostics.renderThreadIdentityHash,
                  mainThreadIdentityHash);
        EXPECT_EQ(diagnostics.surfaceGeneration, 2U);
        EXPECT_GE(diagnostics.resizeAcceptedCount, 1U);
        EXPECT_EQ(diagnostics.lastPresentedFrameSequence, 2U);
        ASSERT_FALSE(::testing::Test::HasFailure());
        ASSERT_TRUE(WriteEvidence(diagnostics,
                                  shutdown,
                                  mainThreadIdentityHash,
                                  startupDurationMs,
                                  shutdownDurationMs));
    }
} // namespace
} // namespace RVX
