#include "ECS/Commands.h"

#include <algorithm>
#include <thread>

namespace RVX::ECS
{
    EntityCommandBuffer::~EntityCommandBuffer()
    {
        TerminalizeOutstanding(CommandStatus::Discarded, CommandError::Discarded);
    }

    EntityReceipt EntityCommandBuffer::Create()
    {
        AssertOwnerThread();
        if (m_finalized)
        {
            return MakeRejectedEntityReceipt(CommandError::AlreadyCommitted);
        }
        if (!m_originRuntimeId.IsValid())
        {
            return MakeRejectedEntityReceipt(CommandError::InvalidTarget);
        }

        try
        {
            auto entityState = std::make_shared<Detail::EntityReceiptState>();
            entityState->sceneRuntimeId = m_originRuntimeId;
            entityState->command = std::make_shared<Detail::CommandReceiptState>();
            m_commands.push_back({
                .receipt = entityState->command,
                .apply = [entityState](Registry& registry,
                                       std::vector<PendingReceipt>& pending,
                                       CommandError& error)
                {
                    const EntityHandle entity = registry.CreateEntity();
                    if (!entity.IsValid())
                    {
                        error = CommandError::OperationRejected;
                        return false;
                    }

                    pending.push_back({.state = entityState, .entity = entity});
                    return true;
                },
            });
            return EntityReceipt(std::move(entityState));
        }
        catch (...)
        {
            return MakeRejectedEntityReceipt(CommandError::RecordingFailed);
        }
    }

    CommandReceipt EntityCommandBuffer::Destroy(EntityHandle entity)
    {
        return RecordTargeted(entity, [](Registry& registry, EntityHandle resolved)
        {
            return registry.DestroyEntity(resolved);
        });
    }

    CommandReceipt EntityCommandBuffer::Destroy(const EntityRef& entity)
    {
        return RecordTargeted(entity, [](Registry& registry, EntityHandle resolved)
        {
            return registry.DestroyEntity(resolved);
        });
    }

    CommandReceipt EntityCommandBuffer::Destroy(const EntityReceipt& receipt)
    {
        return RecordTargeted(receipt, [](Registry& registry, EntityHandle resolved)
        {
            return registry.DestroyEntity(resolved);
        });
    }

    CommandReceipt EntityCommandBuffer::SetEnabled(EntityHandle entity, bool enabled)
    {
        return RecordTargeted(entity, [enabled](Registry& registry, EntityHandle resolved)
        {
            return registry.SetEnabled(resolved, enabled);
        });
    }

    CommandReceipt EntityCommandBuffer::SetEnabled(const EntityRef& entity, bool enabled)
    {
        return RecordTargeted(entity, [enabled](Registry& registry, EntityHandle resolved)
        {
            return registry.SetEnabled(resolved, enabled);
        });
    }

    CommandReceipt EntityCommandBuffer::SetEnabled(const EntityReceipt& receipt, bool enabled)
    {
        return RecordTargeted(receipt, [enabled](Registry& registry, EntityHandle resolved)
        {
            return registry.SetEnabled(resolved, enabled);
        });
    }

    bool EntityCommandBuffer::Commit(Registry& registry)
    {
        AssertOwnerThread();
        registry.AssertOwnerThread();
        if (m_finalized)
        {
            return false;
        }
        if (!IsBoundTo(registry))
        {
            TerminalizeOutstanding(CommandStatus::Rejected, CommandError::InvalidTarget);
            m_finalized = true;
            return false;
        }

        std::vector<PendingReceipt> pending;
        for (size_t commandIndex = 0; commandIndex < m_commands.size(); ++commandIndex)
        {
            RecordedCommand& command = m_commands[commandIndex];
            CommandError error = CommandError::None;
            const size_t pendingStart = pending.size();
            bool applied = false;
            try
            {
                applied = command.apply(registry, pending, error);
            }
            catch (...)
            {
                error = CommandError::OperationRejected;
            }

            if (!applied)
            {
                command.receipt->status = CommandStatus::Rejected;
                command.receipt->error = error == CommandError::None ? CommandError::OperationRejected : error;
                RejectRemaining(m_commands, commandIndex + 1u, CommandError::TransactionAborted);
                m_finalized = true;
                return false;
            }

            for (size_t receiptIndex = pendingStart; receiptIndex < pending.size(); ++receiptIndex)
            {
                PendingReceipt& created = pending[receiptIndex];
                created.state->entity = created.entity;
                created.state->command->status = CommandStatus::Applied;
                created.state->command->error = CommandError::None;
            }
            command.receipt->status = CommandStatus::Applied;
            command.receipt->error = CommandError::None;
        }

        m_committed = true;
        m_finalized = true;
        return true;
    }

    void EntityCommandBuffer::Reset()
    {
        AssertOwnerThread();
        TerminalizeOutstanding(CommandStatus::Discarded, CommandError::Discarded);
        m_commands.clear();
        m_committed = false;
        m_finalized = false;
    }

    CommandReceipt EntityCommandBuffer::RecordTargeted(EntityHandle entity, TargetOperation operation)
    {
        AssertOwnerThread();
        if (m_finalized)
        {
            return MakeRejectedReceipt(CommandError::AlreadyCommitted);
        }
        if (!m_originRuntimeId.IsValid())
        {
            return MakeRejectedReceipt(CommandError::InvalidTarget);
        }

        try
        {
            EntityTarget target;
            target.entity = entity;
            target.sceneRuntimeId = m_originRuntimeId;
            auto receiptState = std::make_shared<Detail::CommandReceiptState>();
            m_commands.push_back({
                .receipt = receiptState,
                .apply = [target = std::move(target), operation = std::move(operation)]
                (Registry& registry, std::vector<PendingReceipt>& pending, CommandError& error)
                {
                    const EntityHandle resolved = ResolveTarget(target, pending);
                    if (!resolved.IsValid())
                    {
                        error = CommandError::InvalidTarget;
                        return false;
                    }

                    if (!operation(registry, resolved))
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

    CommandReceipt EntityCommandBuffer::RecordTargeted(const EntityRef& entity, TargetOperation operation)
    {
        AssertOwnerThread();
        if (m_finalized)
        {
            return MakeRejectedReceipt(CommandError::AlreadyCommitted);
        }
        if (!m_originRuntimeId.IsValid() || entity.GetSceneRuntimeId() != m_originRuntimeId)
        {
            return MakeRejectedReceipt(CommandError::InvalidTarget);
        }

        try
        {
            EntityTarget target;
            target.entity = entity.GetHandle();
            target.sceneRuntimeId = entity.GetSceneRuntimeId();
            auto receiptState = std::make_shared<Detail::CommandReceiptState>();
            m_commands.push_back({
                .receipt = receiptState,
                .apply = [target = std::move(target), operation = std::move(operation)]
                (Registry& registry, std::vector<PendingReceipt>& pending, CommandError& error)
                {
                    const EntityHandle resolved = ResolveTarget(target, pending);
                    if (!resolved.IsValid())
                    {
                        error = CommandError::InvalidTarget;
                        return false;
                    }

                    if (!operation(registry, resolved))
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

    CommandReceipt EntityCommandBuffer::RecordTargeted(const EntityReceipt& receipt, TargetOperation operation)
    {
        AssertOwnerThread();
        if (m_finalized)
        {
            return MakeRejectedReceipt(CommandError::AlreadyCommitted);
        }
        if (!m_originRuntimeId.IsValid() || receipt.m_entityState == nullptr ||
            receipt.m_entityState->sceneRuntimeId != m_originRuntimeId)
        {
            return MakeRejectedReceipt(CommandError::InvalidTarget);
        }

        try
        {
            EntityTarget target;
            target.receipt = receipt.m_entityState;
            target.sceneRuntimeId = receipt.m_entityState->sceneRuntimeId;
            auto receiptState = std::make_shared<Detail::CommandReceiptState>();
            m_commands.push_back({
                .receipt = receiptState,
                .apply = [target = std::move(target), operation = std::move(operation)]
                (Registry& registry, std::vector<PendingReceipt>& pending, CommandError& error)
                {
                    const EntityHandle resolved = ResolveTarget(target, pending);
                    if (!resolved.IsValid())
                    {
                        error = CommandError::InvalidTarget;
                        return false;
                    }

                    if (!operation(registry, resolved))
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

    TransactionReceipt EntityCommandBuffer::CommitTransaction(Registry& registry)
    {
        AssertOwnerThread();
        registry.AssertOwnerThread();
        if (m_finalized)
        {
            return {CommandStatus::Rejected, CommandError::AlreadyCommitted};
        }
        if (!IsBoundTo(registry))
        {
            TerminalizeOutstanding(CommandStatus::Rejected, CommandError::InvalidTarget);
            m_finalized = true;
            return {CommandStatus::Rejected, CommandError::InvalidTarget};
        }

        std::vector<PendingReceipt> pending;
        size_t rejectedIndex = m_commands.size();
        CommandError rejectedError = CommandError::None;
        const bool committed = registry.ExecuteTransaction([&](Registry& staging)
        {
            for (size_t commandIndex = 0; commandIndex < m_commands.size(); ++commandIndex)
            {
                CommandError error = CommandError::None;
                bool applied = false;
                try
                {
                    applied = m_commands[commandIndex].apply(staging, pending, error);
                }
                catch (...)
                {
                    error = CommandError::OperationRejected;
                }

                if (!applied)
                {
                    rejectedIndex = commandIndex;
                    rejectedError = error == CommandError::None ? CommandError::OperationRejected : error;
                    return false;
                }
            }
            return true;
        });

        m_finalized = true;
        if (!committed)
        {
            RejectRemaining(m_commands, 0, CommandError::TransactionAborted);
            if (rejectedIndex < m_commands.size())
            {
                m_commands[rejectedIndex].receipt->status = CommandStatus::Rejected;
                m_commands[rejectedIndex].receipt->error = rejectedError;
            }
            return {CommandStatus::Rejected,
                    rejectedError == CommandError::None ? CommandError::TransactionAborted : rejectedError};
        }

        for (PendingReceipt& created : pending)
        {
            created.state->entity = created.entity;
            created.state->command->status = CommandStatus::Applied;
            created.state->command->error = CommandError::None;
        }
        for (RecordedCommand& command : m_commands)
        {
            command.receipt->status = CommandStatus::Applied;
            command.receipt->error = CommandError::None;
        }
        m_committed = true;
        return {CommandStatus::Applied, CommandError::None};
    }

    EntityHandle EntityCommandBuffer::ResolveTarget(const EntityTarget& target,
                                                    const std::vector<PendingReceipt>& pending)
    {
        if (target.entity.IsValid())
        {
            return target.entity;
        }

        if (target.receipt == nullptr)
        {
            return EntityHandle::Invalid();
        }

        if (target.receipt->entity.has_value())
        {
            return *target.receipt->entity;
        }

        const auto found = std::find_if(pending.begin(), pending.end(), [&target](const PendingReceipt& candidate)
        {
            return candidate.state == target.receipt;
        });
        return found != pending.end() ? found->entity : EntityHandle::Invalid();
    }

    void EntityCommandBuffer::RejectRemaining(std::vector<RecordedCommand>& commands,
                                              size_t firstIndex,
                                              CommandError error)
    {
        for (size_t commandIndex = firstIndex; commandIndex < commands.size(); ++commandIndex)
        {
            commands[commandIndex].receipt->status = CommandStatus::Rejected;
            commands[commandIndex].receipt->error = error;
        }
    }

    CommandReceipt EntityCommandBuffer::MakeRejectedReceipt(CommandError error)
    {
        auto state = std::make_shared<Detail::CommandReceiptState>();
        state->status = CommandStatus::Rejected;
        state->error = error;
        return CommandReceipt(std::move(state));
    }

    EntityReceipt EntityCommandBuffer::MakeRejectedEntityReceipt(CommandError error)
    {
        auto state = std::make_shared<Detail::EntityReceiptState>();
        state->command = std::make_shared<Detail::CommandReceiptState>();
        state->command->status = CommandStatus::Rejected;
        state->command->error = error;
        return EntityReceipt(std::move(state));
    }

    void EntityCommandBuffer::TerminalizeOutstanding(CommandStatus status, CommandError error) noexcept
    {
        for (RecordedCommand& command : m_commands)
        {
            if (command.receipt != nullptr && command.receipt->status == CommandStatus::Queued)
            {
                command.receipt->status = status;
                command.receipt->error = error;
            }
        }
    }

    void EntityCommandBuffer::AssertOwnerThread() const
    {
        if (m_originRuntimeId.IsValid())
        {
            RVX_DEBUG_ASSERT_MSG(m_ownerThread == std::this_thread::get_id(),
                                 "EntityCommandBuffer access must remain on its owner thread");
        }
    }

    bool EntityCommandBuffer::IsBoundTo(const Registry& registry) const
    {
        return m_originRuntimeId.IsValid() && m_originRuntimeId == registry.GetSceneRuntimeId();
    }
} // namespace RVX::ECS
