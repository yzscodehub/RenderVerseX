#include "Scene/Component.h"
#include "Scene/SceneEntity.h"

namespace RVX
{

void Component::SetOwner(SceneEntity* owner)
{
    m_owner = owner;
    SetOwnerActor(owner);
}

void Component::RegisterWithOwner()
{
    if (IsRegistered())
        return;

    SetRegistered(true);
    OnRegister();
    if (!IsInitialized())
    {
        InitializeComponent();
        SetInitialized(true);
    }
}

void Component::UnregisterFromOwner()
{
    if (!IsRegistered())
        return;

    OnUnregister();
    SetRegistered(false);
}

void Component::BeginPlayWithOwner()
{
    if (HasBegunPlay() || !IsEnabled())
        return;

    SetHasBegunPlay(true);
    BeginPlay();
}

void Component::EndPlayWithOwner()
{
    if (!HasBegunPlay())
        return;

    EndPlay();
    SetHasBegunPlay(false);
}

void Component::NotifyBoundsChanged()
{
    if (m_owner)
    {
        m_owner->MarkBoundsDirty();
    }
}

} // namespace RVX
