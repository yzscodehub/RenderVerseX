#pragma once

/**
 * @file ResourceUploadRequestInternal.h
 * @brief Testable private byte-accounting seam for upload requests.
 */

#include "RenderContracts/ResourceUploadRequest.h"

namespace RVX::Detail
{
    [[nodiscard]] ResourceUploadRequestCreateCode AccumulateOwnedBytes(
        uint64 elementCount,
        uint64 elementSize,
        uint64& total) noexcept;
} // namespace RVX::Detail
