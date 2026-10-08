cmake_minimum_required(VERSION 3.16)
project(yanflow VERSION 0.2.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "YanFlow requires Windows x64 and the MSVC toolchain")
endif()

# Embed the C++ runtime so the portable package needs no VC redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(yanflow WIN32 main.cpp yanflow.cpp yanflow.h yanflow.rc)
target_compile_features(yanflow PRIVATE cxx_std_17)
target_compile_definitions(yanflow PRIVATE UNICODE _UNICODE)
target_compile_options(yanflow PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/utf-8;/EHsc>")
target_include_directories(yanflow PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../assets/icons")
target_link_libraries(yanflow PRIVATE
    user32 gdi32 winmm ole32 comctl32 shell32 uiautomationcore uuid)
