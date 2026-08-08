/** @file main.cpp @brief Unified RenderVerseX sample executable. */

#include "RegisterSamples.h"
#include "Samples/SampleRegistry.h"
#include "Samples/SampleRunner.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    RVX::SampleRegistry registry;
    std::string error;
    if (!RVX::RegisterRenderVerseSamples(registry, &error))
    {
        std::cerr << error << "\n";
        return 1;
    }

    return RVX::SampleRunner(registry).Run(argc, argv);
}
