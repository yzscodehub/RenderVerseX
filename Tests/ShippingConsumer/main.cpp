/** @file main.cpp @brief Public shipping composition link consumer. */

#include "Engine/Engine.h"
#include "Engine/ShippingRuntimeComposition.h"

#include <filesystem>
#include <type_traits>
#include <utility>

static_assert(!std::is_default_constructible_v<RVX::Engine>);
static_assert(!std::is_copy_constructible_v<RVX::EngineRuntimeDependencies>);
static_assert(
    std::is_nothrow_move_constructible_v<RVX::EngineRuntimeDependencies>);

int main(int argc, char* argv[])
{
    const std::filesystem::path cookedShaderRoot = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::path("Content/Shaders");

    RVX::Engine engine(
        RVX::CreateShippingEngineRuntimeDependencies(cookedShaderRoot));
    return engine.IsInitialized() ? 1 : 0;
}
