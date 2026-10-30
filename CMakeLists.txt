cmake_minimum_required(VERSION 3.21)
project(DeadlockBuildHelper VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Windows only: uses DXGI Desktop Duplication and DirectX 11.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# All three come from vcpkg (see vcpkg.json) when configured with the vcpkg toolchain.
find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc imgcodecs)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Platform-independent part, shared by the app and the tests.
set(DBH_CORE_SOURCES
    src/Config.cpp
    src/VisionEngine.cpp
    src/HeroRecognizer.cpp
    src/AbilityBar.cpp
    src/CounterDatabase.cpp
    src/PlayerAnalysis.cpp
)
set(DBH_DEFINITIONS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)

add_executable(DeadlockBuildHelper
    src/main.cpp
    ${DBH_CORE_SOURCES}
    src/ScreenCapture.cpp
    src/AnalysisWorker.cpp
    src/PanelView.cpp
    src/SettingsMenu.cpp
    src/Overlay.cpp
)

target_include_directories(DeadlockBuildHelper PRIVATE src)

target_compile_definitions(DeadlockBuildHelper PRIVATE ${DBH_DEFINITIONS})

if(MSVC)
    target_compile_options(DeadlockBuildHelper PRIVATE /W4 /permissive- /utf-8 /MP)
endif()

# OFF: console window with live logs (best while calibrating).
# ON:  no console; logs go to build_helper.log next to the exe.
option(DBH_HIDE_CONSOLE "Build without a console window" OFF)
if(DBH_HIDE_CONSOLE AND MSVC)
    target_link_options(DeadlockBuildHelper PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

target_link_libraries(DeadlockBuildHelper PRIVATE
    ${OpenCV_LIBS}
    imgui::imgui
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    dwmapi
)

# Ship config, counters and template icons next to the .exe. On every build, not only after linking:
# icons downloaded later have to get there too.
add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:DeadlockBuildHelper>/assets
)
add_dependencies(copy_assets DeadlockBuildHelper)

# Logic + vision tests (synthetic scoreboards, no game needed):
#   cmake --preset default -DDBH_BUILD_TESTS=ON && cmake --build --preset release && ctest --test-dir build
option(DBH_BUILD_TESTS "Build the logic and vision tests" OFF)
if(DBH_BUILD_TESTS)
    enable_testing()
    foreach(test logic_tests vision_tests)
        add_executable(${test} tests/${test}.cpp ${DBH_CORE_SOURCES})
        target_include_directories(${test} PRIVATE src)
        target_compile_definitions(${test} PRIVATE ${DBH_DEFINITIONS})
        target_link_libraries(${test} PRIVATE ${OpenCV_LIBS} nlohmann_json::nlohmann_json)
        if(MSVC)
            target_compile_options(${test} PRIVATE /utf-8 /permissive-)
        endif()
        add_test(NAME ${test} COMMAND ${test} ${CMAKE_SOURCE_DIR}/assets)
    endforeach()

    # Not a ctest test: runs the whole recognition pipeline on a screenshot you saved, e.g.
    #   build\screenshot_check.exe shot.png build\assets --allies --out checked.png
    add_executable(screenshot_check tests/screenshot_check.cpp ${DBH_CORE_SOURCES})
    target_include_directories(screenshot_check PRIVATE src)
    target_compile_definitions(screenshot_check PRIVATE ${DBH_DEFINITIONS})
    target_link_libraries(screenshot_check PRIVATE ${OpenCV_LIBS} nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(screenshot_check PRIVATE /utf-8 /permissive-)
    endif()
endif()
