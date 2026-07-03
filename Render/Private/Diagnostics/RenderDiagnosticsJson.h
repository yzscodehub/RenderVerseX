#pragma once

/**
 * @file RenderDiagnosticsJson.h
 * @brief Lightweight JSON helpers for render diagnostics artifacts.
 */

#include "Core/Types.h"

#include <string>
#include <string_view>

namespace RVX::RenderDiagnostics
{
    const char* JsonBool(bool value);
    std::string JsonString(std::string_view value);
    std::string JsonOptionalIndex(uint32 value);
} // namespace RVX::RenderDiagnostics
