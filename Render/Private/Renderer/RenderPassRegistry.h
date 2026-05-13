#pragma once

#include "Render/Passes/IRenderPass.h"
#include "RHI/RHIDevice.h"

#include <memory>
#include <vector>

namespace RVX
{
    class RenderPassRegistry
    {
    public:
        void AddPass(std::unique_ptr<IRenderPass> pass, IRHIDevice* device);
        bool RemovePass(const char* name);
        void Clear();

        size_t GetPassCount() const { return m_passes.size(); }
        std::vector<std::unique_ptr<IRenderPass>>& GetPasses() { return m_passes; }
        const std::vector<std::unique_ptr<IRenderPass>>& GetPasses() const { return m_passes; }

    private:
        std::vector<std::unique_ptr<IRenderPass>> m_passes;
    };

} // namespace RVX
