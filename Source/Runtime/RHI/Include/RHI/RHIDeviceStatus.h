#pragma once

/** @file RHIDeviceStatus.h @brief Value-only runtime device health and fault contract */

#include "RHI/RHIDefinitions.h"

#include <string>

namespace RVX
{
    /** @brief Stable runtime health published by every RHI backend. */
    enum class RHIDeviceRuntimeStatus : uint8
    {
        Ready = 0,
        DeviceLost = 1,
        FatalError = 2,
    };

    /** @brief Operation that first made a backend runtime fault observable. */
    enum class RHIDeviceFaultOperation : uint8
    {
        None = 0,
        DeviceCreation = 1,
        ResourceCreation = 2,
        CommandSubmission = 3,
        FencePoll = 4,
        FenceWait = 5,
        SurfaceAcquire = 6,
        SurfaceResize = 7,
        Present = 8,
        Context = 9,
        Shutdown = 10,
    };

    /** @brief Owned snapshot of the first terminal backend fault. */
    struct RHIDeviceFault
    {
        RHIDeviceRuntimeStatus status = RHIDeviceRuntimeStatus::Ready;
        RHIDeviceFaultOperation operation = RHIDeviceFaultOperation::None;
        RHIBackendType backend = RHIBackendType::None;
        uint32 nativeError = 0;
        uint64 sequence = 0;
        std::string message;

        [[nodiscard]] bool IsFailure() const noexcept
        {
            return status != RHIDeviceRuntimeStatus::Ready;
        }

        [[nodiscard]] bool IsDeviceLost() const noexcept
        {
            return status == RHIDeviceRuntimeStatus::DeviceLost;
        }
    };

    [[nodiscard]] constexpr bool IsDeclaredRHIDeviceRuntimeStatus(
        RHIDeviceRuntimeStatus status) noexcept
    {
        return status >= RHIDeviceRuntimeStatus::Ready &&
               status <= RHIDeviceRuntimeStatus::FatalError;
    }

    [[nodiscard]] constexpr bool IsDeclaredRHIDeviceFaultOperation(
        RHIDeviceFaultOperation operation) noexcept
    {
        return operation >= RHIDeviceFaultOperation::None &&
               operation <= RHIDeviceFaultOperation::Shutdown;
    }
} // namespace RVX
