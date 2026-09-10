cmake_minimum_required(VERSION 3.24)

project(Slate
    VERSION 0.3.0
    # The product is Slate; "CAD" is in the description and the shipped name so
    # that somebody searching for drafting software finds it. See
    # src/app/Branding.hpp for where each form of the name belongs.
    DESCRIPTION "Slate CAD - 2D CAD drafting, DWG compatible, opens and saves DWG and DXF"
    # C is enabled for the example plugins only. Compiling the plugin header as
    # real C is what keeps its promise of being C-compatible honest.
    LANGUAGES CXX C
)

# ---------------------------------------------------------------- options

option(CAD_BUILD_TESTS "Build the test suite" ON)
option(CAD_BUILD_APP "Build the Qt application shell" ON)
option(CAD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# ------------------------------------------------------------- global setup

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single output directory so the portable build is a directory that can simply
# be copied, which is the stated initial distribution model.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Export compile_commands.json for clangd and other tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ----------------------------------------------------- warning configuration

# A CAD kernel is numerically delicate; the warnings that catch narrowing,
# sign confusion and uninitialised reads are exactly the ones that catch
# geometry bugs, so they are errors rather than noise.
add_library(cad_warnings INTERFACE)

if(MSVC)
    target_compile_options(cad_warnings INTERFACE
        /W4
        /permissive-        # Enforce standard conformance.
        /Zc:__cplusplus     # Report the real __cplusplus value.
        /Zc:preprocessor    # Standards-conforming preprocessor.
        /Zc:inline
        /utf-8
        /wd4324             # Structure padded due to alignment specifier.
        /wd4251             # DLL-interface warnings; not shipping C++ DLLs.
    )
    if(CAD_WARNINGS_AS_ERRORS)
        target_compile_options(cad_warnings INTERFACE /WX)
    endif()
    # Deterministic floating point. A CAD kernel must not have the optimiser
    # reassociate arithmetic: it changes intersection results between debug and
    # release builds, which is the worst class of bug to chase.
    target_compile_options(cad_warnings INTERFACE /fp:precise)
    target_compile_definitions(cad_warnings INTERFACE
        NOMINMAX                    # <windows.h> min/max macros break std::min.
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        UNICODE _UNICODE
    )
else()
    target_compile_options(cad_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wconversion -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Woverloaded-virtual
        -Wdouble-promotion
    )
    if(CAD_WARNINGS_AS_ERRORS)
        target_compile_options(cad_warnings INTERFACE -Werror)
    endif()
endif()

# ------------------------------------------------------------------ targets

add_subdirectory(src/core)

# Layers are added as they are ported (see ADR 0002). Guarding on existence
# keeps the build green throughout the port rather than requiring every
# subdirectory to be stubbed out before the first one compiles.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/io/CMakeLists.txt")
    add_subdirectory(src/io)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/render/CMakeLists.txt")
    add_subdirectory(src/render)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/abi/CMakeLists.txt")
    add_subdirectory(src/abi)
endif()

if(CAD_BUILD_APP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/app/CMakeLists.txt")
    add_subdirectory(src/app)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt")
    add_subdirectory(tools)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

if(CAD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ------------------------------------------------------------------ summary

message(STATUS "")
message(STATUS "Slate CAD ${PROJECT_VERSION}")
message(STATUS "  Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler:    ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Tests:       ${CAD_BUILD_TESTS}")
message(STATUS "  Qt app:      ${CAD_BUILD_APP}")
message(STATUS "")
