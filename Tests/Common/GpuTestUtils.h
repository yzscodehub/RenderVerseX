#pragma once

#include "Core/Core.h"
#include "RHI/RHI.h"
#include "RHI_BackendFactory/RHIBackendFactory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>

namespace RVX::Test
{
    class GpuLogEnvironment : public ::testing::Environment
    {
    public:
        void SetUp() override
        {
            Log::Initialize();
            RVX_CORE_INFO("GPU Validation Tests");
        }

        void TearDown() override
        {
            Log::Shutdown();
        }
    };

    [[maybe_unused]] inline const auto* g_gpuLogEnvironment =
        ::testing::AddGlobalTestEnvironment(new GpuLogEnvironment);

    inline std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    inline bool IsSoftwareAdapterName(const std::string& adapterName)
    {
        const auto lowerName = ToLowerAscii(adapterName);
        return lowerName.find("llvmpipe") != std::string::npos ||
               lowerName.find("swiftshader") != std::string::npos ||
               lowerName.find("software") != std::string::npos ||
               lowerName.find("warp") != std::string::npos ||
               lowerName.find("basic render") != std::string::npos;
    }

    inline bool IsHardwareGpuDevice(const std::unique_ptr<IRHIDevice>& device)
    {
        return device && !IsSoftwareAdapterName(device->GetCapabilities().adapterName);
    }

    /** @brief Whether required native CI explicitly permits WARP/Lavapipe. */
    inline bool NativeAllowsSoftwareAdapter()
    {
#if defined(_MSC_VER)
        char* value = nullptr;
        std::size_t valueLength = 0;
        if (_dupenv_s(&value,
                      &valueLength,
                      "RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER") != 0 ||
            value == nullptr)
        {
            return false;
        }
        const std::string normalized = ToLowerAscii(value);
        std::free(value);
#else
        const char* value = std::getenv("RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER");
        if (value == nullptr)
        {
            return false;
        }
        const std::string normalized = ToLowerAscii(value);
#endif
        return normalized == "1" || normalized == "true" ||
               normalized == "on" || normalized == "yes";
    }

    /**
     * @brief Create a native-validation device with the CI software-adapter
     * opt-in propagated to the actual RHI creation request.
     *
     * The environment flag is intentionally not a production default.  It is
     * only an explicit validation-host policy, and still leaves a missing
     * backend/device as a required-suite failure rather than treating it as a
     * successful test run.
     */
    inline std::unique_ptr<IRHIDevice> CreateNativeValidationDevice(
        RHIBackendType backend,
        RHIDeviceDesc desc)
    {
        desc.allowSoftwareAdapter = desc.allowSoftwareAdapter ||
            NativeAllowsSoftwareAdapter();
        return CreateRHIDevice(backend, desc);
    }
} // namespace RVX::Test

#define RVX_GTEST_REQUIRE_GPU_DEVICE(device, backend)                                      \
    do                                                                                     \
    {                                                                                      \
        if (!(device))                                                                      \
        {                                                                                  \
            GTEST_SKIP() << RVX::ToString(backend) << " device is not available";          \
        }                                                                                  \
        const auto& rvxGpuCaps = (device)->GetCapabilities();                              \
        if (::RVX::Test::IsSoftwareAdapterName(rvxGpuCaps.adapterName) &&                 \
            !::RVX::Test::NativeAllowsSoftwareAdapter())                                  \
        {                                                                                  \
            GTEST_SKIP() << RVX::ToString(backend) << " uses software adapter '"           \
                         << rvxGpuCaps.adapterName << "'";                                \
        }                                                                                  \
    } while (false)

#define RVX_GTEST_SKIP_IF_NO_GPU_BACKENDS(count)                                           \
    do                                                                                     \
    {                                                                                      \
        if ((count) == 0)                                                                  \
        {                                                                                  \
            GTEST_SKIP() << "No hardware GPU backend is available";                        \
        }                                                                                  \
    } while (false)
