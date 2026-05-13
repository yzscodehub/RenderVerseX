#include "Renderer/RenderPassRegistry.h"
#include "Core/Log.h"

#include <algorithm>
#include <cstring>

namespace RVX
{
    void RenderPassRegistry::AddPass(std::unique_ptr<IRenderPass> pass, IRHIDevice* device)
    {
        if (!pass)
            return;

        RVX_CORE_DEBUG("SceneRenderer: Adding pass '{}'", pass->GetName());
        pass->OnAdd(device);
        m_passes.push_back(std::move(pass));

        std::stable_sort(
            m_passes.begin(),
            m_passes.end(),
            [](const std::unique_ptr<IRenderPass>& a, const std::unique_ptr<IRenderPass>& b)
            {
                return a->GetPriority() < b->GetPriority();
            });
    }

    bool RenderPassRegistry::RemovePass(const char* name)
    {
        if (!name)
            return false;

        auto it = std::find_if(
            m_passes.begin(),
            m_passes.end(),
            [name](const std::unique_ptr<IRenderPass>& pass)
            {
                return pass && std::strcmp(pass->GetName(), name) == 0;
            });

        if (it == m_passes.end())
            return false;

        RVX_CORE_DEBUG("SceneRenderer: Removing pass '{}'", name);
        (*it)->OnRemove();
        m_passes.erase(it);
        return true;
    }

    void RenderPassRegistry::Clear()
    {
        for (auto& pass : m_passes)
        {
            if (pass)
            {
                pass->OnRemove();
            }
        }
        m_passes.clear();
    }

} // namespace RVX
