cmake_minimum_required(VERSION 3.20)
project(VortexForge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_WITH_IMGUI "Build with Dear ImGui + GLFW UI" OFF)
option(USE_SOURCEPP "Link against sourcepp library if installed" OFF)

add_executable(valve_converter
    src/main.cpp
)

set_target_properties(valve_converter PROPERTIES
    OUTPUT_NAME "VortexForge"
)

target_compile_definitions(valve_converter PRIVATE
    VORTEXFORGE_VERSION="${PROJECT_VERSION}"
    VORTEXFORGE_GITHUB_REPO="lynx228/VortexForge"
)

target_include_directories(valve_converter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

if(USE_SOURCEPP)
    find_package(sourcepp CONFIG QUIET)
    if(sourcepp_FOUND)
        target_link_libraries(valve_converter PRIVATE sourcepp::vpk sourcepp::vtf sourcepp::bsp sourcepp::mdl)
        target_compile_definitions(valve_converter PRIVATE HAS_SOURCEPP=1)
    else()
        message(WARNING "sourcepp not found; building with built-in header/fallback parsers.")
    endif()
endif()

if(BUILD_WITH_IMGUI)
    find_package(glfw3 CONFIG REQUIRED)
    find_package(OpenGL REQUIRED)

    set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui" CACHE PATH "Path to Dear ImGui sources")

    set(IMGUI_SOURCES
        "${IMGUI_DIR}/imgui.cpp"
        "${IMGUI_DIR}/imgui_draw.cpp"
        "${IMGUI_DIR}/imgui_tables.cpp"
        "${IMGUI_DIR}/imgui_widgets.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
    )

    foreach(imgui_src IN LISTS IMGUI_SOURCES)
        if(NOT EXISTS "${imgui_src}")
            message(FATAL_ERROR "Dear ImGui source file not found: ${imgui_src}")
        endif()
    endforeach()

    target_sources(valve_converter PRIVATE ${IMGUI_SOURCES})
    target_include_directories(valve_converter PRIVATE
        "${IMGUI_DIR}"
        "${IMGUI_DIR}/backends"
    )
    target_link_libraries(valve_converter PRIVATE glfw OpenGL::GL)
    target_compile_definitions(valve_converter PRIVATE BUILD_WITH_IMGUI=1)

    if(WIN32)
        set_target_properties(valve_converter PROPERTIES WIN32_EXECUTABLE TRUE)
        target_link_options(valve_converter PRIVATE "/ENTRY:mainCRTStartup")
    endif()
endif()

if(WIN32)
    target_compile_definitions(valve_converter PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(valve_converter PRIVATE winhttp)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.rc")
        target_sources(valve_converter PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.rc")
    endif()
endif()

# Автокопирование папки assets рядом с VortexForge.exe после каждой сборки
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    add_custom_command(TARGET valve_converter POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:valve_converter>/assets"
        COMMENT "Copying assets folder to output directory..."
    )
endif()
