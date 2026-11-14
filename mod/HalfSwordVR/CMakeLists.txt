set(TARGET HalfSwordVR)
add_library(${TARGET} SHARED src/dllmain.cpp)
target_link_libraries(${TARGET} PUBLIC UE4SS)
# UE4SS loads Mods/<ModName>/dlls/main.dll
set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME main)
