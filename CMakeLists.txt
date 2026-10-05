cmake_minimum_required(VERSION 3.20)
project(NonstopOverlay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable Unicode for Win32
add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN)

include(FetchContent)

# 1. Fetch Dear ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    )
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)
endif()

# 2. Fetch PFFFT (BSD SIMD FFT)
set(PFFFT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PFFFT_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    pffft
    GIT_REPOSITORY https://github.com/marton78/pffft.git
    GIT_TAG        aa16fd3db58de4ba5dae8b0438440bb9da46b6fa
)
FetchContent_MakeAvailable(pffft)

# 3. Fetch nlohmann/json
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# Nonstop Source Files
set(NONSTOP_SOURCES
    src/main.cpp
    src/audio/wasapi_capture.cpp
    src/audio/wasapi_capture.h
    src/audio/fft_analyzer.cpp
    src/audio/fft_analyzer.h
    src/audio/phase_detector.cpp
    src/audio/phase_detector.h
    src/audio/ring_buffer.h
    src/overlay/overlay_window.cpp
    src/overlay/overlay_window.h
    src/overlay/screen_capturer.cpp
    src/overlay/screen_capturer.h
    src/core/hotkey_manager.cpp
    src/core/hotkey_manager.h
    src/core/types.h
    src/ui/mini_window.cpp
    src/ui/mini_window.h
    src/ui/theme.cpp
    src/ui/theme.h
)

add_executable(nonstop_diagnostics WIN32 ${NONSTOP_SOURCES})

target_include_directories(nonstop_diagnostics PRIVATE
    src
    ${pffft_SOURCE_DIR}/include/pffft
    ${pffft_SOURCE_DIR}/include
)

target_link_libraries(nonstop_diagnostics PRIVATE
    imgui
    PFFFT
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    ole32
    oleaut32
    avrt
    winmm
)

# Output directory settings
set_target_properties(nonstop_diagnostics PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
