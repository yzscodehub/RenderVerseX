#include "Scene/Actor.h"
#include "Scene/Component.h"
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

bool Actor::ShouldAutoRegisterComponent(ActorComponent* component) const
{
    (void)component;
    return false;
}

bool Actor::RemoveComponentInstance(ActorComponent* component)
{
    auto it = std::find_if(m_components.begin(), m_components.end(), [component](const auto& ownedComponent) {
        return ownedComponent.get() == component;
    });

    if (it == m_components.end())
        return false;

    if (component == static_cast<ActorComponent*>(m_rootComponent))
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
