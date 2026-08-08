#pragma once

/** @file SampleScreenshotWriter.h @brief Shared PPM capture writer. */

#include "Render/RenderDiagnostics.h"

#include <filesystem>
#include <string>

namespace RVX
{
    bool WriteSampleScreenshotPPM(const RenderFrameCaptureResult& capture,
                                  const std::filesystem::path& path,
                                  std::string* outError = nullptr);
} // namespace RVX
