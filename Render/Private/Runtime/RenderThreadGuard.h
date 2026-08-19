#pragma once

/**
 * @file RenderThreadGuard.h
 * @brief One-time thread ownership binding for render runtime state.
 */

#include "Core/Types.h"

#include <mutex>
#include <thread>

namespace RVX
{
    enum class RenderThreadGuardCode : uint8
    {
        Owner = 0,
        Unbound = 1,
        WrongThread = 2
    };

    /**
     * @brief Records one owner thread and validates future access.
     *
     * The non-asserting QueryCurrentThread() path supports structured release
     * failure handling. ValidateCurrentThread() additionally asserts in Debug.
     * The same guard type can protect Render-owned or Update-owned state.
     */
    class RenderThreadGuard final : public NonMovable
    {
    public:
        RenderThreadGuard() = default;
        ~RenderThreadGuard() = default;

        RenderThreadGuardCode BindCurrentThread() noexcept;
        [[nodiscard]] RenderThreadGuardCode QueryCurrentThread() const noexcept;
        [[nodiscard]] RenderThreadGuardCode ValidateCurrentThread() const noexcept;
        [[nodiscard]] std::thread::id GetOwnerThreadId() const noexcept;

    private:
        mutable std::mutex m_mutex;
        std::thread::id m_ownerThread{};
    };
} // namespace RVX
