cmake_minimum_required(VERSION 3.20)
project(GTAAdvanceRecomp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Framework and menu checkouts: siblings of this folder's parent
# (../gbarecomp, ../recomp-ui), with patches/ applied to gbarecomp. On the
# author's machine they are shared with the Duke Nukem Advance project.
set(_deps "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(NOT EXISTS "${_deps}/gbarecomp/CMakeLists.txt")
    set(_deps "${CMAKE_CURRENT_SOURCE_DIR}/../../DukeAdvance")
endif()
set(GBARECOMP_ROOT "${_deps}/gbarecomp"
    CACHE PATH "Path to the gbarecomp framework checkout")

# Shared in-game settings menu (mstan/recomp-ui, MIT). Must be set before the
# framework is added: it switches the runtime's menu adapter on.
set(GBARECOMP_RUNTIME_UI_ROOT "${_deps}/recomp-ui"
    CACHE PATH "recomp-ui checkout")
set(RECOMP_UI_ROOT "${GBARECOMP_RUNTIME_UI_ROOT}" CACHE PATH "")

# Link-cable netplay for the game's own multiplayer modes (framework
# recomp-net adapter). Must be set before the framework is added.
option(GTA_NETPLAY "Build link-cable netplay for the multiplayer modes" ON)
set(GBARECOMP_NETPLAY ${GTA_NETPLAY} CACHE BOOL "" FORCE)

# Framework: runtime, hardware model, debug server. Tools/tests stay unbuilt.
add_subdirectory("${GBARECOMP_ROOT}" gbarecomp EXCLUDE_FROM_ALL)
include("${CMAKE_CURRENT_BINARY_DIR}/gbarecomp/runtime.cmake")

gbarecomp_add_runtime_target(GTAAdvanceRecomp
    GAME_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated"
    DEBUG_PORT 19851
    WINDOW_TITLE "Grand Theft Auto Advance"
    DEFAULT_GAME_CONFIG_PATH "game.toml"
)
target_sources(GTAAdvanceRecomp PRIVATE src/main.cpp src/pc_controls.cpp
    src/scene_host.cpp src/scene/scene.cpp src/menu_mouse.cpp)
target_include_directories(GTAAdvanceRecomp PRIVATE
    "${RECOMP_UI_ROOT}/src" ${SDL2_INCLUDE_DIR})
# The runtime archives reference each other; GNU ld needs them grouped.
target_link_gbarecomp_runtime_stack(GTAAdvanceRecomp)
if(GTA_NETPLAY)
    target_link_libraries(GTAAdvanceRecomp PRIVATE gbarecomp_netplay)
    gbarecomp_target_netplay_identity(GTAAdvanceRecomp "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated")
    target_compile_definitions(GTAAdvanceRecomp PRIVATE GTA_NETPLAY=1)
    # Online rooms (a room server's relay carries the match).
    target_sources(GTAAdvanceRecomp PRIVATE src/online/room_client.cpp
        src/online/relay_bridge.cpp src/online/online_match.cpp src/online/online_ui.cpp)
    target_include_directories(GTAAdvanceRecomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(WIN32)
        target_link_libraries(GTAAdvanceRecomp PRIVATE ws2_32)
    endif()
endif()

# Dear ImGui core + SDL2 backend + the menu model. recomp-ui is C++17 while
# the generated cartridge code is C++20, so it gets its own static library.
# (recomp_ui.cmake is included again here: the framework included it in its
# own directory scope, which hides recomp-ui's asset tables from this one.)
include("${RECOMP_UI_ROOT}/recomp_ui.cmake")
add_library(game_ui STATIC src/game_ui.cpp)
recomp_target_launcher_ui(game_ui CONSOLE gba)
target_include_directories(game_ui PRIVATE ${SDL2_INCLUDE_DIR})
target_link_libraries(game_ui PUBLIC ${SDL2_LIBRARY})
if(WIN32)
    target_link_libraries(game_ui PUBLIC opengl32)
endif()
target_link_libraries(GTAAdvanceRecomp PRIVATE game_ui)
recomp_stage_launcher_assets(GTAAdvanceRecomp CONSOLE gba)

# runtime.cpp reads the title at its own compile time.
target_compile_definitions(gbarecomp_runtime PRIVATE
    GBARECOMP_WINDOW_TITLE="Grand Theft Auto Advance")

gbarecomp_stage_mingw_runtime(GTAAdvanceRecomp)

# Runtime config sits next to the exe.
add_custom_command(TARGET GTAAdvanceRecomp POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/game.toml"
        "$<TARGET_FILE_DIR:GTAAdvanceRecomp>/game.toml"
    VERBATIM)
