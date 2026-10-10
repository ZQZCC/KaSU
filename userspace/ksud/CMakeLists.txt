cmake_minimum_required(VERSION 3.24)
project(ksud LANGUAGES C ASM)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "ksud supports ARM64 Android only")
endif()
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -flto=full")
set(CMAKE_C_FLAGS_MINSIZEREL "-Oz -DNDEBUG -flto=full")
find_program(XZ_EXECUTABLE NAMES xz REQUIRED)

execute_process(COMMAND git rev-list --count HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_COUNT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND git describe --tags --always WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE VERSION_NAME OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
math(EXPR VERSION_CODE "30000 + ${GIT_COUNT} - 84")
string(REGEX REPLACE "^v" "" VERSION_NAME "${VERSION_NAME}")
if(DEFINED ENV{KSU_PACKAGE_NAME})
    set(KSU_PACKAGE_NAME "$ENV{KSU_PACKAGE_NAME}" CACHE STRING "Manager package")
else()
    set(KSU_PACKAGE_NAME "me.weishu.kernelsu" CACHE STRING "Manager package")
endif()

include(dependencies.cmake)
set(SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")
file(GLOB ASSET_BINARIES CONFIGURE_DEPENDS "bin/aarch64/*")
add_custom_command(OUTPUT "${OUTPUT_DIR}/assets.S" "${OUTPUT_DIR}/assets.c"
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${SOURCE_DIR}" "-DOUTPUT_DIR=${OUTPUT_DIR}"
        "-DXZ_EXECUTABLE=${XZ_EXECUTABLE}"
        -P "${SOURCE_DIR}/embed_assets.cmake"
    DEPENDS embed_assets.cmake src/banner src/installer.sh ${ASSET_BINARIES}
    VERBATIM)

add_executable(ksud
    src/main.c src/cli.c src/debug.c src/resetprop.c
    src/driver.c src/process.c src/boot.c src/soft_reboot.c src/su.c
    src/file.c src/label.c src/text.c src/properties.c src/config.c src/feature.c
    src/profile.c src/policy.c src/policy_file.c src/sepolicy.c src/sulog.c
    src/prop_context.c src/prop_persist.c src/prop_area.c src/prop.c
    src/module_state.c src/module_install.c src/module_command.c src/module_cleanup.c
    src/module_query.c src/module_rc.c src/metamodule.c src/script.c src/json.c src/assets.c
    "${OUTPUT_DIR}/assets.S" "${OUTPUT_DIR}/assets.c")
target_include_directories(ksud PRIVATE ../.. src "${ksud_yyjson_SOURCE_DIR}/src"
    "${ksud_xz_SOURCE_DIR}/src/liblzma/api")
target_compile_definitions(ksud PRIVATE
    KSU_VERSION_CODE=${VERSION_CODE} KSU_VERSION_NAME="${VERSION_NAME}"
    KSU_PACKAGE_NAME="${KSU_PACKAGE_NAME}"
    YYJSON_DISABLE_READER=1 YYJSON_DISABLE_FILE=1 YYJSON_DISABLE_UTILS=1
    YYJSON_DISABLE_FAST_FP_CONV=1 YYJSON_DISABLE_NON_STANDARD=1)
target_compile_options(ksud PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-ffunction-sections;-fdata-sections>)
target_link_options(ksud PRIVATE -Wl,-O2 -Wl,--gc-sections -Wl,--icf=safe -Wl,--pack-dyn-relocs=android)
target_link_libraries(ksud PRIVATE ksud_zip log dl m)
