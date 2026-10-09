include(FetchContent)

FetchContent_Declare(ksud_xz
    URL https://codeload.github.com/tukaani-project/xz/tar.gz/d3e650e63c110e830fd5391e7f8b45df0b91d3da
    URL_HASH SHA256=b5aad5c776b324a39a46bbd588d4b5c001266b87ed3887aea1dc2854702d041c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# Build these libraries below instead of using their upstream CMake targets.
FetchContent_Declare(ksud_minizip
    URL https://codeload.github.com/zlib-ng/minizip-ng/tar.gz/7b2387161c542fa9f427352dcdef76097d0d692b
    URL_HASH SHA256=467a189e998c7754ada577758d29324d5b3c866a7fb5d43700b8c551d72ca998
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR no-cmake)
FetchContent_Declare(ksud_zlib
    URL https://codeload.github.com/madler/zlib/tar.gz/da607da739fa6047df13e66a2af6b8bec7c2a498
    URL_HASH SHA256=b9258cf6254e7f7c37f1cd61dba943a1c5ea3cff5718c789834dac359094f5f7
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR no-cmake)
FetchContent_Declare(ksud_yyjson
    URL https://codeload.github.com/ibireme/yyjson/tar.gz/6447536015f3d600f3d65323b10976103b337ca7
    URL_HASH SHA256=13d53411c9d818836a2dc0b3f4be40c34ee4e0fcd21ff903e0aebbfd52e9e08d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR no-cmake)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(XZ_NLS OFF CACHE BOOL "" FORCE)
set(XZ_EXTERNAL_SHA256 OFF CACHE BOOL "" FORCE)
set(XZ_SANDBOX "no" CACHE STRING "" FORCE)
set(XZ_THREADS "no" CACHE STRING "" FORCE)
set(XZ_ENCODERS "" CACHE STRING "" FORCE)
set(XZ_MICROLZMA_ENCODER OFF CACHE BOOL "" FORCE)
set(XZ_MICROLZMA_DECODER OFF CACHE BOOL "" FORCE)
set(XZ_LZIP_DECODER OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZ OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZDEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMADEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMAINFO OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SCRIPTS OFF CACHE BOOL "" FORCE)
set(XZ_DOC OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ksud_xz ksud_minizip ksud_zlib ksud_yyjson)

include(CheckIncludeFile)
include(CheckSymbolExists)
check_include_file(dirent.h HAVE_DIRENT_H)
check_include_file(sys/dirent.h HAVE_SYS_DIRENT_H)
check_include_file(inttypes.h HAVE_INTTYPES_H)
check_include_file(stdint.h HAVE_STDINT_H)
set(HAVE_PDIR ${HAVE_DIRENT_H})
check_symbol_exists(fseeko "stdio.h" HAVE_FSEEKO)
check_symbol_exists(symlink "unistd.h" HAVE_SYMLINK)
check_symbol_exists(readlink "unistd.h" HAVE_READLINK)
configure_file("${ksud_minizip_SOURCE_DIR}/mz_config.h.cmakein" mz_config.h)

add_library(ksud_zip STATIC
    src/zip.c
    src/apk_sign.c
    src/hash.c
    "${ksud_minizip_SOURCE_DIR}/mz_crypt.c"
    "${ksud_minizip_SOURCE_DIR}/mz_os.c"
    "${ksud_minizip_SOURCE_DIR}/mz_os_posix.c"
    "${ksud_minizip_SOURCE_DIR}/mz_strm.c"
    "${ksud_minizip_SOURCE_DIR}/mz_strm_mem.c"
    "${ksud_minizip_SOURCE_DIR}/mz_strm_os_posix.c"
    "${ksud_minizip_SOURCE_DIR}/mz_strm_zlib.c"
    "${ksud_minizip_SOURCE_DIR}/mz_strm_lzma.c"
    "${ksud_minizip_SOURCE_DIR}/mz_zip.c"
    "${ksud_zlib_SOURCE_DIR}/contrib/infback9/infback9.c"
    "${ksud_zlib_SOURCE_DIR}/contrib/infback9/inftree9.c")
target_compile_features(ksud_zip PRIVATE c_std_23)
target_include_directories(ksud_zip PRIVATE "${ksud_minizip_SOURCE_DIR}"
    "${ksud_zlib_SOURCE_DIR}" "${ksud_zlib_SOURCE_DIR}/contrib/infback9"
    "${ksud_xz_SOURCE_DIR}/src/liblzma/api"
    ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(ksud_zip PRIVATE HAVE_ZLIB ZLIB_COMPAT HAVE_LZMA
    LZMA_API_STATIC MZ_ZIP_NO_COMPRESSION MZ_ZIP_NO_ENCRYPTION MZ_ZIP_NO_CRYPTO)
set_source_files_properties(src/hash.c PROPERTIES
    INCLUDE_DIRECTORIES "$<TARGET_PROPERTY:liblzma,INCLUDE_DIRECTORIES>"
    COMPILE_DEFINITIONS "$<TARGET_PROPERTY:liblzma,COMPILE_DEFINITIONS>")
set_source_files_properties("${ksud_zlib_SOURCE_DIR}/contrib/infback9/infback9.c" PROPERTIES
    COMPILE_DEFINITIONS "zcalloc=ksu_zip_calloc;zcfree=ksu_zip_free")
target_link_libraries(ksud_zip PUBLIC liblzma z)
