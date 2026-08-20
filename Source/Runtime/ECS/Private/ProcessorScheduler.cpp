#include "ECS/ProcessorScheduler.h"

#include "ECS/Registry.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace RVX::ECS
{
    namespace
    {
        [[nodiscard]] std::string_view GetDefaultGroupName(ProcessorPhase phase)
        {
            switch (phase)
            {
            case ProcessorPhase::BeginSimulation: return "BeginSimulation";
            case ProcessorPhase::Gameplay: return "Gameplay";
            case ProcessorPhase::BeforeFixedStep: return "BeforeFixedStep";
            case ProcessorPhase::FixedAnimation: return "FixedAnimation";
            case ProcessorPhase::RootMotion: return "RootMotion";
            case ProcessorPhase::SceneToPhysics: return "SceneToPhysics";
            case ProcessorPhase::PhysicsSimulation: return "PhysicsSimulation";
            case ProcessorPhase::PhysicsToScene: return "PhysicsToScene";
            case ProcessorPhase::FixedTransformResolve: return "FixedTransformResolve";
            case ProcessorPhase::EndFixedStep: return "EndFixedStep";
            case ProcessorPhase::PostSimulation: return "PostSimulation";
            case ProcessorPhase::PrePresentation: return "PrePresentation";
            case ProcessorPhase::Transform: return "Transform";
            case ProcessorPhase::Bounds: return "Bounds";
            case ProcessorPhase::Spatial: return "Spatial";
            case ProcessorPhase::Feature: return "Feature";
            case ProcessorPhase::RenderExtraction: return "RenderExtraction";
            case ProcessorPhase::EndFrameCleanup: return "EndFrameCleanup";
            }

            return "Unknown";
        }

        [[nodiscard]] bool Contains(const std::vector<std::type_index>& values, const std::type_index value)
        {
            return std::find(values.begin(), values.end(), value) != values.end();
        }

        [[nodiscard]] std::vector<std::type_index> MakeUnique(const std::vector<std::type_index>& values)
        {
            std::vector<std::type_index> unique;
            unique.reserve(values.size());
            for (const std::type_index value : values)
            {
                if (!Contains(unique, value))
                {
                    unique.push_back(value);
                }
            }
            return unique;
        }
    } // namespace

    bool ProcessorScheduler::Register(ProcessorDescriptor descriptor)
    {
        if (descriptor.name.empty() || (!descriptor.run && !descriptor.runWithContext))
        {
            return false;
        }

        const bool duplicateName = std::any_of(m_processors.begin(), m_processors.end(), [&descriptor](const RegisteredProcessor& existing)
        {
            return existing.descriptor.name == descriptor.name;
        });
        if (duplicateName)
        {
            return false;
        }

        m_processors.push_back({
            .descriptor = std::move(descriptor),
            .registrationSequence = m_nextRegistrationSequence++,
        });
        m_needsCompile = true;
        m_compileSucceeded = false;
        return true;
    }

    void ProcessorScheduler::Clear()
    {
        m_processors.clear();
        m_compiledOrder.clear();
        m_compiledProcessorNames.clear();
        m_conflicts.clear();
        m_lastCompileError.clear();
        m_lastExecutionFailure.reset();
        m_nextRegistrationSequence = 0;
        m_needsCompile = false;
        m_compileSucceeded = false;
    }

    bool ProcessorScheduler::Compile()
    {
        if (!m_needsCompile)
        {
            return m_compileSucceeded;
        }

        m_needsCompile = false;
        m_compileSucceeded = false;
        m_lastCompileError.clear();
        m_compiledOrder.clear();
        m_compiledProcessorNames.clear();
        m_conflicts.clear();

        std::unordered_map<std::string, uint32> processorsByName;
        processorsByName.reserve(m_processors.size());
        for (uint32 index = 0; index < m_processors.size(); ++index)
        {
            processorsByName.emplace(m_processors[index].descriptor.name, index);
        }

        std::vector<std::vector<uint32>> edges(m_processors.size());
        std::vector<uint32> indegree(m_processors.size(), 0);
        const auto addDependency = [this, &processorsByName, &edges, &indegree](uint32 beforeIndex, const std::string& afterName) -> bool
        {
            const auto after = processorsByName.find(afterName);
            if (after == processorsByName.end())
            {
                m_lastCompileError = "Processor '" + m_processors[beforeIndex].descriptor.name
                    + "' references unknown dependency '" + afterName + "'.";
                return false;
            }

            const uint32 afterIndex = after->second;
            if (static_cast<uint8>(m_processors[beforeIndex].descriptor.phase)
                > static_cast<uint8>(m_processors[afterIndex].descriptor.phase))
            {
                m_lastCompileError = "Processor dependency from '" + m_processors[beforeIndex].descriptor.name
                    + "' to '" + m_processors[afterIndex].descriptor.name + "' violates fixed phase order.";
                return false;
            }
            if (m_processors[beforeIndex].descriptor.stepMode != m_processors[afterIndex].descriptor.stepMode)
            {
                m_lastCompileError = "Processor dependency between '" + m_processors[beforeIndex].descriptor.name
                    + "' and '" + m_processors[afterIndex].descriptor.name + "' crosses variable and fixed clocks.";
                return false;
            }

            std::vector<uint32>& outgoing = edges[beforeIndex];
            if (std::find(outgoing.begin(), outgoing.end(), afterIndex) == outgoing.end())
            {
                outgoing.push_back(afterIndex);
                ++indegree[afterIndex];
            }
            return true;
        };

        for (uint32 index = 0; index < m_processors.size(); ++index)
        {
            const ProcessorDescriptor& descriptor = m_processors[index].descriptor;
            for (const std::string& afterName : descriptor.before)
            {
                if (!addDependency(index, afterName))
                {
                    return false;
                }
            }
            for (const std::string& beforeName : descriptor.after)
            {
                const auto before = processorsByName.find(beforeName);
                if (before == processorsByName.end())
                {
                    m_lastCompileError = "Processor '" + descriptor.name
                        + "' references unknown dependency '" + beforeName + "'.";
                    return false;
                }
                if (!addDependency(before->second, descriptor.name))
                {
                    return false;
                }
            }
        }

        std::vector<uint32> ready;
        ready.reserve(m_processors.size());
        for (uint32 index = 0; index < m_processors.size(); ++index)
        {
            if (indegree[index] == 0)
            {
                ready.push_back(index);
            }
        }

        while (!ready.empty())
        {
            const auto next = std::min_element(ready.begin(), ready.end(), [this](uint32 lhs, uint32 rhs)
            {
                return IsScheduledBefore(lhs, rhs);
            });
            const uint32 index = *next;
            ready.erase(next);
            m_compiledOrder.push_back(index);

            for (const uint32 successor : edges[index])
            {
                if (--indegree[successor] == 0)
                {
                    ready.push_back(successor);
                }
            }
        }

        if (m_compiledOrder.size() != m_processors.size())
        {
            m_compiledOrder.clear();
            m_lastCompileError = "Processor dependency cycle detected.";
            return false;
        }

        m_compiledProcessorNames.reserve(m_compiledOrder.size());
        for (const uint32 index : m_compiledOrder)
        {
            m_compiledProcessorNames.push_back(m_processors[index].descriptor.name);
        }
        BuildConflictDiagnostics();
        m_compileSucceeded = true;
        return true;
    }

    void ProcessorScheduler::Run(Registry& registry)
    {
        static_cast<void>(RunVariable(registry));
    }

    bool ProcessorScheduler::RunVariable(Registry& registry, float64 deltaSeconds, uint64 frameSequence)
    {
        m_lastExecutionFailure.reset();
        if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0)
        {
            return false;
        }
        return RunStep(registry, ProcessorStepMode::Variable, deltaSeconds, frameSequence, 0);
    }

    bool ProcessorScheduler::RunFixed(Registry& registry, float64 deltaSeconds, uint64 fixedStepSequence)
    {
        m_lastExecutionFailure.reset();
        if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0)
        {
            return false;
        }
        return RunStep(registry, ProcessorStepMode::Fixed, deltaSeconds, 0, fixedStepSequence);
    }

    bool ProcessorScheduler::IsScheduledBefore(uint32 lhsIndex, uint32 rhsIndex) const
    {
        const RegisteredProcessor& lhs = m_processors[lhsIndex];
        const RegisteredProcessor& rhs = m_processors[rhsIndex];
        if (lhs.descriptor.phase != rhs.descriptor.phase)
        {
            return lhs.descriptor.phase < rhs.descriptor.phase;
        }
        if (lhs.descriptor.groupOrder != rhs.descriptor.groupOrder)
        {
            return lhs.descriptor.groupOrder < rhs.descriptor.groupOrder;
        }

        const std::string_view lhsGroup = GetEffectiveGroup(lhs);
        const std::string_view rhsGroup = GetEffectiveGroup(rhs);
        if (lhsGroup != rhsGroup)
        {
            return lhsGroup < rhsGroup;
        }
        if (lhs.descriptor.order != rhs.descriptor.order)
        {
            return lhs.descriptor.order < rhs.descriptor.order;
        }
        return lhs.registrationSequence < rhs.registrationSequence;
    }

    std::string_view ProcessorScheduler::GetEffectiveGroup(const RegisteredProcessor& processor) const
    {
        return processor.descriptor.group.empty()
            ? GetDefaultGroupName(processor.descriptor.phase)
            : processor.descriptor.group;
    }

    bool ProcessorScheduler::RunStep(Registry& registry,
        ProcessorStepMode stepMode,
        float64 deltaSeconds,
        uint64 frameSequence,
        uint64 fixedStepSequence)
    {
        m_lastExecutionFailure.reset();
        if (!Compile())
        {
            return false;
        }

        for (const uint32 index : m_compiledOrder)
        {
            const RegisteredProcessor& processor = m_processors[index];
            if (processor.descriptor.stepMode != stepMode)
            {
                continue;
            }

            if (processor.descriptor.runWithContext)
            {
                ProcessorExecutionContext context{
                    registry,
                    processor.descriptor.phase,
                    GetEffectiveGroup(processor),
                    stepMode,
                    deltaSeconds,
                    frameSequence,
                    fixedStepSequence,
                };
                processor.descriptor.runWithContext(context);
                if (context.HasReportedFailure())
                {
                    m_lastExecutionFailure = ProcessorExecutionFailure{
                        .processorName = processor.descriptor.name,
                        .phase = processor.descriptor.phase,
                        .group = std::string(GetEffectiveGroup(processor)),
                        .stepMode = stepMode,
                        .deltaSeconds = deltaSeconds,
                        .frameSequence = frameSequence,
                        .fixedStepSequence = fixedStepSequence,
                        .reason = context.GetFailureReason(),
                    };
                    return false;
                }
            }
            else
            {
                processor.descriptor.run(registry);
            }
        }
        return true;
    }

    void ProcessorScheduler::BuildConflictDiagnostics()
    {
        const auto addConflicts = [this](const RegisteredProcessor& first,
                                         const RegisteredProcessor& second,
                                         const std::vector<std::type_index>& firstReads,
                                         const std::vector<std::type_index>& firstWrites,
                                         const std::vector<std::type_index>& secondReads,
                                         const std::vector<std::type_index>& secondWrites,
                                         ProcessorAccessDomain domain)
        {
            const std::vector<std::type_index> uniqueFirstReads = MakeUnique(firstReads);
            const std::vector<std::type_index> uniqueFirstWrites = MakeUnique(firstWrites);
            const std::vector<std::type_index> uniqueSecondReads = MakeUnique(secondReads);
            const std::vector<std::type_index> uniqueSecondWrites = MakeUnique(secondWrites);

            const auto append = [this, &first, &second, domain](const std::type_index key, ProcessorConflictKind kind)
            {
                m_conflicts.push_back({
                    .firstProcessor = first.descriptor.name,
                    .secondProcessor = second.descriptor.name,
                    .accessName = key.name(),
                    .domain = domain,
                    .kind = kind,
                });
            };

            for (const std::type_index key : uniqueFirstWrites)
            {
                if (Contains(uniqueSecondWrites, key))
                {
                    append(key, ProcessorConflictKind::WriteWrite);
                }
                if (Contains(uniqueSecondReads, key))
                {
                    append(key, ProcessorConflictKind::ReadWrite);
                }
            }
            for (const std::type_index key : uniqueFirstReads)
            {
                if (Contains(uniqueSecondWrites, key))
                {
                    append(key, ProcessorConflictKind::ReadWrite);
                }
            }
        };

        for (uint32 firstIndex = 0; firstIndex < m_processors.size(); ++firstIndex)
        {
            for (uint32 secondIndex = firstIndex + 1; secondIndex < m_processors.size(); ++secondIndex)
            {
                const ProcessorAccess& first = m_processors[firstIndex].descriptor.access;
                const ProcessorAccess& second = m_processors[secondIndex].descriptor.access;
                addConflicts(m_processors[firstIndex], m_processors[secondIndex],
                    first.reads, first.writes, second.reads, second.writes, ProcessorAccessDomain::Fragment);
                addConflicts(m_processors[firstIndex], m_processors[secondIndex],
                    first.resourceReads, first.resourceWrites, second.resourceReads, second.resourceWrites,
                    ProcessorAccessDomain::Resource);
            }
        }
    }
} // namespace RVX::ECS
