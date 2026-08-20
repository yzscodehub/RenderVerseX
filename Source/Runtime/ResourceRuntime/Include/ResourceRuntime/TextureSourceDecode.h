#pragma once

/** @file TextureSourceDecode.h @brief Injected development texture decode contract */

#include "Core/Diagnostics/Trace.h"
#include "Resource/Loader/TextureReference.h"
#include "Resource/Types/TextureResource.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RVX::Resource
{
    struct DecodedTextureData
    {
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        TextureMetadata metadata;
        std::string sourceKey;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return bytes && !bytes->empty() && metadata.width != 0 &&
                   metadata.height != 0;
        }
    };

    /**
     * @brief Optional source decoder injected only by development composition.
     *
     * Runtime resource streaming owns scheduling and publication, while the
     * implementation that parses PNG/JPEG/HDR source bytes remains outside the
     * shipping target closure.
     */
    class ITextureSourceDecoder
    {
    public:
        virtual ~ITextureSourceDecoder() = default;

        virtual bool Decode(
            const TextureReference& reference,
            const std::string& modelPath,
            const Diagnostics::TraceContext& traceContext,
            DecodedTextureData& outData,
            std::string& outError,
            const std::function<bool()>& isCancellationRequested = {}) = 0;
    };
} // namespace RVX::Resource
