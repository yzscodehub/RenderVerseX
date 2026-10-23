#include "Common/GpuTestUtils.h"
#include "DX12CommandContext.h"
#include "DX12Device.h"
#include "DX12Resources.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

using namespace RVX;

namespace
{
    class ScopedEnvironmentVariable final
    {
    public:
        ScopedEnvironmentVariable(std::string name, std::string value)
            : m_name(std::move(name))
        {
            char* existing = nullptr;
            size_t existingLength = 0;
            if (_dupenv_s(&existing, &existingLength, m_name.c_str()) == 0 &&
                existing != nullptr)
            {
                m_originalValue = existing;
            }
            std::free(existing);

            m_setSucceeded = _putenv_s(m_name.c_str(), value.c_str()) == 0;
        }

        ~ScopedEnvironmentVariable()
        {
            if (!m_setSucceeded)
            {
                return;
            }

            const char* restoredValue =
                m_originalValue ? m_originalValue->c_str() : "";
            (void)_putenv_s(m_name.c_str(), restoredValue);
        }

        [[nodiscard]] bool IsSet() const noexcept
        {
            return m_setSucceeded;
        }

    private:
        std::string m_name;
        std::optional<std::string> m_originalValue;
        bool m_setSucceeded = false;
    };

    class ScopedReportPath final
    {
    public:
        ScopedReportPath()
            : m_path(CreateUniquePath())
            , m_environment("RVX_DX12_DEBUG_QUEUE_REPORT_PATH",
                            m_path.string())
        {
            std::error_code error;
            std::filesystem::remove(m_path, error);
        }

        ~ScopedReportPath()
        {
            std::error_code error;
            std::filesystem::remove(m_path, error);
        }

        [[nodiscard]] bool IsSet() const noexcept
        {
            return m_environment.IsSet();
        }

        [[nodiscard]] const std::filesystem::path& GetPath() const noexcept
        {
            return m_path;
        }

    private:
        static std::filesystem::path CreateUniquePath()
        {
            static std::atomic<uint64> s_nextId = 0;
            const uint64 id = s_nextId.fetch_add(1, std::memory_order_relaxed);
            return std::filesystem::temp_directory_path() /
                ("rvx-dx12-native-diagnostics-" +
                 std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(id) + ".json");
        }

        std::filesystem::path m_path;
        ScopedEnvironmentVariable m_environment;
    };

    [[nodiscard]] std::string ReadReportText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        EXPECT_TRUE(static_cast<bool>(input)) << "DX12 native diagnostics report was not written: "
                                              << path.string();
        if (!input)
        {
            return {};
        }

        return {std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
    }

    [[nodiscard]] nlohmann::json ParseReport(const std::string& reportText)
    {
        const nlohmann::json report =
            nlohmann::json::parse(reportText, nullptr, false, true);
        EXPECT_FALSE(report.is_discarded()) << "DX12 native diagnostics report is not valid JSON";
        return report;
    }

    [[nodiscard]] ComPtr<ID3D12InfoQueue> RequireInfoQueue(DX12Device& device)
    {
        ComPtr<ID3D12InfoQueue> infoQueue;
        EXPECT_NE(device.GetD3DDevice(), nullptr);
        if (device.GetD3DDevice() == nullptr ||
            FAILED(device.GetD3DDevice()->QueryInterface(IID_PPV_ARGS(&infoQueue))))
        {
            ADD_FAILURE() << "DX12 debug layer did not expose ID3D12InfoQueue";
        }
        return infoQueue;
    }

    [[nodiscard]] std::unique_ptr<IRHIDevice> CreateDebugDevice()
    {
        RHIDeviceDesc deviceDesc;
        deviceDesc.enableDebugLayer = true;
        return RVX::Test::CreateNativeValidationDevice(RHIBackendType::DX12, deviceDesc);
    }

    void ClearForInjectedMessage(ID3D12InfoQueue& infoQueue)
    {
        infoQueue.ClearStoredMessages();
        ASSERT_EQ(infoQueue.GetNumStoredMessages(), 0u);
        ASSERT_EQ(infoQueue.GetNumStoredMessagesAllowedByRetrievalFilter(), 0u);
    }

    [[nodiscard]] const nlohmann::json* FindMessage(
        const nlohmann::json& report,
        const std::string& description)
    {
        if (!report.contains("messages") || !report.at("messages").is_array())
        {
            return nullptr;
        }

        for (const nlohmann::json& message : report.at("messages"))
        {
            if (message.value("description", "") == description)
            {
                return &message;
            }
        }

        return nullptr;
    }
} // namespace

TEST(DX12Validation, NativeDiagnosticsExitReportEscapesAndCountsInjectedWarning)
{
    ScopedReportPath reportPath;
    ASSERT_TRUE(reportPath.IsSet());

    auto device = CreateDebugDevice();
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);
    auto* dx12Device = dynamic_cast<DX12Device*>(device.get());
    ASSERT_NE(dx12Device, nullptr);

    ComPtr<ID3D12InfoQueue> infoQueue = RequireInfoQueue(*dx12Device);
    ASSERT_NE(infoQueue, nullptr);
    ClearForInjectedMessage(*infoQueue.Get());

    const std::string description =
        "RVX diagnostic \"quoted\"\nline\\backslash";
    ASSERT_TRUE(SUCCEEDED(infoQueue->AddApplicationMessage(
        D3D12_MESSAGE_SEVERITY_WARNING,
        description.c_str())));

    const RHINativeValidationDiagnostics diagnostics =
        device->GetNativeValidationDiagnostics();
    EXPECT_TRUE(diagnostics.enabled);
    EXPECT_TRUE(diagnostics.available);
    EXPECT_TRUE(diagnostics.readComplete);
    EXPECT_EQ(diagnostics.messageCount, 1u);
    EXPECT_EQ(diagnostics.warningCount, 1u);
    EXPECT_EQ(diagnostics.errorCount, 0u);
    EXPECT_EQ(diagnostics.corruptionCount, 0u);

    const uint64 expectedDeniedByStorageFilter =
        infoQueue->GetNumMessagesDeniedByStorageFilter();
    infoQueue.Reset();
    device.reset();

    const std::string reportText = ReadReportText(reportPath.GetPath());
    // Keep a raw literal outside GTest's stringifying macros for MSVC's
    // traditional preprocessor. Parsed JSON below also verifies exact bytes.
    constexpr const char* encodedDescription =
        R"rvx("description":"RVX diagnostic \"quoted\"\u000aline\\backslash")rvx";
    EXPECT_NE(reportText.find(encodedDescription), std::string::npos);
    const nlohmann::json report = ParseReport(reportText);
    ASSERT_TRUE(report.is_object());
    EXPECT_EQ(report.value("schema", ""), "RVX.DX12DebugQueue");
    EXPECT_EQ(report.value("schemaVersion", 0), 1);
    EXPECT_TRUE(report.value("debugLayerEnabled", false));
    EXPECT_TRUE(report.value("available", false));
    EXPECT_TRUE(report.value("readComplete", false));
    EXPECT_GE(report.value("messageCountLimit", 0ULL), 1ULL);
    EXPECT_EQ(report.value("discardedMessageCount", 1ULL), 0ULL);
    EXPECT_EQ(report.value("deniedByStorageFilterCount", ~0ULL), expectedDeniedByStorageFilter);
    EXPECT_TRUE(report.value("criticalMessagesVisible", false));
    EXPECT_EQ(report.value("coverage", ""), "warning-error-corruption");
    EXPECT_EQ(report.value("allowedByStorageFilterCount", 0ULL), 1ULL);
    EXPECT_EQ(report.value("retrievalMessageCount", 0ULL),
              report.value("messageCount", 1ULL));
    EXPECT_EQ(report.value("warningCount", 0ULL), 1ULL);
    EXPECT_EQ(report.value("errorCount", 1ULL), 0ULL);
    EXPECT_EQ(report.value("corruptionCount", 1ULL), 0ULL);
    EXPECT_TRUE(report.value("passed", false));

    const nlohmann::json* message = FindMessage(report, description);
    ASSERT_NE(message, nullptr);
    EXPECT_EQ(message->value("id", 0U),
              static_cast<uint32>(D3D12_MESSAGE_ID_STRING_FROM_APPLICATION));
    EXPECT_EQ(message->value("category", 0U),
              static_cast<uint32>(D3D12_MESSAGE_CATEGORY_APPLICATION_DEFINED));
    EXPECT_EQ(message->value("severity", 0U),
              static_cast<uint32>(D3D12_MESSAGE_SEVERITY_WARNING));
    EXPECT_EQ(message->value("description", ""), description);
}

TEST(DX12Validation, NativeDiagnosticsMarksDroppedQueueMessagesIncomplete)
{
    ScopedReportPath reportPath;
    ASSERT_TRUE(reportPath.IsSet());

    auto device = CreateDebugDevice();
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);
    auto* dx12Device = dynamic_cast<DX12Device*>(device.get());
    ASSERT_NE(dx12Device, nullptr);

    ComPtr<ID3D12InfoQueue> infoQueue = RequireInfoQueue(*dx12Device);
    ASSERT_NE(infoQueue, nullptr);
    ClearForInjectedMessage(*infoQueue.Get());
    ASSERT_TRUE(SUCCEEDED(infoQueue->SetMessageCountLimit(2u)));

    for (uint32 index = 0; index < 3u; ++index)
    {
        const std::string description =
            "RVX diagnostic queue overflow " + std::to_string(index);
        ASSERT_TRUE(SUCCEEDED(infoQueue->AddApplicationMessage(
            D3D12_MESSAGE_SEVERITY_WARNING,
            description.c_str())));
    }

    const RHINativeValidationDiagnostics diagnostics =
        device->GetNativeValidationDiagnostics();
    EXPECT_TRUE(diagnostics.enabled);
    EXPECT_TRUE(diagnostics.available);
    EXPECT_FALSE(diagnostics.readComplete);

    infoQueue.Reset();
    device.reset();

    const nlohmann::json report = ParseReport(ReadReportText(reportPath.GetPath()));
    ASSERT_TRUE(report.is_object());
    EXPECT_EQ(report.value("schemaVersion", 0), 1);
    EXPECT_TRUE(report.value("available", false));
    EXPECT_FALSE(report.value("readComplete", true));
    EXPECT_EQ(report.value("messageCountLimit", 0ULL), 2ULL);
    EXPECT_GT(report.value("discardedMessageCount", 0ULL), 0ULL);
    EXPECT_EQ(report.value("retrievalMessageCount", 0ULL),
              report.value("messageCount", 1ULL));
    EXPECT_FALSE(report.value("passed", true));
}

TEST(DX12Validation, NativeDiagnosticsPreservesChainedTextureTransitions)
{
    auto device = CreateDebugDevice();
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);
    auto desc = RHITextureDesc::Texture2D(4, 4, RHIFormat::RGBA8_UNORM,
        RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource | RHITextureUsage::CopySrc);
    desc.mipLevels = 2;
    auto texture = device->CreateTexture(desc);
    ASSERT_TRUE(texture);
    auto* native = static_cast<DX12Texture*>(texture.Get())->GetResource();
    D3D12_RESOURCE_BARRIER barriers[3]{};
    for (auto& barrier : barriers)
    {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = native;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    EXPECT_EQ(GetDX12LegacyBarrierBatchSize(nullptr, 0), 0u);
    EXPECT_EQ(GetDX12LegacyBarrierBatchSize(barriers, 3), 1u);
    barriers[0].Transition.Subresource = 0;
    barriers[1].Transition.Subresource = 1;
    EXPECT_EQ(GetDX12LegacyBarrierBatchSize(barriers, 3), 2u);
    barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    EXPECT_EQ(GetDX12LegacyBarrierBatchSize(barriers, 3), 3u);

    auto commands = device->CreateCommandContext(RHICommandQueueType::Graphics);
    auto fence = device->CreateFence(0);
    ASSERT_TRUE(commands);
    ASSERT_TRUE(fence);
    commands->Begin();
    commands->TextureBarrier(texture.Get(), RHIResourceState::Common, RHIResourceState::RenderTarget);
    commands->TextureBarrier(texture.Get(), RHIResourceState::RenderTarget, RHIResourceState::Common);
    commands->TextureBarrier(texture.Get(), RHIResourceState::Common, RHIResourceState::CopySource);
    commands->End();
    const uint64 submitted = device->SubmitCommandContext(commands.Get(), fence.Get());
    ASSERT_NE(submitted, 0u);
    device->WaitForFence(fence.Get(), submitted);
    ASSERT_GE(fence->GetCompletedValue(), submitted);
    const auto diagnostics = device->GetNativeValidationDiagnostics();
    EXPECT_TRUE(diagnostics.available);
    EXPECT_TRUE(diagnostics.readComplete);
    EXPECT_EQ(diagnostics.warningCount, 0u);
    EXPECT_EQ(diagnostics.errorCount, 0u);
    EXPECT_EQ(diagnostics.corruptionCount, 0u);
}

TEST(DX12Validation, NativeDiagnosticsRejectsFiltersThatHideWarnings)
{
    ScopedReportPath reportPath;
    ASSERT_TRUE(reportPath.IsSet());
    auto device = CreateDebugDevice();
    RVX_GTEST_REQUIRE_GPU_DEVICE(device, RHIBackendType::DX12);
    auto* dx12 = dynamic_cast<DX12Device*>(device.get());
    ASSERT_NE(dx12, nullptr);
    auto queue = RequireInfoQueue(*dx12);
    ASSERT_NE(queue, nullptr);
    ClearForInjectedMessage(*queue.Get());
    D3D12_MESSAGE_SEVERITY severity = D3D12_MESSAGE_SEVERITY_INFO;
    D3D12_INFO_QUEUE_FILTER filter{};
    filter.DenyList.NumSeverities = 1;
    filter.DenyList.pSeverityList = &severity;
    ASSERT_TRUE(SUCCEEDED(queue->AddStorageFilterEntries(&filter)));
    ASSERT_TRUE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "info only")));
    EXPECT_TRUE(device->GetNativeValidationDiagnostics().readComplete);
    severity = D3D12_MESSAGE_SEVERITY_WARNING;
    ASSERT_TRUE(SUCCEEDED(queue->AddStorageFilterEntries(&filter)));
    ASSERT_TRUE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_WARNING, "hidden warning")));
    EXPECT_FALSE(device->GetNativeValidationDiagnostics().readComplete);
    queue.Reset();
    device.reset();
    const auto report = ParseReport(ReadReportText(reportPath.GetPath()));
    ASSERT_TRUE(report.is_object());
    EXPECT_FALSE(report.value("criticalMessagesVisible", true));
    EXPECT_FALSE(report.value("readComplete", true));
    EXPECT_FALSE(report.value("passed", true));
}
