#include "Core/Core.h"
#include "Render/Renderer/RenderScene.h"
#include "Runtime/Camera/Camera.h"
#include "TestFramework/TestRunner.h"
#include <vector>

using namespace RVX;
using namespace RVX::Test;

namespace
{
    RenderObject MakeObject(const Vec3& center, float extent = 0.5f)
    {
        RenderObject object;
        object.bounds = AABB(center - Vec3(extent), center + Vec3(extent));
        object.visible = true;
        return object;
    }

    Camera MakeTestCamera()
    {
        Camera camera;
        camera.SetPerspective(1.04719755f, 1.0f, 0.1f, 100.0f);
        camera.SetPosition(Vec3(0.0f, 0.0f, 5.0f));
        camera.LookAt(Vec3(0.0f, 0.0f, 0.0f));
        return camera;
    }
} // namespace

bool Test_CullAgainstCameraRejectsOutsideObjects()
{
    RenderScene scene;
    scene.AddObject(MakeObject(Vec3(0.0f, 0.0f, 0.0f)));
    scene.AddObject(MakeObject(Vec3(100.0f, 0.0f, 0.0f)));

    std::vector<uint32_t> visibleIndices;
    scene.CullAgainstCamera(MakeTestCamera(), visibleIndices);

    TEST_ASSERT_EQ(visibleIndices.size(), 1);
    TEST_ASSERT_EQ(visibleIndices[0], 0u);
    return true;
}

bool Test_CullAgainstCameraHonorsVisibilityFlag()
{
    RenderScene scene;
    RenderObject hidden = MakeObject(Vec3(0.0f, 0.0f, 0.0f));
    hidden.visible = false;
    scene.AddObject(hidden);

    std::vector<uint32_t> visibleIndices;
    scene.CullAgainstCamera(MakeTestCamera(), visibleIndices);

    TEST_ASSERT_TRUE(visibleIndices.empty());
    return true;
}

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("RenderScene Validation Tests");

    TestSuite suite;
    suite.AddTest("CullAgainstCameraRejectsOutsideObjects", Test_CullAgainstCameraRejectsOutsideObjects);
    suite.AddTest("CullAgainstCameraHonorsVisibilityFlag", Test_CullAgainstCameraHonorsVisibilityFlag);

    auto results = suite.Run();
    suite.PrintResults(results);

    Log::Shutdown();

    for (const auto& result : results)
    {
        if (!result.passed)
            return 1;
    }

    return 0;
}
