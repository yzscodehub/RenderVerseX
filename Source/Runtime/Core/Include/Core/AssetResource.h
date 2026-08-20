#pragma once

/**
 * @file AssetResource.h
 * @brief Engine-level asset identity contract shared by runtime-facing modules.
 */

#include "Core/Types.h"

#include <string_view>

namespace RVX
{
    class IAssetResource
    {
    public:
        virtual ~IAssetResource() = default;

        virtual uint64 GetAssetResourceId() const = 0;
        virtual std::string_view GetAssetResourceName() const = 0;
        virtual bool IsAssetResourceLoaded() const = 0;
    };

} // namespace RVX
