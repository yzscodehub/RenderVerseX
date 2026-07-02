#pragma once

#include "Core/Core.h"
#include "RHI/RHI.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
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
} // namespace RVX::Test

#define RVX_GTEST_REQUIRE_GPU_DEVICE(device, backend)                                      \
    do                                                                                     \
    {                                                                                      \
        if (!(device))                                                                      \
        {                                                                                  \
            GTEST_SKIP() << RVX::ToString(backend) << " device is not available";          \
        }                                                                                  \
        const auto& rvxGpuCaps = (device)->GetCapabilities();                              \
        if (::RVX::Test::IsSoftwareAdapterName(rvxGpuCaps.adapterName))                    \
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
