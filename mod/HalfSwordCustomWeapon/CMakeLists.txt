# Test mod: an uncooked custom weapon part built at runtime from an OBJ file.
set(TARGET HalfSwordCustomWeapon)
add_library(${TARGET} SHARED src/main.cpp)
target_compile_definitions(${TARGET} PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(${TARGET} PUBLIC UE4SS)
# UE4SS loads Mods/<ModName>/dlls/main.dll
set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME main)
