#include "DX11BindingRemapper.h"

namespace RVX
{
    DX11BindingRemapper::DX11BindingRemapper()
    {
        Initialize();
    }

    void DX11BindingRemapper::Initialize()
    {
        if (m_initialized) return;

        // Default slot allocation
        // Set 0
        m_setAssignments[0] = { 0, 4, 0, 32, 0, 2, 0, 4 };
        // Set 1
        m_setAssignments[1] = { 4, 4, 32, 32, 2, 2, 4, 4 };
        // Samplers use the renderer's sparse logical slot ABI directly; see
        // GetSamplerSlot(). The range values remain for custom assignments.
        m_setAssignments[2] = { 8, 4, 64, 32, 4, 2, 8, 8 };
        // Set 3. D3D11 has only 16 sampler slots; set 3 has no default sampler range.
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
        if ((set == 0 || set == 2) && binding < 16)
        {
            if (set == 0 || (binding >= 6 && binding <= 10))
            {
                return binding;
            }
        }
        if (set >= m_setAssignments.size()) return UINT32_MAX;
        const auto& assignment = m_setAssignments[set];
        if (binding >= assignment.samplerSlotCount) return UINT32_MAX;
        return assignment.samplerSlotBase + binding;
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
