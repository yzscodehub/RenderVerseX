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
        // Set 2
        m_setAssignments[2] = { 8, 4, 64, 32, 4, 2, 8, 4 };
        // Set 3
        m_setAssignments[3] = { 12, 2, 96, 32, 6, 2, 12, 4 };

        m_initialized = true;
    }

    uint32 DX11BindingRemapper::GetCBSlot(uint32 set, uint32 binding) const
    {
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
