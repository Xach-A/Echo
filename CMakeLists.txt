cmake_minimum_required(VERSION 3.24)

# Product identity is configurable at configure time.
set(ECHO_APP_NAME "Echo" CACHE STRING "Display name of the application")
set(ECHO_APP_ID "Echo" CACHE STRING "Identifier used for the settings folder and mutex")
set(ECHO_APP_PUBLISHER "Echo contributors" CACHE STRING "Publisher shown in version info")

project(Echo VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT WIN32)
    message(FATAL_ERROR "Echo targets Windows 10/11 x64 only.")
endif()

option(ECHO_BUILD_TESTS "Build automated tests" ON)
option(ECHO_BUILD_TOOLS "Build hardware probe tools" ON)
option(ECHO_SANITIZE "Instrument with AddressSanitizer + UndefinedBehaviorSanitizer (clang; for test runs)" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

configure_file(src/app/Version.h.in ${CMAKE_BINARY_DIR}/generated/Version.h @ONLY)

# ---------------------------------------------------------------------------
# Compiler settings
# ---------------------------------------------------------------------------
add_library(echo_options INTERFACE)
target_compile_definitions(echo_options INTERFACE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_include_directories(echo_options INTERFACE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/third_party)
if(MSVC)
    target_compile_options(echo_options INTERFACE /W4 /permissive- /utf-8 /fp:fast /EHsc)
else()
    target_compile_options(echo_options INTERFACE -Wall -Wextra -Wno-unused-parameter
        -Wno-missing-field-initializers -ffp-contract=fast -fno-math-errno)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        target_compile_options(echo_options INTERFACE -O2)
    endif()
    if(ECHO_SANITIZE)
        # Sanitizers need the dynamic runtime (no -static).
        target_compile_options(echo_options INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=undefined
            -fno-omit-frame-pointer -g)
        target_link_options(echo_options INTERFACE -fsanitize=address,undefined)
    else()
        # Static C++ runtime: the executables have no dependency on libc++.dll.
        target_link_options(echo_options INTERFACE -static)
    endif()
    target_compile_options(echo_options INTERFACE -msse4.1)
endif()

# ---------------------------------------------------------------------------
# RNNoise (BSD-3-Clause): neural speech enhancement used by the noise
# suppressor. Built with runtime CPU dispatch (SSE4.1/AVX2 when available).
# ---------------------------------------------------------------------------
file(GLOB RNNOISE_SOURCES third_party/rnnoise/src/*.c)
set(RNNOISE_X86 third_party/rnnoise/src/x86/x86_dnn_map.c third_party/rnnoise/src/x86/x86cpu.c
    third_party/rnnoise/src/x86/nnet_sse4_1.c third_party/rnnoise/src/x86/nnet_avx2.c)
add_library(rnnoise STATIC ${RNNOISE_SOURCES} ${RNNOISE_X86})
target_include_directories(rnnoise PUBLIC third_party/rnnoise/include PRIVATE third_party/rnnoise/src)
target_compile_definitions(rnnoise PRIVATE RNN_ENABLE_X86_RTCD=1)
if(MSVC)
    target_compile_definitions(rnnoise PRIVATE CPU_INFO_BY_C=1)
    set_source_files_properties(third_party/rnnoise/src/x86/nnet_avx2.c PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
else()
    target_compile_definitions(rnnoise PRIVATE CPU_INFO_BY_ASM=1)
    target_compile_options(rnnoise PRIVATE -w -O2)
    set_source_files_properties(third_party/rnnoise/src/x86/nnet_sse4_1.c PROPERTIES COMPILE_OPTIONS "-msse4.1")
    set_source_files_properties(third_party/rnnoise/src/x86/nnet_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
endif()

# ---------------------------------------------------------------------------
# Core engine + DSP (no UI)
# ---------------------------------------------------------------------------
file(GLOB ECHO_CORE_SOURCES CONFIGURE_DEPENDS
    src/core/*.cpp src/dsp/*.cpp src/dsp/effects/*.cpp src/session/*.cpp src/ai/*.cpp)
add_library(echo_core STATIC ${ECHO_CORE_SOURCES})
target_link_libraries(echo_core PUBLIC echo_options avrt rnnoise dxgi secur32)

# ---------------------------------------------------------------------------
# Windows audio platform layer (WASAPI, sessions, virtual cables)
# ---------------------------------------------------------------------------
file(GLOB ECHO_AUDIO_SOURCES CONFIGURE_DEPENDS src/audio/*.cpp)
if(ECHO_AUDIO_SOURCES)
add_library(echo_audio STATIC ${ECHO_AUDIO_SOURCES})
target_link_libraries(echo_audio PUBLIC echo_core ole32 oleaut32 uuid mmdevapi avrt ksuser
    propsys shell32 shlwapi psapi version runtimeobject winmm mfplat mfreadwrite mfuuid winhttp bcrypt)
endif()

# ---------------------------------------------------------------------------
# Dear ImGui
# ---------------------------------------------------------------------------
add_library(imgui STATIC
    third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_demo.cpp third_party/imgui/imgui_stdlib.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS UNICODE _UNICODE NOMINMAX)
if(NOT MSVC)
    target_compile_options(imgui PRIVATE -w)
    target_link_options(imgui INTERFACE -static)
endif()
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
# The soundboard's built-in sounds live next to the executables (also for
# the tests and tools in bin/).
file(GLOB ECHO_SOUND_FILES CONFIGURE_DEPENDS sounds/*)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/sounds.stamp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sounds
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ECHO_SOUND_FILES} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sounds
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/sounds.stamp
    DEPENDS ${ECHO_SOUND_FILES}
    COMMENT "Copying the built-in sounds")
add_custom_target(echo_sounds ALL DEPENDS ${CMAKE_BINARY_DIR}/sounds.stamp)
# The documentation too (Settings > About > Open documentation).
file(GLOB ECHO_DOC_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS docs/*.md)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/docs.stamp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/docs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ECHO_DOC_FILES} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/docs
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/docs.stamp
    DEPENDS ${ECHO_DOC_FILES}
    COMMENT "Copying the documentation")
add_custom_target(echo_docs ALL DEPENDS ${CMAKE_BINARY_DIR}/docs.stamp)

file(GLOB ECHO_UI_SOURCES CONFIGURE_DEPENDS src/ui/*.cpp src/app/*.cpp)
if(ECHO_UI_SOURCES)
add_executable(Echo WIN32 ${ECHO_UI_SOURCES} resources/Echo.rc)
target_link_libraries(Echo PRIVATE echo_audio imgui comdlg32 shcore dbghelp dwmapi windowscodecs)
if(NOT MSVC)
    target_link_options(Echo PRIVATE -municode)
endif()
set_target_properties(Echo PROPERTIES OUTPUT_NAME "${ECHO_APP_NAME}")
add_dependencies(Echo echo_sounds echo_docs)
endif()

# ---------------------------------------------------------------------------
# Tests and tools
# ---------------------------------------------------------------------------
if(ECHO_BUILD_TESTS)
    enable_testing()
    file(GLOB ECHO_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
    add_executable(echo_tests ${ECHO_TEST_SOURCES})
    target_link_libraries(echo_tests PRIVATE echo_core)
    if(TARGET echo_audio)
        target_link_libraries(echo_tests PRIVATE echo_audio) # Windows device logic (cable pairing)
    endif()
    target_compile_definitions(echo_tests PRIVATE ECHO_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
        ECHO_TEST_SPEECH_DIR="${CMAKE_BINARY_DIR}/test-speech")
    add_dependencies(echo_tests echo_sounds)
    add_test(NAME echo_tests COMMAND echo_tests)
endif()

if(ECHO_BUILD_TOOLS)
    add_executable(echo_bench tools/bench.cpp)
    target_link_libraries(echo_bench PRIVATE echo_core avrt)
    if(NOT MSVC)
        target_link_options(echo_bench PRIVATE -municode)
    endif()

    add_executable(echo_hwtest tools/hwtest.cpp)
    target_link_libraries(echo_hwtest PRIVATE echo_audio)
    add_dependencies(echo_hwtest echo_sounds)
    if(NOT MSVC)
        target_link_options(echo_hwtest PRIVATE -municode)
    endif()
endif()

# ---------------------------------------------------------------------------
# Install layout (used by the installer and the portable zip)
# ---------------------------------------------------------------------------
if(TARGET Echo)
    install(TARGETS Echo RUNTIME DESTINATION .)
endif()
install(DIRECTORY presets/ DESTINATION presets)
install(DIRECTORY sounds/ DESTINATION sounds)
install(FILES LICENSE THIRD_PARTY_NOTICES.md README.md DESTINATION .)
install(DIRECTORY docs/ DESTINATION docs)
