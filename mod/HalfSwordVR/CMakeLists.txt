set(TARGET HalfSwordVR)
add_library(${TARGET} SHARED
    src/dllmain.cpp
    src/arm_control.cpp
    src/config.cpp
    src/engine_slot.cpp src/engine_texture.cpp
    src/input_watch.cpp
    src/log.cpp
    src/stereo_device.cpp
    src/vr_present.cpp
    src/xr_loader.cpp)
target_include_directories(${TARGET} PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
target_compile_definitions(${TARGET} PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(${TARGET} PUBLIC UE4SS)
# UE4SS loads Mods/<ModName>/dlls/main.dll
set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME main)
