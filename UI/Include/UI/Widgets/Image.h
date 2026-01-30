/**
 * @file Image.h
 * @brief Image widget
 */

#pragma once

#include "UI/Widget.h"
#include <algorithm>

namespace RVX::UI
{

class RHITexture;

/**
 * @brief Image display mode
 */
enum class ImageMode : uint8
{
    Simple,         ///< Display as-is, may stretch
    Sliced,         ///< 9-slice scaling
    Tiled,          ///< Tile the image
    Filled          ///< Fill with clipping (radial, horizontal, etc.)
};

/**
 * @brief Fill method for filled images
 */
enum class FillMethod : uint8
{
    Horizontal,
    Vertical,
    Radial90,
    Radial180,
    Radial360
};

/**
 * @brief Image widget for displaying textures
 */
class Image : public Widget
{
public:
    using Ptr = std::shared_ptr<Image>;

    Image() = default;

    const char* GetTypeName() const override { return "Image"; }

    // =========================================================================
    // Texture
    // =========================================================================

    void SetTexture(std::shared_ptr<RHITexture> texture) { m_texture = texture; }
    std::shared_ptr<RHITexture> GetTexture() const { return m_texture; }

    // =========================================================================
    // Display Mode
    // =========================================================================

    ImageMode GetImageMode() const { return m_mode; }
    void SetImageMode(ImageMode mode) { m_mode = mode; }

    // =========================================================================
    // Color
    // =========================================================================

    const UIColor& GetColor() const { return m_color; }
    void SetColor(const UIColor& color) { m_color = color; }

    // =========================================================================
    // UV Rect
    // =========================================================================

    const Rect& GetUVRect() const { return m_uvRect; }
    void SetUVRect(const Rect& rect) { m_uvRect = rect; }

    // =========================================================================
    // 9-Slice
    // =========================================================================

    const EdgeInsets& GetSliceBorder() const { return m_sliceBorder; }
    void SetSliceBorder(const EdgeInsets& border) { m_sliceBorder = border; }

    // =========================================================================
    // Fill
    // =========================================================================

    FillMethod GetFillMethod() const { return m_fillMethod; }
    void SetFillMethod(FillMethod method) { m_fillMethod = method; }

    float GetFillAmount() const { return m_fillAmount; }
    void SetFillAmount(float amount) { m_fillAmount = std::clamp(amount, 0.0f, 1.0f); }

    // =========================================================================
    // Preserve Aspect
    // =========================================================================

    bool GetPreserveAspect() const { return m_preserveAspect; }
    void SetPreserveAspect(bool preserve) { m_preserveAspect = preserve; }

    // =========================================================================
    // Factory
    // =========================================================================

    static Ptr Create()
    {
        return std::make_shared<Image>();
    }

protected:
    void OnRender(UIRenderer& renderer) override;

private:
    std::shared_ptr<RHITexture> m_texture;
    ImageMode m_mode = ImageMode::Simple;
    UIColor m_color = UIColor::White();
    Rect m_uvRect = Rect(0, 0, 1, 1);
    EdgeInsets m_sliceBorder;
    FillMethod m_fillMethod = FillMethod::Horizontal;
    float m_fillAmount = 1.0f;
    bool m_preserveAspect = false;
};

} // namespace RVX::UI
