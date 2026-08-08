#include "Scene/SceneSystemScheduler.h"

#include <algorithm>

namespace RVX
{

SceneSystemHandle SceneSystemScheduler::Register(std::string name,
                                                 SceneUpdatePhase phase,
                                                 SceneSystemCallback callback,
                                                 int32 order)
{
    if (!callback)
        return InvalidSceneSystemHandle;

    Entry entry;
    entry.handle = m_handles.Allocate();
    entry.name = std::move(name);
    entry.phase = phase;
    entry.callback = std::move(callback);
    entry.order = order;
    entry.registrationSequence = m_nextRegistrationSequence++;
    m_entries.push_back(std::move(entry));
    return m_entries.back().handle;
}

bool SceneSystemScheduler::Unregister(SceneSystemHandle handle)
{
    auto it = std::find_if(m_entries.begin(), m_entries.end(),
                           [handle](const Entry& entry) {
                               return entry.handle == handle;
                           });
    if (it == m_entries.end())
        return false;

    m_handles.Free(handle);
    m_entries.erase(it);
    return true;
}

void SceneSystemScheduler::Execute(SceneUpdatePhase phase, float deltaTime)
{
    std::vector<Entry> snapshot;
    for (const Entry& entry : m_entries)
    {
        if (entry.phase == phase && entry.callback)
            snapshot.push_back(entry);
    }

    std::stable_sort(snapshot.begin(), snapshot.end(),
                     [](const Entry& lhs, const Entry& rhs) {
                         if (lhs.order != rhs.order)
                             return lhs.order < rhs.order;
                         return lhs.registrationSequence < rhs.registrationSequence;
                     });
    for (const Entry& entry : snapshot)
    {
        if (m_handles.IsValid(entry.handle) && entry.callback)
            entry.callback(deltaTime);
    }
}

void SceneSystemScheduler::Clear()
{
    for (const Entry& entry : m_entries)
        m_handles.Free(entry.handle);
    m_entries.clear();
}

} // namespace RVX
