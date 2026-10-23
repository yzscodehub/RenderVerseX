#include "DX11BindingRemapper.h"

namespace RVX
{
    namespace
    {
        constexpr std::array<uint32, 8> kFrameSamplerLogicalBindings = {
            0u, 1u, 2u, 5u, 7u, 9u, 13u, 19u};
    }

    DX11BindingRemapper::DX11BindingRemapper()
    {
        Initialize();
    }

    void DX11BindingRemapper::Initialize()
    {
        if (m_initialized) return;

        // Default slot allocation
        // Set 0
        m_setAssignments[0] = { 0, 4, 0, 32, 0, 2, 0, 8 };
        // Set 1
        m_setAssignments[1] = { 4, 4, 32, 32, 2, 2, 0, 0 };
        // Samplers use the fixed lossless ABI in GetSamplerSlot(). The range
        // values document the physical allocation; sampler mappings are not
        // configurable because a custom mapping would diverge from FXC
        // register-space flattening and reflection restoration.
        m_setAssignments[2] = { 8, 4, 64, 32, 4, 2, 8, 8 };
        // Set 3. D3D11 has only 16 sampler slots; set 3 has no sampler range.
        m_setAssignments[3] = { 12, 2, 96, 32, 6, 2, 0, 0 };

        m_initialized = true;
    }

    uint32 DX11BindingRemapper::GetCBSlot(uint32 set, uint32 binding) const
    {
        // Match the SM5 source-flattening ABI in DXCCompiler.cpp. Frame
        // constant buffers retain sparse logical slots while the currently
        // supported object/material sets reserve one non-overlapping slot.
        if (set == 0 && binding < DX11_MAX_CBUFFER_SLOTS)
        {
            return binding;
        }
        if (binding == 0)
        {
            if (set == 1) return 1;
            if (set == 2) return 2;
            if (set == 3) return 4;
        }
        if (set >= m_setAssignments.size()) return UINT32_MAX;
        const auto& assignment = m_setAssignments[set];
        if (binding >= assignment.cbSlotCount) return UINT32_MAX;
        return assignment.cbSlotBase + binding;
    }

    uint32 DX11BindingRemapper::GetSRVSlot(uint32 set, uint32 binding) const
    {
        if (set >= m_setAssignments.size()) return UINT32_MAX;
        const auto& assignment = m_setAssignments[set];
        if (binding >= assignment.srvSlotCount) return UINT32_MAX;
        return assignment.srvSlotBase + binding;
    }

    uint32 DX11BindingRemapper::GetUAVSlot(uint32 set, uint32 binding) const
    {
        if (set >= m_setAssignments.size()) return UINT32_MAX;
        const auto& assignment = m_setAssignments[set];
        if (binding >= assignment.uavSlotCount) return UINT32_MAX;
        return assignment.uavSlotBase + binding;
    }

    uint32 DX11BindingRemapper::GetSamplerSlot(uint32 set, uint32 binding) const
    {
        if (set == 0)
        {
            for (uint32 physicalSlot = 0;
                 physicalSlot < kFrameSamplerLogicalBindings.size();
                 ++physicalSlot)
            {
                if (kFrameSamplerLogicalBindings[physicalSlot] == binding)
                {
                    return physicalSlot;
                }
            }
            return UINT32_MAX;
        }

        if (set == 2 && binding >= 14u && binding <= 21u)
        {
            return 8u + binding - 14u;
        }

        return UINT32_MAX;
    }

    void DX11BindingRemapper::SetSlotAssignment(uint32 set, const SlotAssignment& assignment)
    {
        if (set < m_setAssignments.size())
        {
            m_setAssignments[set] = assignment;
        }
    }

    const DX11BindingRemapper::SlotAssignment& DX11BindingRemapper::GetSlotAssignment(uint32 set) const
    {
        static SlotAssignment empty = {};
        if (set >= m_setAssignments.size()) return empty;
        return m_setAssignments[set];
    }

} // namespace RVX
