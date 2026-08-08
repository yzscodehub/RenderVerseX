#include "Scene/Components/StaticMeshComponent.h"

#include <gtest/gtest.h>

using namespace RVX;

TEST(SceneLinkClosureValidation, StaticMeshSceneDataLinksWithoutRenderContracts)
{
    StaticMeshComponent component;
    EXPECT_FALSE(component.HasRenderData());
}
