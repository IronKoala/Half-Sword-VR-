# Builds the in-game mod as a UE4SS C++ mod.
# UE4SS is pinned (deps/RE-UE4SS) to the exact commit of the UE4SS.dll installed in the game,
# because C++ mods must match the UE4SS build they load into.
cmake_minimum_required(VERSION 3.22)
project(HalfSwordVRMod)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../deps/RE-UE4SS ${CMAKE_BINARY_DIR}/RE-UE4SS)
add_subdirectory(HalfSwordVR)
