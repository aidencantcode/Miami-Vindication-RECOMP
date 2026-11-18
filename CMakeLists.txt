cmake_minimum_required(VERSION 3.25)

# Dependencies come from vcpkg (manifest: vcpkg.json). CMakePresets.json points
# CMAKE_TOOLCHAIN_FILE at Visual Studio's bundled vcpkg.
project(GangstarNative LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

find_package(SDL2 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_path(STB_INCLUDE_DIR stb_vorbis.c)  # vcpkg "stb" (header-only)
# Unicorn: the same 2.1.4 build the Python probe uses, prepared by tools/setup_unicorn.ps1
# (headers + import lib + DLL under third_party/unicorn, git-ignored).
set(UNICORN_ROOT "${CMAKE_SOURCE_DIR}/third_party/unicorn" CACHE PATH "Prebuilt Unicorn")
add_library(unicorn SHARED IMPORTED)
set_target_properties(unicorn PROPERTIES
    IMPORTED_IMPLIB "${UNICORN_ROOT}/lib/unicorn.lib"
    IMPORTED_LOCATION "${UNICORN_ROOT}/bin/unicorn.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${UNICORN_ROOT}/include")
set(UNICORN_TARGET unicorn)

add_executable(gangstar
    src/main.cpp
    src/core/log.cpp
    src/core/guest_memory.cpp
    src/loader/elf_image.cpp
    src/hle/imports.cpp
    src/hle/bionic_data.cpp
    src/hle/libc_core.cpp
    src/hle/libc_stdio.cpp
    src/cpu/cpu.cpp
    src/rt/runtime.cpp
    src/jni/jni.cpp
    src/gl/gles1.cpp
    src/input/input.cpp
    src/settings/settings.cpp
    src/game/mods.cpp
    src/ui/overlay.cpp
    src/audio/mixer.cpp
    src/audio/media_player.cpp
    src/game.cpp
    src/selftest.cpp
)
target_include_directories(gangstar PRIVATE src ${STB_INCLUDE_DIR})
target_compile_definitions(gangstar PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(gangstar PRIVATE /W4 /permissive- /utf-8 /wd4100)
endif()
target_link_libraries(gangstar PRIVATE SDL2::SDL2 SDL2::SDL2main OpenGL::GL imgui::imgui ${UNICORN_TARGET})

# Copy runtime DLLs next to the exe.
add_custom_command(TARGET gangstar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:gangstar> $<TARGET_FILE_DIR:gangstar>
    COMMAND_EXPAND_LISTS)

# stb_vorbis is compiled inside mixer.cpp; silence its code-generation warnings.
if(MSVC)
    set_source_files_properties(src/audio/mixer.cpp PROPERTIES COMPILE_OPTIONS "/wd4701;/wd4703")
endif()
