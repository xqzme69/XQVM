cmake_minimum_required(VERSION 3.24)

project(xqvm VERSION 0.1.0 LANGUAGES CXX)

option(XQVM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(XQVM_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(XQVM_BUILD_FUZZER "Build the libFuzzer module-loader target" OFF)

if(XQVM_BUILD_FUZZER AND WIN32)
    # The prebuilt Windows libFuzzer runtime uses the unannotated MSVC STL ABI.
    add_compile_definitions(_DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
endif()

add_library(xqvm STATIC
    src/compiler.cpp
    src/disassembler.cpp
    src/examples.cpp
    src/machine_ir.cpp
    src/module.cpp
    src/physical.cpp
    src/primitive.cpp
    src/profile.cpp
    src/runtime.cpp
)

target_include_directories(xqvm PUBLIC include)
target_compile_features(xqvm PUBLIC cxx_std_20)

function(xqvm_enable_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
        if(XQVM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        if(XQVM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

xqvm_enable_project_warnings(xqvm)

if(XQVM_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "XQVM_ENABLE_SANITIZERS currently requires Clang or GCC")
    endif()
    if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(FATAL_ERROR
            "Windows Clang ASan is incompatible with the Debug CRT in this toolchain; "
            "use RelWithDebInfo or Release")
    endif()
    target_compile_options(xqvm PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(xqvm PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

add_executable(xqvm_cli apps/xqvm.cpp)
target_link_libraries(xqvm_cli PRIVATE xqvm)
xqvm_enable_project_warnings(xqvm_cli)
set_target_properties(xqvm_cli PROPERTIES OUTPUT_NAME xqvm)

include(CTest)
if(BUILD_TESTING)
    add_executable(xqvm_tests tests/xqvm_tests.cpp)
    target_link_libraries(xqvm_tests PRIVATE xqvm)
    xqvm_enable_project_warnings(xqvm_tests)
    add_test(NAME xqvm_tests COMMAND xqvm_tests)
endif()

function(xqvm_copy_clang_asan_runtime target)
    if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        return()
    endif()
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
        OUTPUT_VARIABLE XQVM_CLANG_RESOURCE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    set(XQVM_ASAN_RUNTIME
        "${XQVM_CLANG_RESOURCE_DIR}/lib/windows/clang_rt.asan_dynamic-x86_64.dll")
    if(NOT EXISTS "${XQVM_ASAN_RUNTIME}")
        message(FATAL_ERROR "Clang ASan runtime was not found: ${XQVM_ASAN_RUNTIME}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${XQVM_ASAN_RUNTIME}" "$<TARGET_FILE_DIR:${target}>")
endfunction()

if(XQVM_ENABLE_SANITIZERS)
    xqvm_copy_clang_asan_runtime(xqvm_cli)
    if(BUILD_TESTING)
        xqvm_copy_clang_asan_runtime(xqvm_tests)
    endif()
endif()

if(XQVM_BUILD_FUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "XQVM_BUILD_FUZZER requires Clang with libFuzzer")
    endif()
    add_executable(xqvm_module_fuzz fuzz/module_loader_fuzz.cpp)
    target_compile_options(xqvm PRIVATE -fsanitize=fuzzer-no-link)
    target_link_options(xqvm_cli PRIVATE -fsanitize=fuzzer-no-link)
    target_link_libraries(xqvm_module_fuzz PRIVATE xqvm)
    xqvm_enable_project_warnings(xqvm_module_fuzz)
    target_compile_options(xqvm_module_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(xqvm_module_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    xqvm_copy_clang_asan_runtime(xqvm_module_fuzz)
endif()
