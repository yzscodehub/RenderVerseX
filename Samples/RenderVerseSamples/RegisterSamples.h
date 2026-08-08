#pragma once

#include <string>

namespace RVX
{
    class SampleRegistry;

    bool RegisterRenderVerseSamples(SampleRegistry& registry,
                                    std::string* outError = nullptr);
} // namespace RVX
