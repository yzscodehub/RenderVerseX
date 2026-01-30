/**
 * @file JobGraph.h
 * @brief Job dependency graph for complex task scheduling
 * 
 * Provides a dependency-based job scheduling system where jobs
 * can declare dependencies on other jobs.
 */

#pragma once

#include "Core/Job/JobSystem.h"
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace RVX
{
    class JobGraph;

    /**
     * @brief A node in the job dependency graph
     */
    class JobNode : public std::enable_shared_from_this<JobNode>
    {
    public:
        using Ptr = std::shared_ptr<JobNode>;
        using WorkFunction = std::function<void()>;

        // =========================================================================
        // Construction
        // =========================================================================

        JobNode() = default;
        explicit JobNode(const std::string& name);
        explicit JobNode(WorkFunction work, const std::string& name = "");

        static Ptr Create(const std::string& name = "")
        {
            return std::make_shared<JobNode>(name);
        }

        static Ptr Create(WorkFunction work, const std::string& name = "")
        {
            return std::make_shared<JobNode>(std::move(work), name);
        }

        // =========================================================================
        // Configuration
        // =========================================================================

        /// Set the work function
        void SetWork(WorkFunction work) { m_work = std::move(work); }

        /// Get the name
        const std::string& GetName() const { return m_name; }

        /// Set priority
        void SetPriority(JobPriority priority) { m_priority = priority; }
        JobPriority GetPriority() const { return m_priority; }

        // =========================================================================
        // Dependencies
        // =========================================================================

        /**
         * @brief Add a dependency (this job runs after the dependency)
         */
        void DependsOn(JobNode::Ptr dependency);

        /**
         * @brief Add multiple dependencies
         */
        void DependsOn(const std::vector<JobNode::Ptr>& dependencies);

        /**
         * @brief Make another job depend on this one
         */
        void Then(JobNode::Ptr successor);

        /**
         * @brief Get all dependencies
         */
        const std::vector<JobNode::Ptr>& GetDependencies() const { return m_dependencies; }

        /**
         * @brief Get all successors
         */
        const std::vector<JobNode*>& GetSuccessors() const { return m_successors; }

        // =========================================================================
        // State
        // =========================================================================

        /// Check if ready to execute (all dependencies complete)
        bool IsReady() const;

        /// Check if completed
        bool IsComplete() const { return m_completed.load(); }

        /// Get remaining dependency count
        int GetPendingDependencies() const { return m_pendingDependencies.load(); }

    private:
        friend class JobGraph;

        void Reset();
        void Execute();
        void OnDependencyComplete();

        std::string m_name;
        WorkFunction m_work;
        JobPriority m_priority = JobPriority::Normal;

        std::vector<JobNode::Ptr> m_dependencies;
        std::vector<JobNode*> m_successors;

        std::atomic<int> m_pendingDependencies{0};
        std::atomic<bool> m_completed{false};
        std::atomic<bool> m_scheduled{false};
    };

    /**
     * @brief Job graph for dependency-based scheduling
     * 
     * Manages a set of jobs with dependencies and executes them
     * in the correct order using the JobSystem.
     * 
     * Usage:
     * @code
     * JobGraph graph;
     * 
     * auto loadMesh = graph.AddJob("LoadMesh", []() { LoadMeshFromDisk(); });
     * auto loadTextures = graph.AddJob("LoadTextures", []() { LoadTexturesFromDisk(); });
     * auto createBuffers = graph.AddJob("CreateBuffers", []() { CreateGPUBuffers(); });
     * 
     * createBuffers->DependsOn({loadMesh, loadTextures});
     * 
     * graph.Execute();  // Runs in parallel, respecting dependencies
     * graph.Wait();
     * @endcode
     */
    class JobGraph
    {
    public:
        JobGraph() = default;
        ~JobGraph() = default;

        // Non-copyable
        JobGraph(const JobGraph&) = delete;
        JobGraph& operator=(const JobGraph&) = delete;

        // Movable
        JobGraph(JobGraph&&) = default;
        JobGraph& operator=(JobGraph&&) = default;

        // =========================================================================
        // Building
        // =========================================================================

        /**
         * @brief Add a job to the graph
         * @param name Job name for debugging
         * @param work Work function
         * @return The created job node
         */
        JobNode::Ptr AddJob(const std::string& name, JobNode::WorkFunction work);

        /**
         * @brief Add an existing job node
         */
        void AddJob(JobNode::Ptr node);

        /**
         * @brief Find a job by name
         */
        JobNode::Ptr FindJob(const std::string& name) const;

        /**
         * @brief Get all jobs
         */
        const std::vector<JobNode::Ptr>& GetJobs() const { return m_jobs; }

        /**
         * @brief Clear all jobs
         */
        void Clear();

        // =========================================================================
        // Execution
        // =========================================================================

        /**
         * @brief Execute the job graph
         * 
         * Jobs are scheduled based on their dependencies.
         * Returns immediately; use Wait() to block.
         */
        void Execute();

        /**
         * @brief Wait for all jobs to complete
         */
        void Wait();

        /**
         * @brief Check if all jobs are complete
         */
        bool IsComplete() const;

        /**
         * @brief Reset the graph for re-execution
         */
        void Reset();

        // =========================================================================
        // Debugging
        // =========================================================================

        /**
         * @brief Validate the graph (check for cycles)
         * @return True if valid, false if cycle detected
         */
        bool Validate() const;

        /**
         * @brief Get job count
         */
        size_t GetJobCount() const { return m_jobs.size(); }

        /**
         * @brief Get completed job count
         */
        size_t GetCompletedJobCount() const;

    private:
        void ScheduleReadyJobs();
        void OnJobComplete(JobNode* node);

        std::vector<JobNode::Ptr> m_jobs;
        std::unordered_map<std::string, JobNode::Ptr> m_jobLookup;

        std::mutex m_mutex;
        std::atomic<size_t> m_completedCount{0};
        std::atomic<bool> m_executing{false};
    };

    /**
     * @brief Builder for creating job graphs with fluent API
     */
    class JobGraphBuilder
    {
    public:
        explicit JobGraphBuilder(JobGraph& graph) : m_graph(graph) {}

        /**
         * @brief Add a job
         */
        JobGraphBuilder& Job(const std::string& name, JobNode::WorkFunction work)
        {
            m_lastJob = m_graph.AddJob(name, std::move(work));
            return *this;
        }

        /**
         * @brief Make the last job depend on another
         */
        JobGraphBuilder& DependsOn(const std::string& name)
        {
            if (m_lastJob)
            {
                auto dep = m_graph.FindJob(name);
                if (dep) m_lastJob->DependsOn(dep);
            }
            return *this;
        }

        /**
         * @brief Get the last created job
         */
        JobNode::Ptr GetLastJob() const { return m_lastJob; }

    private:
        JobGraph& m_graph;
        JobNode::Ptr m_lastJob;
    };

} // namespace RVX
