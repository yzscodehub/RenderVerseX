# =============================================================================
# Scripting Module - Lua scripting integration with sol2
# =============================================================================
add_library(RVX_Scripting STATIC)

# MSVC needs /bigobj for sol2 bindings due to large template instantiations
if(MSVC)
    target_compile_options(RVX_Scripting PRIVATE /bigobj)
endif()

target_sources(RVX_Scripting PRIVATE
    Private/LuaState.cpp
    Private/ScriptEngine.cpp
    Private/ScriptComponent.cpp
    Private/Bindings/CoreBindings.cpp
    Private/Bindings/MathBindings.cpp
    Private/Bindings/SceneBindings.cpp
    Private/Bindings/InputBindings.cpp
)

target_include_directories(RVX_Scripting PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

# Find Lua (vcpkg provides lua as unofficial::lua::lua)
find_package(lua CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)

target_link_libraries(RVX_Scripting PUBLIC
    RVX::Core
    RVX::Scene
)

# Link Lua - try different target names used by vcpkg
if(TARGET lua::lua)
    target_link_libraries(RVX_Scripting PUBLIC lua::lua)
elseif(TARGET lua)
    target_link_libraries(RVX_Scripting PUBLIC lua)
else()
    # Fallback to find_package variables
    find_package(Lua REQUIRED)
    target_link_libraries(RVX_Scripting PUBLIC ${LUA_LIBRARIES})
    target_include_directories(RVX_Scripting PRIVATE ${LUA_INCLUDE_DIR})
endif()

target_link_libraries(RVX_Scripting PUBLIC sol2)

target_include_directories(RVX_Scripting PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Runtime/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/HAL/Include>
)

# Alias for cleaner target names
add_library(RVX::Scripting ALIAS RVX_Scripting)
