#pragma once

/**
 * @file JsonWriter.h
 * @brief Lightweight JSON formatting helpers for diagnostics artifacts.
 */

#include "Core/Types.h"

#include <string>
#include <string_view>

namespace RVX::Diagnostics
{
    class JsonWriter
    {
    public:
        static const char* Bool(bool value);
        static std::string String(std::string_view value);
        static std::string OptionalIndex(uint32 value);
    };

    const char* JsonBool(bool value);
    std::string JsonString(std::string_view value);
    std::string JsonOptionalIndex(uint32 value);
} // namespace RVX::Diagnostics
