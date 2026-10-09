file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(WRITE "${OUTPUT_DIR}/assets.S" ".section .rodata.ksud_assets,\"a\"\n")
file(WRITE "${OUTPUT_DIR}/assets.c" "#include \"assets.h\"\n")
set(entries "")
set(index 0)
foreach(name busybox waitsys)
    set(source "${SOURCE_DIR}/bin/aarch64/${name}")
    set(packed "${OUTPUT_DIR}/${name}.xz")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=XZ_OPT --unset=XZ_DEFAULTS
        "${XZ_EXECUTABLE}" --compress --stdout --threads=1 --check=crc64
        --arm64 --lzma2=preset=6,dict=2MiB,lp=2,lc=2 -- "${source}"
        OUTPUT_FILE "${packed}" COMMAND_ERROR_IS_FATAL ANY)
    file(SIZE "${source}" size)
    file(SIZE "${packed}" packed_size)
    file(TO_CMAKE_PATH "${packed}" packed)
    file(APPEND "${OUTPUT_DIR}/assets.S"
        ".p2align 3\n.global ksu_asset_${index}\nksu_asset_${index}:\n.incbin \"${packed}\"\n")
    file(APPEND "${OUTPUT_DIR}/assets.c" "extern const unsigned char ksu_asset_${index}[];\n")
    string(APPEND entries "{\"${name}\", ksu_asset_${index}, ${packed_size}, ${size}},\n")
    math(EXPR index "${index} + 1")
endforeach()
foreach(name banner installer)
    if(name STREQUAL "installer")
        set(source "${SOURCE_DIR}/src/installer.sh")
    else()
        set(source "${SOURCE_DIR}/src/banner")
    endif()
    file(TO_CMAKE_PATH "${source}" source)
    file(APPEND "${OUTPUT_DIR}/assets.S"
        ".p2align 3\n.global ksu_${name}\nksu_${name}:\n.incbin \"${source}\"\n.byte 0\n")
endforeach()
file(APPEND "${OUTPUT_DIR}/assets.S" ".section .note.GNU-stack,\"\"\n")
file(APPEND "${OUTPUT_DIR}/assets.c"
    "const struct ksu_asset ksu_assets[] = {\n${entries}};\nconst size_t ksu_asset_count = ${index};\n")
