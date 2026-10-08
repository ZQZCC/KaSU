cmake_minimum_required(VERSION 3.18.1)

project(kernelsu LANGUAGES C)

add_library(k SHARED jni.c ksu.c)

target_include_directories(k PRIVATE .)

find_library(log-lib log)

target_link_libraries(k PRIVATE ${log-lib})
