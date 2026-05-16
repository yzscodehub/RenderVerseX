#include "Scene/Actor.h"
#include "Scene/Component.h"
#include "Scene/SceneComponent.h"

#include <algorithm>
#include <utility>

namespace RVX
{

std::atomic<Actor::Handle> Actor::s_nextHandle{1};

Actor::Handle Actor::GenerateHandle()
{
    return s_nextHandle.fetch_add(1, std::memory_order_relaxed);
}

Actor::Actor(const std::string& name)
    : m_handle(GenerateHandle())
    , m_name(name)
{
}

Actor::~Actor()
{
    DestroyAllComponents();
}

void Actor::SetRootComponent(SceneComponent* rootComponent)
{
    if (rootComponent && rootComponent->GetOwner() != this)
        return;

    if (m_rootComponent == rootComponent)
        return;

    if (m_rootComponent)
    {
        m_rootComponent->DetachFromComponent();
    }

    m_rootComponent = rootComponent;
}

Vec3 Actor::GetWorldPosition() const
{
    return m_rootComponent ? m_rootComponent->GetWorldLocation() : Vec3(0.0f);
}

Quat Actor::GetWorldRotation() const
{
    return m_rootComponent ? m_rootComponent->GetWorldRotation() : Quat(1.0f, 0.0f, 0.0f, 0.0f);
}

Vec3 Actor::GetWorldScale() const
{
    return m_rootComponent ? m_rootComponent->GetWorldScale() : Vec3(1.0f);
}

Mat4 Actor::GetWorldMatrix() const
{
    return m_rootComponent ? m_rootComponent->GetWorldTransform() : Mat4(1.0f);
}

void Actor::SetPosition(const Vec3& position)
{
    if (m_rootComponent)
    {
        m_rootComponent->SetRelativeLocation(position);
    }
}

void Actor::SetRotation(const Quat& rotation)
{
    if (m_rootComponent)
    {
        m_rootComponent->SetRelativeRotation(rotation);
    }
}

void Actor::SetScale(const Vec3& scale)
{
    if (m_rootComponent)
    {
        m_rootComponent->SetRelativeScale(scale);
    }
}

void Actor::Translate(const Vec3& delta)
{
    if (m_rootComponent)
    {
        m_rootComponent->SetRelativeLocation(m_rootComponent->GetRelativeLocation() + delta);
    }
}

ActorComponent* Actor::AddOwnedComponent(std::unique_ptr<ActorComponent> component)
{
    if (!component)
        return nullptr;

    // Legacy Component subclasses must enter through SceneEntity so their
    // SceneEntity owner and legacy component map remain coherent.
    if (dynamic_cast<Component*>(component.get()))
        return nullptr;

    ActorComponent* ptr = component.get();
    ptr->SetOwnerActor(this);
    m_components.push_back(std::move(component));
    ptr->OnComponentCreated();

    if (ShouldAutoRegisterComponent(ptr))
    {
        ptr->SetRegistered(true);
        ptr->OnRegister();
        if (!ptr->IsInitialized())
        {
            ptr->InitializeComponent();
            ptr->SetInitialized(true);
        }
    }

    return ptr;
}

void Actor::RegisterAllComponents()
{
    for (auto& component : m_components)
    {
        if (!component || component->IsRegistered())
            continue;

        component->SetRegistered(true);
        component->OnRegister();
        if (!component->IsInitialized())
        {
            component->InitializeComponent();
            component->SetInitialized(true);
        }
    }
}

void Actor::UnregisterAllComponents()
{
    for (auto it = m_components.rbegin(); it != m_components.rend(); ++it)
    {
        ActorComponent* component = it->get();
        if (!component || !component->IsRegistered())
            continue;

        component->OnUnregister();
        component->SetRegistered(false);
    }
}

void Actor::BeginPlay()
{
    if (!m_hasBegunPlay)
    {
        m_hasBegunPlay = true;
    }

    auto snapshot = MakeComponentSnapshot();
    BeginComponentDispatch();
    for (ActorComponent* component : snapshot)
    {
        if (component && !IsComponentRemovalPending(component) &&
            component->IsEnabled() && !component->HasBegunPlay())
        {
            component->SetHasBegunPlay(true);
            component->BeginPlay();
        }
    }
    EndComponentDispatch();
}

void Actor::Tick(float deltaTime)
{
    if (!IsActive())
        return;

    auto snapshot = MakeComponentSnapshot();
    BeginComponentDispatch();
    for (ActorComponent* component : snapshot)
    {
        if (component && !IsComponentRemovalPending(component) &&
            component->IsEnabled() && component->CanEverTick() && component->IsTickEnabled())
        {
            component->TickComponent(deltaTime);
        }
    }
    EndComponentDispatch();
}

void Actor::EndPlay()
{
    if (!m_hasBegunPlay)
        return;

    auto snapshot = MakeComponentSnapshot();
    BeginComponentDispatch(true);
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
    {
        ActorComponent* component = *it;
        if (component && !IsComponentRemovalPending(component) && component->HasBegunPlay())
        {
            component->EndPlay();
            component->SetHasBegunPlay(false);
        }
    }
    EndComponentDispatch(true);
    m_hasBegunPlay = false;
}

bool Actor::ShouldAutoRegisterComponent(ActorComponent* component) const
{
    (void)component;
    return false;
}

bool Actor::RemoveComponentInstance(ActorComponent* component)
{
    if (!component)
        return false;

    if (component == static_cast<ActorComponent*>(m_rootComponent))
        return false;

    auto it = std::find_if(m_components.begin(), m_components.end(), [component](const auto& ownedComponent) {
        return ownedComponent.get() == component;
    });

    if (it == m_components.end())
        return false;

    if (m_componentDispatchDepth > 0)
    {
        QueuePendingComponentRemoval(component);
        return IsComponentRemovalPending(component);
    }

    return RemoveComponentInstanceNow(component);
}

bool Actor::RemoveComponentInstanceNow(ActorComponent* component)
{
    auto it = std::find_if(m_components.begin(), m_components.end(), [component](const auto& ownedComponent) {
        return ownedComponent.get() == component;
    });

    if (it == m_components.end())
        return false;

    if (component == static_cast<ActorComponent*>(m_rootComponent))
        return false;

    std::unique_ptr<ActorComponent> removedComponent = std::move(*it);
    m_components.erase(it);

    ActorComponent* removed = removedComponent.get();
    if (removed->HasBegunPlay())
    {
        removed->SetHasBegunPlay(false);
        removed->EndPlay();
    }
    if (removed->IsRegistered())
    {
        removed->OnUnregister();
        removed->SetRegistered(false);
    }
    removed->OnComponentDestroyed();
    removed->SetOwnerActor(nullptr);
    return true;
}

bool Actor::IsComponentRemovalPending(const ActorComponent* component) const
{
    return std::find_if(m_pendingRemoveComponents.begin(),
                        m_pendingRemoveComponents.end(),
                        [component](const PendingComponentRemoval& pending) {
                            return pending.component == component;
                        }) != m_pendingRemoveComponents.end();
}

void Actor::QueuePendingComponentRemoval(ActorComponent* component)
{
    if (!component)
        return;

    if (component == static_cast<ActorComponent*>(m_rootComponent))
        return;

    const bool dispatchEndPlay =
        component->HasBegunPlay() && m_componentEndPlayDispatchDepth == 0 && m_hasBegunPlay;
    auto it = std::find_if(m_pendingRemoveComponents.begin(),
                           m_pendingRemoveComponents.end(),
                           [component](const PendingComponentRemoval& pending) {
                               return pending.component == component;
                           });
    if (it != m_pendingRemoveComponents.end())
    {
        it->dispatchEndPlay = it->dispatchEndPlay || dispatchEndPlay;
        return;
    }

    m_pendingRemoveComponents.push_back({ component, dispatchEndPlay });
}

void Actor::FlushPendingComponentRemovals()
{
    if (m_pendingRemoveComponents.empty())
        return;

    auto pending = std::move(m_pendingRemoveComponents);
    m_pendingRemoveComponents.clear();

    for (const PendingComponentRemoval& removal : pending)
    {
        ActorComponent* component = removal.component;
        if (component && component->HasBegunPlay())
        {
            if (!removal.dispatchEndPlay)
            {
                component->SetHasBegunPlay(false);
            }
        }
        RemoveComponentInstanceNow(component);
    }
}

std::vector<ActorComponent*> Actor::MakeComponentSnapshot() const
{
    std::vector<ActorComponent*> snapshot;
    snapshot.reserve(m_components.size());
    for (const auto& component : m_components)
    {
        if (component)
        {
            snapshot.push_back(component.get());
        }
    }
    return snapshot;
}

void Actor::BeginComponentDispatch(bool dispatchingEndPlay)
{
    ++m_componentDispatchDepth;
    if (dispatchingEndPlay)
    {
        ++m_componentEndPlayDispatchDepth;
    }
}

void Actor::EndComponentDispatch(bool dispatchingEndPlay)
{
    if (m_componentDispatchDepth <= 0)
        return;

    if (dispatchingEndPlay && m_componentEndPlayDispatchDepth > 0)
    {
        --m_componentEndPlayDispatchDepth;
    }

    --m_componentDispatchDepth;
    if (m_componentDispatchDepth == 0)
    {
        FlushPendingComponentRemovals();
    }
}

void Actor::DestroyAllComponents()
{
    EndPlay();
    UnregisterAllComponents();

    m_pendingRemoveComponents.clear();
    m_componentDispatchDepth = 0;
    m_componentEndPlayDispatchDepth = 0;

    for (auto it = m_components.rbegin(); it != m_components.rend(); ++it)
    {
        ActorComponent* component = it->get();
        if (!component)
            continue;

        component->OnComponentDestroyed();
        component->SetOwnerActor(nullptr);
    }
    m_rootComponent = nullptr;
    m_components.clear();
}

} // namespace RVX
