#include "RenderContracts/RenderProxy.h"
#include "Scene/Components/StaticMeshComponent.h"

#include <gtest/gtest.h>

using namespace RVX;

TEST(SceneLinkClosureValidation, StaticMeshProxyPathLinksWithoutAnimation)
{
    StaticMeshComponent component;
    RenderPrimitiveProxy proxy;

    EXPECT_FALSE(component.CreateRenderProxy(proxy));
}
