#pragma once

/**
 * @file RenderExecutionReceiptDigest.h
 * @brief Stable full-content SHA-256 binding for render execution receipts.
 *
 * This deliberately belongs to Render rather than a product-facing Sample
 * reporter.  Completion-owned qualification evidence must be able to bind the
 * complete receipt while the render submission is still authoritative; a
 * Sample only consumes the resulting digest when it serializes evidence.
 */

#include "Core/Hash/SHA256.h"
#include "Render/Features/RenderFeatureTypes.h"

#include <iosfwd>
#include <string>

namespace RVX
{
    /**
     * @brief Writes every semantic field in a locale-independent canonical
     * representation.
     *
     * Floating-point values use @c max_digits10 under the classic locale;
     * non-finite values are represented by stable tokens. The output is an
     * internal digest preimage, not a backwards-compatible report format.
     */
    void WriteRenderExecutionReceiptCanonical(
        std::ostream& stream,
        const RenderExecutionReceipt& receipt);

    [[nodiscard]] std::string SerializeRenderExecutionReceiptCanonical(
        const RenderExecutionReceipt& receipt);

    /**
     * @brief Computes the full canonical digest without throwing.
     *
     * An all-zero digest signals that canonicalization could not allocate or
     * serialize. Callers in a noexcept render completion path must reject it.
     */
    [[nodiscard]] Hash::SHA256Digest ComputeRenderExecutionReceiptDigest(
        const RenderExecutionReceipt& receipt) noexcept;

    [[nodiscard]] bool IsRenderExecutionReceiptDigestValid(
        const Hash::SHA256Digest& digest) noexcept;
} // namespace RVX
