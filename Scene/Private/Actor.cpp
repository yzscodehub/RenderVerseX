#include "Scene/Actor.h"
#include "Scene/SceneComponent.h"

#include <algorithm>

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
    if (!rootComponent || rootComponent->GetOwner() == this)
    {
        m_rootComponent = rootComponent;
    }
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
    if (m_hasBegunPlay)
        return;

    m_hasBegunPlay = true;
    for (auto& component : m_components)
    {
        if (component && component->IsEnabled() && !component->HasBegunPlay())
        {
            component->SetHasBegunPlay(true);
            component->BeginPlay();
        }
    }
}

void Actor::Tick(float deltaTime)
{
    if (!m_active)
        return;

    for (auto& component : m_components)
    {
        if (component && component->IsEnabled() && component->CanEverTick() && component->IsTickEnabled())
        {
            component->TickComponent(deltaTime);
        }
    }
}

void Actor::EndPlay()
{
    if (!m_hasBegunPlay)
        return;

    for (auto it = m_components.rbegin(); it != m_components.rend(); ++it)
    {
        ActorComponent* component = it->get();
        if (component && component->HasBegunPlay())
        {
            component->EndPlay();
            component->SetHasBegunPlay(false);
        }
    }
    m_hasBegunPlay = false;
}

bool Actor::RemoveComponentInstance(ActorComponent* component)
{
    auto it = std::find_if(m_components.begin(), m_components.end(), [component](const auto& ownedComponent) {
        return ownedComponent.get() == component;
    });

    if (it == m_components.end())
        return false;

    if (component->HasBegunPlay())
    {
        component->EndPlay();
        component->SetHasBegunPlay(false);
    }
    if (component->IsRegistered())
    {
        component->OnUnregister();
        component->SetRegistered(false);
    }
    if (component == static_cast<ActorComponent*>(m_rootComponent))
    {
        m_rootComponent = nullptr;
    }

    component->OnComponentDestroyed();
    component->SetOwnerActor(nullptr);
    m_components.erase(it);
    return true;
}

void Actor::DestroyAllComponents()
{
    EndPlay();
    UnregisterAllComponents();

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
