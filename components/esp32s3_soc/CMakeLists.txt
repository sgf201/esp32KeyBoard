# esp32s3_soc: ships a precompiled libesp32s3_soc.a providing GT911_RST().
#
# The original CMakeLists used the legacy register_component() macro from the
# ESP-IDF 3.x build system, which is why main.c could not find esp32s3_soc.h
# after upgrading to IDF 5.x. Rewritten with the modern
# idf_component_register() API; the prebuilt archive is imported below.
idf_component_register(INCLUDE_DIRS "include")

add_prebuilt_library(esp32s3_soc_lib "${CMAKE_CURRENT_LIST_DIR}/lib/libesp32s3_soc.a")
target_link_libraries(${COMPONENT_LIB} INTERFACE esp32s3_soc_lib)

# The archive was built for an older IDF, so let the linker accept it.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--allow-multiple-definition")
