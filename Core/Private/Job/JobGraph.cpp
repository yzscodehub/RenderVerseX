/**
 * @file JobGraph.cpp
 * @brief JobGraph implementation
 */

#include "Core/Job/JobGraph.h"
#include <algorithm>
#include <queue>

namespace RVX
{

// ============================================================================
// JobNode
// ============================================================================

JobNode::JobNode(const std::string& name)
    : m_name(name)
{
}

JobNode::JobNode(WorkFunction work, const std::string& name)
    : m_name(name)
    , m_work(std::move(work))
{
}

void JobNode::DependsOn(JobNode::Ptr dependency)
{
    if (!dependency) return;
    
    m_dependencies.push_back(dependency);
    dependency->m_successors.push_back(this);
}

void JobNode::DependsOn(const std::vector<JobNode::Ptr>& dependencies)
{
    for (const auto& dep : dependencies)
    {
        DependsOn(dep);
    }
}

void JobNode::Then(JobNode::Ptr successor)
{
    if (!successor) return;
    
    successor->m_dependencies.push_back(
        std::static_pointer_cast<JobNode>(shared_from_this()));
    m_successors.push_back(successor.get());
}

bool JobNode::IsReady() const
{
    return m_pendingDependencies.load() == 0 && !m_scheduled.load();
}

void JobNode::Reset()
{
    m_completed.store(false);
    m_scheduled.store(false);
    m_pendingDependencies.store(static_cast<int>(m_dependencies.size()));
}

void JobNode::Execute()
{
    if (m_work)
    {
        m_work();
    }
    m_completed.store(true);
}

void JobNode::OnDependencyComplete()
{
    m_pendingDependencies.fetch_sub(1);
}

// ============================================================================
// JobGraph
// ============================================================================

JobNode::Ptr JobGraph::AddJob(const std::string& name, JobNode::WorkFunction work)
{
    auto node = JobNode::Create(std::move(work), name);
    AddJob(node);
    return node;
}

void JobGraph::AddJob(JobNode::Ptr node)
{
    if (!node) return;
    
    m_jobs.push_back(node);
    if (!node->GetName().empty())
    {
        m_jobLookup[node->GetName()] = node;
    }
}

JobNode::Ptr JobGraph::FindJob(const std::string& name) const
{
    auto it = m_jobLookup.find(name);
    return (it != m_jobLookup.end()) ? it->second : nullptr;
}

void JobGraph::Clear()
{
    m_jobs.clear();
    m_jobLookup.clear();
    m_completedCount.store(0);
    m_executing.store(false);
}

void JobGraph::Execute()
{
    if (m_jobs.empty() || m_executing.exchange(true))
        return;

    m_completedCount.store(0);

    // Reset all jobs
    for (auto& job : m_jobs)
    {
        job->Reset();
    }

    // Schedule initially ready jobs
    ScheduleReadyJobs();
}

void JobGraph::ScheduleReadyJobs()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& job : m_jobs)
    {
        if (job->IsReady() && !job->m_scheduled.exchange(true))
        {
            JobSystem::Get().Submit([this, jobPtr = job.get()]() {
                jobPtr->Execute();
                OnJobComplete(jobPtr);
            });
        }
    }
}

void JobGraph::OnJobComplete(JobNode* node)
{
    m_completedCount.fetch_add(1);

    // Notify successors
    for (JobNode* successor : node->GetSuccessors())
    {
        successor->OnDependencyComplete();
    }

    // Schedule newly ready jobs
    ScheduleReadyJobs();
}

void JobGraph::Wait()
{
    while (!IsComplete())
    {
        std::this_thread::yield();
    }
    m_executing.store(false);
}

bool JobGraph::IsComplete() const
{
    return m_completedCount.load() >= m_jobs.size();
}

void JobGraph::Reset()
{
    for (auto& job : m_jobs)
    {
        job->Reset();
    }
    m_completedCount.store(0);
    m_executing.store(false);
}

bool JobGraph::Validate() const
{
    // Check for cycles using DFS
    std::unordered_map<JobNode*, int> color;  // 0=white, 1=gray, 2=black
    
    for (const auto& job : m_jobs)
    {
        color[job.get()] = 0;
    }

    std::function<bool(JobNode*)> hasCycle = [&](JobNode* node) -> bool {
        color[node] = 1;  // Gray
        
        for (const auto& dep : node->GetDependencies())
        {
            if (color[dep.get()] == 1)  // Back edge = cycle
                return true;
            if (color[dep.get()] == 0 && hasCycle(dep.get()))
                return true;
        }
        
        color[node] = 2;  // Black
        return false;
    };

    for (const auto& job : m_jobs)
    {
        if (color[job.get()] == 0 && hasCycle(job.get()))
        {
            return false;
        }
    }

    return true;
}

size_t JobGraph::GetCompletedJobCount() const
{
    size_t count = 0;
    for (const auto& job : m_jobs)
    {
        if (job->IsComplete())
            count++;
    }
    return count;
}

} // namespace RVX
