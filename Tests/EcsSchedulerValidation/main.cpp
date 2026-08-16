#include "ECS/ECS.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <typeindex>
#include <vector>

namespace
{
    struct PositionFragment
    {
        int32_t value = 0;
    };

    struct VelocityFragment
    {
        int32_t value = 0;
    };

    struct PhysicsWorldResource {};
} // namespace

TEST(EcsSchedulerValidation, SchedulerRunsSeriallyInPhaseOrderThenStableRegistrationOrder)
{
    RVX::ECS::Registry registry;
    RVX::ECS::ProcessorScheduler scheduler;
    std::vector<std::string> order;

    EXPECT_TRUE(scheduler.Register({
        .name = "update-second",
        .phase = RVX::ECS::ProcessorPhase::Update,
        .order = 10,
        .access = {},
        .run = [&](RVX::ECS::Registry&) { order.emplace_back("update-second"); },
    }));
    EXPECT_TRUE(scheduler.Register({
        .name = "pre",
        .phase = RVX::ECS::ProcessorPhase::PreUpdate,
        .order = 100,
        .access = {},
        .run = [&](RVX::ECS::Registry&) { order.emplace_back("pre"); },
    }));
    EXPECT_TRUE(scheduler.Register({
        .name = "update-first",
        .phase = RVX::ECS::ProcessorPhase::Update,
        .order = 0,
        .access = {},
        .run = [&](RVX::ECS::Registry&) { order.emplace_back("update-first"); },
    }));
    EXPECT_FALSE(scheduler.Register({
        .name = "pre",
        .access = {},
        .run = [](RVX::ECS::Registry&) {},
    }));

    scheduler.Run(registry);
    EXPECT_EQ(order, (std::vector<std::string>{"pre", "update-first", "update-second"}));
}

TEST(EcsSchedulerValidation, DependenciesCompileToDeterministicOrderAndRejectInvalidGraphs)
{
    RVX::ECS::Registry registry;
    RVX::ECS::ProcessorScheduler scheduler;
    std::vector<std::string> order;

    EXPECT_TRUE(scheduler.Register({
        .name = "constraints",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .group = "simulation",
        .order = 0,
        .after = {"integrate"},
        .run = [&](RVX::ECS::Registry&) { order.emplace_back("constraints"); },
    }));
    EXPECT_TRUE(scheduler.Register({
        .name = "integrate",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .group = "simulation",
        .order = 100,
        .before = {"constraints"},
        .run = [&](RVX::ECS::Registry&) { order.emplace_back("integrate"); },
    }));
    EXPECT_TRUE(scheduler.Register({
        .name = "presentation",
        .phase = RVX::ECS::ProcessorPhase::PrePresentation,
        .run = [&](RVX::ECS::Registry&) { order.emplace_back("presentation"); },
    }));

    EXPECT_TRUE(scheduler.Compile());
    EXPECT_EQ(scheduler.GetCompiledProcessorNames(),
        (std::vector<std::string>{"integrate", "constraints", "presentation"}));
    EXPECT_TRUE(scheduler.RunVariable(registry, 1.0 / 60.0, 12));
    EXPECT_EQ(order, scheduler.GetCompiledProcessorNames());

    RVX::ECS::ProcessorScheduler unknownDependency;
    EXPECT_TRUE(unknownDependency.Register({
        .name = "orphan",
        .after = {"missing"},
        .run = [](RVX::ECS::Registry&) {},
    }));
    EXPECT_FALSE(unknownDependency.Compile());
    EXPECT_NE(unknownDependency.GetLastCompileError().find("unknown dependency"), std::string::npos);

    RVX::ECS::ProcessorScheduler cyclic;
    EXPECT_TRUE(cyclic.Register({
        .name = "one",
        .before = {"two"},
        .run = [](RVX::ECS::Registry&) {},
    }));
    EXPECT_TRUE(cyclic.Register({
        .name = "two",
        .before = {"one"},
        .run = [](RVX::ECS::Registry&) {},
    }));
    EXPECT_FALSE(cyclic.Compile());
    EXPECT_NE(cyclic.GetLastCompileError().find("cycle"), std::string::npos);
}

TEST(EcsSchedulerValidation, VariableAndFixedProcessorsReceiveTheirOwnClock)
{
    RVX::ECS::Registry registry;
    RVX::ECS::ProcessorScheduler scheduler;
    std::vector<std::string> executions;

    EXPECT_TRUE(scheduler.Register({
        .name = "gameplay",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .stepMode = RVX::ECS::ProcessorStepMode::Variable,
        .runWithContext = [&](RVX::ECS::ProcessorExecutionContext& context)
        {
            EXPECT_EQ(context.stepMode, RVX::ECS::ProcessorStepMode::Variable);
            EXPECT_DOUBLE_EQ(context.deltaSeconds, 1.0 / 30.0);
            EXPECT_EQ(context.frameSequence, 7u);
            EXPECT_EQ(context.fixedStepSequence, 0u);
            executions.emplace_back("gameplay");
        },
    }));
    EXPECT_TRUE(scheduler.Register({
        .name = "physics",
        .phase = RVX::ECS::ProcessorPhase::PhysicsSimulation,
        .stepMode = RVX::ECS::ProcessorStepMode::Fixed,
        .runWithContext = [&](RVX::ECS::ProcessorExecutionContext& context)
        {
            EXPECT_EQ(context.stepMode, RVX::ECS::ProcessorStepMode::Fixed);
            EXPECT_DOUBLE_EQ(context.deltaSeconds, 1.0 / 60.0);
            EXPECT_EQ(context.frameSequence, 0u);
            EXPECT_EQ(context.fixedStepSequence, 42u);
            executions.emplace_back("physics");
        },
    }));

    EXPECT_TRUE(scheduler.RunVariable(registry, 1.0 / 30.0, 7));
    EXPECT_EQ(executions, (std::vector<std::string>{"gameplay"}));
    EXPECT_TRUE(scheduler.RunFixed(registry, 1.0 / 60.0, 42));
    EXPECT_EQ(executions, (std::vector<std::string>{"gameplay", "physics"}));
    EXPECT_FALSE(scheduler.RunFixed(registry, 0.0, 43));
    EXPECT_FALSE(scheduler.RunVariable(
        registry, std::numeric_limits<double>::quiet_NaN(), 8));
    EXPECT_FALSE(scheduler.RunVariable(
        registry, std::numeric_limits<double>::infinity(), 8));
    EXPECT_FALSE(scheduler.RunFixed(
        registry, std::numeric_limits<double>::quiet_NaN(), 43));
    EXPECT_FALSE(scheduler.RunFixed(
        registry, std::numeric_limits<double>::infinity(), 43));
}

TEST(EcsSchedulerValidation, VariableProcessorFailureStopsTheStepAndPreservesTheFirstReason)
{
    RVX::ECS::Registry registry;
    RVX::ECS::ProcessorScheduler scheduler;
    std::vector<std::string> executions;

    ASSERT_TRUE(scheduler.Register({
        .name = "failing-gameplay",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .runWithContext = [&executions](RVX::ECS::ProcessorExecutionContext& context)
        {
            executions.emplace_back("failing-gameplay");
            context.ReportFailure("first reason");
            context.ReportFailure("second reason");
        },
    }));
    ASSERT_TRUE(scheduler.Register({
        .name = "must-not-run",
        .phase = RVX::ECS::ProcessorPhase::PostSimulation,
        .run = [&executions](RVX::ECS::Registry&) { executions.emplace_back("must-not-run"); },
    }));

    EXPECT_FALSE(scheduler.RunVariable(registry, 1.0 / 30.0, 17));
    EXPECT_EQ(executions, (std::vector<std::string>{"failing-gameplay"}));

    const std::optional<RVX::ECS::ProcessorExecutionFailure>& failure = scheduler.GetLastExecutionFailure();
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->processorName, "failing-gameplay");
    EXPECT_EQ(failure->phase, RVX::ECS::ProcessorPhase::Gameplay);
    EXPECT_EQ(failure->group, "Gameplay");
    EXPECT_EQ(failure->stepMode, RVX::ECS::ProcessorStepMode::Variable);
    EXPECT_DOUBLE_EQ(failure->deltaSeconds, 1.0 / 30.0);
    EXPECT_EQ(failure->frameSequence, 17u);
    EXPECT_EQ(failure->fixedStepSequence, 0u);
    EXPECT_EQ(failure->reason, "first reason");
}

TEST(EcsSchedulerValidation, FixedProcessorFailureReportsItsFixedStepContext)
{
    RVX::ECS::Registry registry;
    RVX::ECS::ProcessorScheduler scheduler;

    ASSERT_TRUE(scheduler.Register({
        .name = "failing-physics",
        .phase = RVX::ECS::ProcessorPhase::PhysicsSimulation,
        .stepMode = RVX::ECS::ProcessorStepMode::Fixed,
        .runWithContext = [](RVX::ECS::ProcessorExecutionContext& context)
        {
            context.ReportFailure("physics advance failed");
        },
    }));

    EXPECT_FALSE(scheduler.RunFixed(registry, 1.0 / 120.0, 91));
    const std::optional<RVX::ECS::ProcessorExecutionFailure>& failure = scheduler.GetLastExecutionFailure();
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->processorName, "failing-physics");
    EXPECT_EQ(failure->phase, RVX::ECS::ProcessorPhase::PhysicsSimulation);
    EXPECT_EQ(failure->stepMode, RVX::ECS::ProcessorStepMode::Fixed);
    EXPECT_DOUBLE_EQ(failure->deltaSeconds, 1.0 / 120.0);
    EXPECT_EQ(failure->frameSequence, 0u);
    EXPECT_EQ(failure->fixedStepSequence, 91u);
    EXPECT_EQ(failure->reason, "physics advance failed");
}

TEST(EcsSchedulerValidation, SuccessfulStepClearsThePreviousExecutionFailure)
{
    RVX::ECS::Registry registry;
    RVX::ECS::ProcessorScheduler scheduler;

    ASSERT_TRUE(scheduler.Register({
        .name = "conditionally-failing",
        .runWithContext = [](RVX::ECS::ProcessorExecutionContext& context)
        {
            if (context.frameSequence == 1)
            {
                context.ReportFailure("transient failure");
            }
        },
    }));

    EXPECT_FALSE(scheduler.RunVariable(registry, 1.0 / 60.0, 1));
    ASSERT_TRUE(scheduler.GetLastExecutionFailure().has_value());
    EXPECT_TRUE(scheduler.RunVariable(registry, 1.0 / 60.0, 2));
    EXPECT_FALSE(scheduler.GetLastExecutionFailure().has_value());
}

TEST(EcsSchedulerValidation, CompileReportsConservativeFragmentAndResourceConflicts)
{
    RVX::ECS::ProcessorScheduler scheduler;
    const std::type_index position = typeid(PositionFragment);
    const std::type_index velocity = typeid(VelocityFragment);
    const std::type_index physicsWorld = typeid(PhysicsWorldResource);

    EXPECT_TRUE(scheduler.Register({
        .name = "writer",
        .access = {
            .reads = {velocity},
            .writes = {position},
            .resourceWrites = {physicsWorld},
        },
        .run = [](RVX::ECS::Registry&) {},
    }));
    EXPECT_TRUE(scheduler.Register({
        .name = "reader",
        .access = {
            .reads = {position},
            .resourceReads = {physicsWorld},
        },
        .run = [](RVX::ECS::Registry&) {},
    }));
    EXPECT_TRUE(scheduler.Register({
        .name = "competing-writer",
        .access = {
            .writes = {position},
        },
        .run = [](RVX::ECS::Registry&) {},
    }));

    EXPECT_TRUE(scheduler.Compile());
    const std::vector<RVX::ECS::ProcessorConflict>& conflicts = scheduler.GetConflictDiagnostics();
    ASSERT_EQ(conflicts.size(), 4u);
    uint32_t fragmentReadWriteCount = 0;
    uint32_t fragmentWriteWriteCount = 0;
    uint32_t resourceReadWriteCount = 0;
    for (const RVX::ECS::ProcessorConflict& conflict : conflicts)
    {
        if (conflict.domain == RVX::ECS::ProcessorAccessDomain::Fragment
            && conflict.kind == RVX::ECS::ProcessorConflictKind::ReadWrite)
        {
            ++fragmentReadWriteCount;
        }
        else if (conflict.domain == RVX::ECS::ProcessorAccessDomain::Fragment
            && conflict.kind == RVX::ECS::ProcessorConflictKind::WriteWrite)
        {
            ++fragmentWriteWriteCount;
        }
        else if (conflict.domain == RVX::ECS::ProcessorAccessDomain::Resource
            && conflict.kind == RVX::ECS::ProcessorConflictKind::ReadWrite)
        {
            ++resourceReadWriteCount;
        }
    }
    EXPECT_EQ(fragmentReadWriteCount, 2u);
    EXPECT_EQ(fragmentWriteWriteCount, 1u);
    EXPECT_EQ(resourceReadWriteCount, 1u);
}
