#include "Render/Render.h"

#include <string_view>

int main()
{
    RVX::RenderSubsystem renderer;
    return std::string_view(renderer.GetName()) == "RenderSubsystem" ? 0 : 1;
}
