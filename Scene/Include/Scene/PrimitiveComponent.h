#pragma once

/**
 * @file PrimitiveComponent.h
 * @brief Scene component base for renderable primitives
 */

#include "Core/Math/AABB.h"
#include "Core/Types.h"
#include "Scene/SceneComponent.h"

namespace RVX
{
    class RenderScene;

    /**
     * @brief Base class for scene components that can contribute render data.
     */
    class PrimitiveComponent : public SceneComponent
    {
    public:
        // =====================================================================
        // Type Information
        // =====================================================================

        const char* GetClassName() const override { return "PrimitiveComponent"; }

        // =====================================================================
        // Render State
        // =====================================================================

        bool IsVisible() const { return m_visible; }
        void SetVisible(bool visible) { m_visible = visible; }

        uint32 GetLayerMask() const { return m_layerMask; }
        void SetLayerMask(uint32 layerMask) { m_layerMask = layerMask; }

        // =====================================================================
        // Bounds
        // =====================================================================

        const AABB& GetLocalBounds() const { return m_localBounds; }
        void SetLocalBounds(const AABB& bounds);

        AABB GetWorldBounds() const;

        // =====================================================================
        // Render Extraction
        // =====================================================================

        virtual bool HasRenderData() const { return false; }
        virtual void CollectRenderData(RenderScene& scene) const { (void)scene; }

    protected:
        void OnTransformChanged() override;

    private:
        bool m_visible = true;
        uint32 m_layerMask = ~0u;
        AABB m_localBounds;
        mutable AABB m_cachedWorldBounds;
        mutable bool m_boundsDirty = true;
    };

} // namespace RVX
