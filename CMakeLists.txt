cmake_minimum_required(VERSION 3.15)
project(schwung-4k-eq VERSION 1.0.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# No -ffast-math: it lets the compiler assume no NaN/Inf, which removes the
# safety of the clamps on the realtime path. The core's overload rail and its
# isfinite() guards are load-bearing.
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# Device runtime is glibc 2.35 / libstdc++ 6.0.29 — link the C++ runtime
# statically so the .so carries no libstdc++ version requirement at all.
set(FKQ_STATIC_LINK -static-libstdc++ -static-libgcc)

set(FKQ_INCLUDES
    src/dsp
    src/host
    src/ported/shared-daf/dsp
    src/ported/daf-plugin)

# The Move module. audio_fx: the chain host hardcodes
# "<audio_fx>/<id>/<id>.so" and IGNORES module.json's "dsp" field
# (chain_host.c). Only sound_generators load dsp.so — shipping this as
# dsp.so is a known way to produce a module that never loads.
add_library(fourk_eq SHARED
    src/dsp/fourk_eq_plugin.cpp
    src/ported/shared-daf/dsp/FourKEQDSP.cpp)
target_include_directories(fourk_eq PRIVATE ${FKQ_INCLUDES})
target_link_libraries(fourk_eq PRIVATE m pthread)
target_link_options(fourk_eq PRIVATE ${FKQ_STATIC_LINK})
set_target_properties(fourk_eq PROPERTIES
    OUTPUT_NAME "4k-eq" PREFIX "" SUFFIX ".so")

# Loader test: dlopens the real 4k-eq.so exactly like the chain host does,
# proves every control APPLIES, and times the realtime factor. Cross-compiled
# and run ON the Move — the go/no-go gate before wiring it into a set.
add_executable(fkq_loadtest tools/loadtest.cpp)
target_include_directories(fkq_loadtest PRIVATE src/host)
target_link_libraries(fkq_loadtest PRIVATE dl m)
target_link_options(fkq_loadtest PRIVATE ${FKQ_STATIC_LINK})
