#pragma once

#include "DX11Common.h"
#include <array>

namespace RVX
{
    // =============================================================================
    // DX11 Binding Remapper
    // =============================================================================
    // Maps RHI (set, binding) model to DX11 slot model
    //
    // Mapping strategy:
    // - Set 0-3 map to different slot ranges
    // - CBV: slots 0-13 (DX11 limit is 14 per stage)
    // - SRV: slots 0-127
    // - UAV: slots 0-7 (DX11.0) or 0-63 (DX11.1)
    // - Sampler: slots 0-15
    //
    // Default allocation:
    // Set 0: CB 0-3,   SRV 0-31,  UAV 0-1,  Sampler 0-3
    // Set 1: CB 4-7,   SRV 32-63, UAV 2-3,  Sampler 4-7
    // Set 2: CB 8-11,  SRV 64-95, UAV 4-5,  Sampler 8-11
    // Set 3: CB 12-13, SRV 96-127, UAV 6-7, Sampler 12-15

    class DX11BindingRemapper
    {
    public:
        struct SlotAssignment
        {
            uint32 cbSlotBase = 0;
            uint32 cbSlotCount = 4;
            uint32 srvSlotBase = 0;
            uint32 srvSlotCount = 32;
            uint32 uavSlotBase = 0;
            uint32 uavSlotCount = 2;
            uint32 samplerSlotBase = 0;
            uint32 samplerSlotCount = 4;
        };

        static DX11BindingRemapper& Get()
        {
            static DX11BindingRemapper instance;
            return instance;
        }

        void Initialize();

        // Get DX11 slot from (set, binding)
        uint32 GetCBSlot(uint32 set, uint32 binding) const;
        uint32 GetSRVSlot(uint32 set, uint32 binding) const;
        uint32 GetUAVSlot(uint32 set, uint32 binding) const;
        uint32 GetSamplerSlot(uint32 set, uint32 binding) const;

        // Configure slot assignments
        void SetSlotAssignment(uint32 set, const SlotAssignment& assignment);
        const SlotAssignment& GetSlotAssignment(uint32 set) const;

        // Push constants use a reserved CB slot
        static constexpr uint32 PUSH_CONSTANT_SLOT = 13;

    private:
        DX11BindingRemapper();

        std::array<SlotAssignment, 4> m_setAssignments;
        bool m_initialized = false;
    };

} // namespace RVX
