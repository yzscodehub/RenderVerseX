/**
 * @file Image.cpp
 * @brief Image widget implementation
 */

#include "UI/Widgets/Image.h"

#include "UI/UIRenderer.h"

namespace RVX::UI
{

void Image::OnRender(UIRenderer& renderer)
{
    renderer.DrawImage(m_textureView, GetGlobalRect(), m_uvRect, m_color);
}

} // namespace RVX::UI
