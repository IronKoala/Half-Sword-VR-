cmake_minimum_required(VERSION 3.24)
project(HalfSwordVR LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "Half Sword VR builds with MSVC on Windows (Visual Studio 2022 or Build Tools).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
# OpenXR SDK: headers + loader (Apache-2.0). Built statically so we ship one file.
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
  GIT_TAG release-1.1.38
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(openxr)
# The SDK forces the DLL C runtime (/MD) on its static loader; match ours (/MT).
set_target_properties(openxr_loader PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(tools/xrtest)
