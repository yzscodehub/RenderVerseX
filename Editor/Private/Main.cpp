/**
 * @file Main.cpp
 * @brief RenderVerseX Editor entry point
 */

#include "Editor/Editor.h"
#include "Editor/EditorApplication.h"
#include "Core/Log.h"

#include <iostream>
#include <exception>

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    try
    {
        // Initialize core logging
        RVX::Log::Initialize();
        RVX_CORE_INFO("Starting RenderVerseX Editor...");

        // Create and run the editor application
        RVX::Editor::EditorApplication app;
        
        if (!app.Initialize())
        {
            RVX_CORE_ERROR("Failed to initialize editor application");
            return -1;
        }

        // Run the main loop
        int result = app.Run();

        // Cleanup
        app.Shutdown();
        RVX_CORE_INFO("Editor shutdown complete");

        return result;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return -1;
    }
}
