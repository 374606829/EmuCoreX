# SPDX-License-Identifier: BSD-3-Clause
# ReShade FX standalone compiler - vendored subset of D:\PS2\reshade-main\source\
# Used by EmuCoreX to compile *.fx (ReShade FX) sources to SPIR-V at runtime.
#
# This static library only compiles the language frontend + SPIR-V backend.
# Windows / GLSL / HLSL / DXBC / DXIL backends are intentionally excluded for
# the Android Vulkan build. Add the corresponding source files back here if a
# different backend is needed in the future.

cmake_minimum_required(VERSION 3.16)

set(RESHADEFX_SOURCES
    source/effect_lexer.cpp
    source/effect_expression.cpp
    source/effect_symbol_table.cpp
    source/effect_preprocessor.cpp
    source/effect_parser_exp.cpp
    source/effect_parser_stmt.cpp
    source/effect_codegen_spirv.cpp
    source/effect_codegen_glsl.cpp
)

set(RESHADEFX_HEADERS
    source/effect_codegen.hpp
    source/effect_expression.hpp
    source/effect_lexer.hpp
    source/effect_module.hpp
    source/effect_parser.hpp
    source/effect_preprocessor.hpp
    source/effect_symbol_table.hpp
    source/effect_token.hpp
    source/effect_symbol_table_intrinsics.inl
)

add_library(reshadefx_static STATIC ${RESHADEFX_SOURCES} ${RESHADEFX_HEADERS})

target_include_directories(reshadefx_static
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/source
)

# SPIR-V headers (spirv.hpp / GLSL.std.450.h) are reused from the glslang
# vendored copy already shipped under app/src/main/cpp/3rdparty/glslang.
target_include_directories(reshadefx_static
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../glslang/glslang/SPIRV
)

target_compile_features(reshadefx_static PRIVATE cxx_std_17)

# ReShade FX upstream is built with /utf-8 on MSVC and warnings-as-errors. We
# relax both knobs for the Android NDK build so the upstream code compiles
# cleanly without any local source edits.
if(MSVC)
    target_compile_options(reshadefx_static PRIVATE /utf-8 /wd4100 /wd4244 /wd4267 /wd4456 /wd4458 /wd4459)
else()
    # The PCSX2 host build forces `-fno-exceptions` globally via
    # BuildParameters.cmake, and the upstream reshadefx slice we vendored
    # does not actually use try/catch/throw, so we deliberately do *not*
    # try to opt back into exceptions for this static library - that would
    # be silently overridden anyway, and would create a footgun if upstream
    # ever introduces a throw site without us noticing.
    target_compile_options(reshadefx_static PRIVATE
        -Wno-unused-parameter
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-unused-but-set-variable
        -Wno-sign-compare
        -Wno-shadow
        -Wno-deprecated-declarations
        -Wno-implicit-fallthrough
        -Wno-misleading-indentation
        -Wno-parentheses
        -Wno-reorder-ctor
        -Wno-conversion
    )
endif()

# ReShade FX uses Windows-specific helpers (e.g. localtime_s) only inside the
# main runtime, never inside this language-frontend slice, so no platform
# shims are required for Android.
