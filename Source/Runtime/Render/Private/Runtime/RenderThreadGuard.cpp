#include "Runtime/RenderThreadGuard.h"

#include "Core/Assert.h"

namespace RVX
{
    RenderThreadGuardCode RenderThreadGuard::BindCurrentThread() noexcept
    {
        const std::thread::id currentThread = std::this_thread::get_id();
        std::lock_guard lock(m_mutex);
        if (m_ownerThread == std::thread::id{})
        {
            m_ownerThread = currentThread;
            return RenderThreadGuardCode::Owner;
        }
        return m_ownerThread == currentThread
                   ? RenderThreadGuardCode::Owner
                   : RenderThreadGuardCode::WrongThread;
    }

    RenderThreadGuardCode RenderThreadGuard::QueryCurrentThread() const noexcept
    {
        const std::thread::id currentThread = std::this_thread::get_id();
        std::lock_guard lock(m_mutex);
        if (m_ownerThread == std::thread::id{})
        {
            return RenderThreadGuardCode::Unbound;
        }
        return m_ownerThread == currentThread
                   ? RenderThreadGuardCode::Owner
                   : RenderThreadGuardCode::WrongThread;
    }

    RenderThreadGuardCode
        RenderThreadGuard::ValidateCurrentThread() const noexcept
    {
        const RenderThreadGuardCode code = QueryCurrentThread();
        RVX_DEBUG_ASSERT_MSG(code == RenderThreadGuardCode::Owner,
                             "Render thread ownership validation failed");
        return code;
    }

    std::thread::id RenderThreadGuard::GetOwnerThreadId() const noexcept
    {
        std::lock_guard lock(m_mutex);
        return m_ownerThread;
    }
} // namespace RVX
