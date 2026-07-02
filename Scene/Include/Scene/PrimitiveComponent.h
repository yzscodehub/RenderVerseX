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
    struct RenderPrimitiveProxy;
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

        void OnRegister() override;
        void OnUnregister() override;

        bool IsVisible() const { return m_visible; }
        void SetVisible(bool visible) { m_visible = visible; }
        void SetEnabled(bool enabled) override;

        uint32 GetLayerMask() const { return m_layerMask; }
        void SetLayerMask(uint32 layerMask);

        bool IsSpatialDirty() const { return m_spatialDirty; }
        void ClearSpatialDirty() { m_spatialDirty = false; }

        // =====================================================================
        // Bounds
        // =====================================================================

        const AABB& GetLocalBounds() const { return m_localBounds; }
        void SetLocalBounds(const AABB& bounds);

        AABB GetWorldBounds() const;

        // =====================================================================
        // Render Extraction
        // =====================================================================

        virtual bool HasRenderProxy() const { return HasRenderData(); }
        virtual bool CreateRenderProxy(RenderPrimitiveProxy& outProxy) const { (void)outProxy; return false; }

        /** @brief Legacy render extraction path; use RenderProxy for the main renderer path. */
        virtual bool HasRenderData() const { return false; }
        virtual void CollectRenderData(RenderScene& scene) const { (void)scene; }

    protected:
        void OnTransformChanged() override;

    private:
        void MarkSpatialDirty();

        bool m_visible = true;
        uint32 m_layerMask = ~0u;
        AABB m_localBounds;
        mutable AABB m_cachedWorldBounds;
        mutable bool m_boundsDirty = true;
        bool m_spatialDirty = true;
    };

} // namespace RVX
