#pragma once

#include "ECS/Registry.h"

#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace RVX::ECS
{
    enum class CommandStatus : uint8
    {
        Queued = 0,
        Applied,
        Rejected,
        Discarded,
    };

    enum class CommandError : uint8
    {
        None = 0,
        AlreadyCommitted,
        InvalidTarget,
        OperationRejected,
        TransactionAborted,
        RecordingFailed,
        Discarded,
    };

    namespace Detail
    {
        struct CommandReceiptState
        {
            CommandStatus status = CommandStatus::Queued;
            CommandError error = CommandError::None;
        };

        struct EntityReceiptState
        {
            std::optional<EntityHandle> entity;
            SceneRuntimeId sceneRuntimeId;
            std::shared_ptr<CommandReceiptState> command;
        };
    } // namespace Detail

    /** @brief Observable result for one deferred structural command. */
    class CommandReceipt
    {
    public:
        CommandReceipt() = default;

        [[nodiscard]] CommandStatus GetStatus() const
        {
            return m_state != nullptr ? m_state->status : CommandStatus::Rejected;
        }

        [[nodiscard]] CommandError GetError() const
        {
            return m_state != nullptr ? m_state->error : CommandError::InvalidTarget;
        }

        [[nodiscard]] bool IsQueued() const { return GetStatus() == CommandStatus::Queued; }
        [[nodiscard]] bool IsApplied() const { return GetStatus() == CommandStatus::Applied; }
        [[nodiscard]] bool IsRejected() const { return GetStatus() == CommandStatus::Rejected; }
        [[nodiscard]] bool IsDiscarded() const { return GetStatus() == CommandStatus::Discarded; }
        explicit operator bool() const { return IsApplied(); }

    protected:
        friend class EntityCommandBuffer;

        explicit CommandReceipt(std::shared_ptr<Detail::CommandReceiptState> state)
            : m_state(std::move(state))
        {
        }

        std::shared_ptr<Detail::CommandReceiptState> m_state;
    };

    /** @brief Deferred result of an entity-create command. */
    class EntityReceipt final : public CommandReceipt
    {
    public:
        EntityReceipt() = default;

        [[nodiscard]] bool IsResolved() const
        {
            return IsApplied() && m_entityState != nullptr && m_entityState->entity.has_value();
        }

        [[nodiscard]] EntityHandle GetEntity() const
        {
            return IsResolved() ? *m_entityState->entity : EntityHandle::Invalid();
        }

    private:
        friend class EntityCommandBuffer;
        friend class EntityTransaction;

        explicit EntityReceipt(std::shared_ptr<Detail::EntityReceiptState> state)
            : CommandReceipt(state != nullptr ? state->command : nullptr)
            , m_entityState(std::move(state))
        {
        }

        std::shared_ptr<Detail::EntityReceiptState> m_entityState;
    };

    /** @brief Aggregate status of one all-or-nothing EntityTransaction commit. */
    class TransactionReceipt
    {
    public:
        [[nodiscard]] CommandStatus GetStatus() const { return m_status; }
        [[nodiscard]] CommandError GetError() const { return m_error; }
        [[nodiscard]] bool IsApplied() const { return m_status == CommandStatus::Applied; }
        [[nodiscard]] bool IsRejected() const { return m_status == CommandStatus::Rejected; }
        explicit operator bool() const { return IsApplied(); }

    private:
        friend class EntityCommandBuffer;
        friend class EntityTransaction;

        TransactionReceipt(CommandStatus status, CommandError error)
            : m_status(status)
            , m_error(error)
        {
        }

        CommandStatus m_status = CommandStatus::Queued;
        CommandError m_error = CommandError::None;
    };

    /**
     * @brief Recorded structural changes committed at a phase boundary.
     *
     * Commit applies commands in recording order. Each operation is locally
     * exception-safe and carries a receipt; use EntityTransaction for a group
     * that must be published all-or-nothing. Buffers are bound to the Registry
     * that created them. EntityHandle overloads remain registry-local; prefer
     * EntityRef where the target may cross an API boundary.
     *
     * This P1 buffer is owner-thread-only and is not an MPSC recorder.
     */
    class EntityCommandBuffer
    {
    public:
        EntityCommandBuffer() = default;
        EntityCommandBuffer(const EntityCommandBuffer&) = delete;
        EntityCommandBuffer& operator=(const EntityCommandBuffer&) = delete;
        EntityCommandBuffer(EntityCommandBuffer&&) noexcept = default;
        EntityCommandBuffer& operator=(EntityCommandBuffer&&) noexcept = default;
        ~EntityCommandBuffer();

        [[nodiscard]] EntityReceipt Create();
        /** @brief EntityHandle targets are valid only in this buffer's origin Registry. */
        [[nodiscard]] CommandReceipt Destroy(EntityHandle entity);
        [[nodiscard]] CommandReceipt Destroy(const EntityRef& entity);
        [[nodiscard]] CommandReceipt Destroy(const EntityReceipt& receipt);
        [[nodiscard]] CommandReceipt SetEnabled(EntityHandle entity, bool enabled);
        [[nodiscard]] CommandReceipt SetEnabled(const EntityRef& entity, bool enabled);
        [[nodiscard]] CommandReceipt SetEnabled(const EntityReceipt& receipt, bool enabled);

        template<Fragment T>
        [[nodiscard]] CommandReceipt Add(EntityHandle entity, T value = {})
        {
            return RecordTargeted(entity, [value](Registry& staging, EntityHandle resolved)
            {
                return staging.Add<T>(resolved, value);
            });
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt Add(const EntityRef& entity, T value = {})
        {
            return RecordTargeted(entity, [value](Registry& staging, EntityHandle resolved)
            {
                return staging.Add<T>(resolved, value);
            });
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt Add(const EntityReceipt& receipt, T value = {})
        {
            return RecordTargeted(receipt, [value](Registry& staging, EntityHandle resolved)
            {
                return staging.Add<T>(resolved, value);
            });
        }

        /**
         * @brief Add a fragment whose EntityHandle member references another
         * entity created in the same command buffer.
         *
         * Both receipts are resolved inside transaction staging, so hierarchy
         * and relationship fragments publish atomically without predicting
         * future handles or capturing pointers.
         */
        template<Fragment T, EntityHandle T::* ReferencedEntityMember>
        [[nodiscard]] CommandReceipt AddLinked(const EntityReceipt& targetReceipt,
                                               const EntityReceipt& referencedReceipt,
                                               T value = {})
        {
            AssertOwnerThread();
            if (m_finalized)
            {
                return MakeRejectedReceipt(CommandError::AlreadyCommitted);
            }
            if (!m_originRuntimeId.IsValid() || targetReceipt.m_entityState == nullptr ||
                referencedReceipt.m_entityState == nullptr ||
                targetReceipt.m_entityState->sceneRuntimeId != m_originRuntimeId ||
                referencedReceipt.m_entityState->sceneRuntimeId != m_originRuntimeId)
            {
                return MakeRejectedReceipt(CommandError::InvalidTarget);
            }

            try
            {
                EntityTarget target;
                target.receipt = targetReceipt.m_entityState;
                target.sceneRuntimeId = m_originRuntimeId;
                EntityTarget referenced;
                referenced.receipt = referencedReceipt.m_entityState;
                referenced.sceneRuntimeId = m_originRuntimeId;
                auto receiptState = std::make_shared<Detail::CommandReceiptState>();
                m_commands.push_back({
                    .receipt = receiptState,
                    .apply = [target = std::move(target),
                              referenced = std::move(referenced),
                              value](Registry& registry,
                                     std::vector<PendingReceipt>& pending,
                                     CommandError& error) mutable
                    {
                        const EntityHandle resolvedTarget = ResolveTarget(target, pending);
                        const EntityHandle resolvedReference = ResolveTarget(referenced, pending);
                        if (!resolvedTarget.IsValid() || !resolvedReference.IsValid())
                        {
                            error = CommandError::InvalidTarget;
                            return false;
                        }
                        value.*ReferencedEntityMember = resolvedReference;
                        if (!registry.Add<T>(resolvedTarget, value))
                        {
                            error = CommandError::OperationRejected;
                            return false;
                        }
                        return true;
                    },
                });
                return CommandReceipt(std::move(receiptState));
            }
            catch (...)
            {
                return MakeRejectedReceipt(CommandError::RecordingFailed);
            }
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt Remove(EntityHandle entity)
        {
            return RecordTargeted(entity, [](Registry& staging, EntityHandle resolved)
            {
                return staging.Remove<T>(resolved);
            });
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt Remove(const EntityRef& entity)
        {
            return RecordTargeted(entity, [](Registry& staging, EntityHandle resolved)
            {
                return staging.Remove<T>(resolved);
            });
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt Remove(const EntityReceipt& receipt)
        {
            return RecordTargeted(receipt, [](Registry& staging, EntityHandle resolved)
            {
                return staging.Remove<T>(resolved);
            });
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt SetFragmentEnabled(EntityHandle entity, bool enabled)
        {
            return RecordTargeted(entity, [enabled](Registry& staging, EntityHandle resolved)
            {
                return staging.SetFragmentEnabled<T>(resolved, enabled);
            });
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt SetFragmentEnabled(const EntityRef& entity, bool enabled)
        {
            return RecordTargeted(entity, [enabled](Registry& staging, EntityHandle resolved)
            {
                return staging.SetFragmentEnabled<T>(resolved, enabled);
            });
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt SetFragmentEnabled(const EntityReceipt& receipt, bool enabled)
        {
            return RecordTargeted(receipt, [enabled](Registry& staging, EntityHandle resolved)
            {
                return staging.SetFragmentEnabled<T>(resolved, enabled);
            });
        }

        [[nodiscard]] bool IsEmpty() const { return m_commands.empty(); }
        [[nodiscard]] bool IsCommitted() const { return m_committed; }
        /** @brief Applies commands in order; inspect individual receipts on failure. */
        bool Commit(Registry& registry);
        void Reset();

    private:
        friend class Registry;
        friend class EntityTransaction;

        EntityCommandBuffer(SceneRuntimeId originRuntimeId, std::thread::id ownerThread)
            : m_originRuntimeId(originRuntimeId)
            , m_ownerThread(ownerThread)
        {
        }

        struct PendingReceipt
        {
            std::shared_ptr<Detail::EntityReceiptState> state;
            EntityHandle entity = EntityHandle::Invalid();
        };

        struct EntityTarget
        {
            EntityHandle entity = EntityHandle::Invalid();
            SceneRuntimeId sceneRuntimeId;
            std::shared_ptr<Detail::EntityReceiptState> receipt;
        };

        using Command = std::function<bool(Registry&, std::vector<PendingReceipt>&, CommandError&)>;
        using TargetOperation = std::function<bool(Registry&, EntityHandle)>;

        struct RecordedCommand
        {
            std::shared_ptr<Detail::CommandReceiptState> receipt;
            Command apply;
        };

        [[nodiscard]] CommandReceipt RecordTargeted(EntityHandle entity, TargetOperation operation);
        [[nodiscard]] CommandReceipt RecordTargeted(const EntityRef& entity, TargetOperation operation);
        [[nodiscard]] CommandReceipt RecordTargeted(const EntityReceipt& receipt, TargetOperation operation);
        [[nodiscard]] TransactionReceipt CommitTransaction(Registry& registry);
        static EntityHandle ResolveTarget(const EntityTarget& target, const std::vector<PendingReceipt>& pending);
        static void RejectRemaining(std::vector<RecordedCommand>& commands, size_t firstIndex, CommandError error);
        static CommandReceipt MakeRejectedReceipt(CommandError error);
        static EntityReceipt MakeRejectedEntityReceipt(CommandError error);
        void TerminalizeOutstanding(CommandStatus status, CommandError error) noexcept;
        void AssertOwnerThread() const;
        [[nodiscard]] bool IsBoundTo(const Registry& registry) const;

        std::vector<RecordedCommand> m_commands;
        SceneRuntimeId m_originRuntimeId;
        std::thread::id m_ownerThread;
        bool m_committed = false;
        bool m_finalized = false;
    };

    /**
     * @brief Bound command buffer that commits the complete group atomically.
     *
     * Transactions are non-owning, owner-thread views over their originating
     * Registry. A stale transaction rejects pending receipts without touching
     * the former Registry address.
     */
    class EntityTransaction
    {
    public:
        explicit EntityTransaction(Registry& registry)
            : m_registry(&registry)
            , m_lifetime(registry.m_lifetime)
            , m_commands(registry.CreateCommandBuffer())
        {
        }

        [[nodiscard]] EntityReceipt Create() { return m_commands.Create(); }
        [[nodiscard]] CommandReceipt Destroy(EntityHandle entity) { return m_commands.Destroy(entity); }
        [[nodiscard]] CommandReceipt Destroy(const EntityRef& entity) { return m_commands.Destroy(entity); }
        [[nodiscard]] CommandReceipt Destroy(const EntityReceipt& receipt) { return m_commands.Destroy(receipt); }
        [[nodiscard]] CommandReceipt SetEnabled(EntityHandle entity, bool enabled)
        {
            return m_commands.SetEnabled(entity, enabled);
        }
        [[nodiscard]] CommandReceipt SetEnabled(const EntityRef& entity, bool enabled)
        {
            return m_commands.SetEnabled(entity, enabled);
        }
        [[nodiscard]] CommandReceipt SetEnabled(const EntityReceipt& receipt, bool enabled)
        {
            return m_commands.SetEnabled(receipt, enabled);
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt Add(EntityHandle entity, T value = {})
        {
            return m_commands.Add<T>(entity, value);
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt Add(const EntityRef& entity, T value = {})
        {
            return m_commands.Add<T>(entity, value);
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt Add(const EntityReceipt& receipt, T value = {})
        {
            return m_commands.Add<T>(receipt, value);
        }

        template<Fragment T, EntityHandle T::* ReferencedEntityMember>
        [[nodiscard]] CommandReceipt AddLinked(const EntityReceipt& targetReceipt,
                                               const EntityReceipt& referencedReceipt,
                                               T value = {})
        {
            return m_commands.AddLinked<T, ReferencedEntityMember>(
                targetReceipt, referencedReceipt, value);
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt Remove(EntityHandle entity)
        {
            return m_commands.Remove<T>(entity);
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt Remove(const EntityRef& entity)
        {
            return m_commands.Remove<T>(entity);
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt Remove(const EntityReceipt& receipt)
        {
            return m_commands.Remove<T>(receipt);
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt SetFragmentEnabled(EntityHandle entity, bool enabled)
        {
            return m_commands.SetFragmentEnabled<T>(entity, enabled);
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt SetFragmentEnabled(const EntityRef& entity, bool enabled)
        {
            return m_commands.SetFragmentEnabled<T>(entity, enabled);
        }

        template<Fragment T>
        [[nodiscard]] CommandReceipt SetFragmentEnabled(const EntityReceipt& receipt, bool enabled)
        {
            return m_commands.SetFragmentEnabled<T>(receipt, enabled);
        }

        [[nodiscard]] EntityCommandBuffer& Commands() { return m_commands; }
        [[nodiscard]] const EntityCommandBuffer& Commands() const { return m_commands; }
        [[nodiscard]] TransactionReceipt Commit()
        {
            const std::shared_ptr<Detail::RegistryLifetime> lifetime = m_lifetime.lock();
            if (m_registry == nullptr || lifetime == nullptr || !lifetime->alive.load(std::memory_order_acquire))
            {
                m_commands.TerminalizeOutstanding(CommandStatus::Rejected, CommandError::InvalidTarget);
                return TransactionReceipt(CommandStatus::Rejected, CommandError::InvalidTarget);
            }
            return m_commands.CommitTransaction(*m_registry);
        }

    private:
        Registry* m_registry = nullptr;
        std::weak_ptr<Detail::RegistryLifetime> m_lifetime;
        EntityCommandBuffer m_commands;
    };
} // namespace RVX::ECS
