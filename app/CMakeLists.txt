# Guwange (Xbox Live Arcade, CAVE, title 58410A78): static recompilation host application.
#
# The game code is not in this repository: run the code generator first (tools/codegen.ps1), which writes generated/
# from your own default.xex and game cores, together with the CMake glue (generated/rexglue.cmake, made by
# `rexglue codegen`).
#
# Stage 1, Windows PC (development): the SDK's emulated Xenos GPU, as the reference.
#   cmake --preset win-pc && cmake --build --preset win-pc        (from app/, inside tools\win\env.ps1)
# Stage 2: Renut's Direct3D-level renderer (futari_renderer = "renut", the default): deko3d on the Switch, plume's
# Vulkan / D3D12 on the PC (futari-nx's, through banjo-nx; cmake/guwange_renut_gpu.cmake).
#   Switch: tools/build_switch.ps1 (devkitA64, the PC tree's shader cache first).

cmake_minimum_required(VERSION 3.25)
project(guwange LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK is built from source in this repository unless another tree is given.
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake")
    message(FATAL_ERROR "app/generated/rexglue.cmake is missing: run tools/codegen.ps1 first")
endif()

# futari-nx's (peggle2-nx's, 2026-10-13): -mearly-ra=none for everything compiled for the console (devkitA64 GCC 16.1's
# AArch64 early register allocation dropped table lookups of the NEON vperm helper). Before the SDK is added.
option(GUWANGE_SWITCH_EARLY_RA "Leave GCC's AArch64 early register allocation pass on (Switch; can miscompile)" OFF)
if(REXGLUE_PLATFORM_SWITCH AND NOT GUWANGE_SWITCH_EARLY_RA)
    add_compile_options(-mearly-ra=none)
    add_link_options(-mearly-ra=none)
endif()

# No Vulkan driver in the Switch link (the user, 2026-10-21: deko3d only): the SDK's switch_vulkan_stub.c answers the
# Vulkan entry points the code references.
set(REXGLUE_SWITCH_LINK_NVK OFF CACHE BOOL "" FORCE)

include(generated/rexglue.cmake)

# The build re-runs codegen whenever one of its inputs changes; the post-codegen steps (tools/guwange_post_codegen.py:
# never-ending guest loops made explicit, then futari-nx's performance steps, tools/post_codegen.py) must follow EVERY
# codegen or they are lost (futari-nx's lesson of 2026-10-06), so they are appended to the codegen command itself.
# GUWANGE_PERF_CODEGEN_POST=OFF keeps the correctness step only: the generated code as rexglue writes it (the A side
# of the A/B pair).
option(GUWANGE_PERF_CODEGEN_POST "Run futari-nx's performance steps (tools/post_codegen.py) after every codegen" ON)
if(TARGET guwange_codegen)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_guwange_post_args "")
    if(NOT GUWANGE_PERF_CODEGEN_POST)
        set(_guwange_post_args --correctness-only)
    endif()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp" APPEND
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/guwange_post_codegen.py"
                ${_guwange_post_args})
endif()

set(GUWANGE_SOURCES
    src/main.cpp
    # What the generated code needs from the application (rex_spin_forever for tools/spin_forever.py).
    src/guwange_fixes.cpp
    # Video recordings of the game image for testing (cvar guwange_video_s; tools/make_video.sh makes the MP4).
    src/guwange_capture.cpp
    # Scripted pad for unattended test runs (cvar guwange_input_script; tools/win/boot_test.ps1 -Pad).
    src/guwange_input_script.cpp
    # PC sampling profiler (cvar guwange_pc_profile_s). Nothing on the Switch.
    src/guwange_pc_profiler.cpp
    # Development: guest memory words logged periodically (cvar guwange_diag_watch).
    src/guwange_diag.cpp
    # Windows: fatal host exceptions with module, offset and stack (logs/crash.txt for unhandled ones).
    src/guwange_crash_log.cpp
    # The PC self-checks of the post-codegen steps ("[verify] codegen checks" every 10 s; the cores compile it too).
    src/guwange_codegen_checks.cpp
    # Native replacements of the console profile's hottest guest functions (src/cpu/guwange_native.h: a key each,
    # a code-hash check, verified against the recompiled function with guwange_cpu_verify).
    src/cpu/guwange_native.cpp
    src/cpu/guwange_native_crt.cpp
    src/cpu/guwange_native_video.cpp
    # Input lag: the frame's controller read as late as the frame allows (futari_late_input).
    src/guwange_late_input.cpp
    # PGO profile collection (the Switch build with GUWANGE_PGO=generate; empty otherwise).
    src/guwange_pgo.cpp
    # On/off keys for every enhancement + the CPU / non-CPU masters (guwange_cpu_enhancements, guwange_noncpu_...).
    src/guwange_enhancements.cpp
)

if(NOT REXGLUE_PLATFORM_SWITCH)
    # The Switch profiler counters the renderer reports to (no-ops on the PC).
    list(APPEND GUWANGE_SOURCES src/me/me_pc_stubs.cpp)
endif()

if(WIN32)
    add_executable(guwange WIN32 ${GUWANGE_SOURCES})
else()
    add_executable(guwange ${GUWANGE_SOURCES})
endif()

# Link the SDK. Stage 1 draws with ReXGlue's Xenos GPU emulation (the PC reference).
rexglue_setup_target(guwange GPU_PLUGINS xenos)

# Ryujinx starts NROs without argv[0]; the SDK then takes the NRO's usual place on the SD (banjo-nx's fix).
if(REXGLUE_PLATFORM_SWITCH AND TARGET rexcore)
    target_compile_definitions(rexcore PRIVATE REX_SWITCH_FALLBACK_EXE="/switch/guwange-nx/guwange.nro")
endif()

target_include_directories(guwange PRIVATE src)
# The capture includes xxhash.h directly; when the SDK is built from source it is private to its libraries.
if(TARGET xxhash)
    target_include_directories(guwange PRIVATE $<TARGET_PROPERTY:xxhash,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Settings template next to the executable, only when there is none yet (the copy there is the user's to edit).
if(NOT REXGLUE_PLATFORM_SWITCH)
    add_custom_command(TARGET guwange POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/guwange.toml -DDST=$<TARGET_FILE_DIR:guwange>/guwange.toml
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_if_missing.cmake"
        VERBATIM)
endif()

# The SDK compiles itself with these (directory scope), but they do not reach the generated game code, which is built
# in this directory (futari-nx's rule, every game-core target included): GCC's default -ffp-contract=fast fuses
# separate PPC fmul/fadd into FMA and results drift from the Xbox 360; strict aliasing is unsafe for recompiled code.
# -fno-math-errno: nothing reads errno (one fsqrt instead of a check and a possible libm call).
set(GUWANGE_GAME_TARGETS guwange guwange_recomp
    guwange_Guwange01_x360 guwange_Guwange01_coop_x360 guwange_Guwange_arrange_x360 guwange_Guwange_blue_x360)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target ${GUWANGE_GAME_TARGETS})
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -ffp-contract=off -fno-strict-aliasing -fno-math-errno)
        endif()
    endforeach()
endif()

# The game cores' half of src/guwange_codegen_checks.cpp: a core is a DLL on the PC, with its own counters of the
# generated code's self-checks; the differences go to the executable under the core's name. Not built with the
# generated code's precompiled header.
foreach(_core guwange01:Guwange01_x360 guwange01_coop:Guwange01_coop_x360 guwange_arrange:Guwange_arrange_x360
              guwange_blue:Guwange_blue_x360)
    string(REPLACE ":" ";" _core "${_core}")
    list(GET _core 0 _core_tree)
    list(GET _core 1 _core_file)
    if(TARGET guwange_${_core_file})
        target_sources(guwange_${_core_file} PRIVATE src/guwange_codegen_checks.cpp)
        target_compile_definitions(guwange_${_core_file} PRIVATE GUWANGE_CORE_NAME="${_core_tree}")
    endif()
endforeach()
set_source_files_properties(src/guwange_codegen_checks.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# futari-nx's post-codegen steps (tools/post_codegen.py, run by tools/guwange_post_codegen.py; the user, 2026-10-22):
# one switch back per step, as futari-nx has them (FUTARI_GEN_*). OFF compiles the statements as they were before the
# step, on every platform, so a PC build can take one step out to find which of them a difference comes from. The two
# _POISON options are PC tests, never for a build that ships: every write the step left out writes 0xDEADBEEF... (a
# signalling NaN for a float register) instead, so a reader the analysis missed breaks visibly.
option(GUWANGE_GEN_RO_CONSTANTS "Loads from the image's read-only data as constants" ON)
option(GUWANGE_GEN_FLOAT_LOADS "Single-precision loads straight into a float register" ON)
option(GUWANGE_GEN_LEAF_LOCALS "ctx registers of leaf functions with loops as locals" ON)
option(GUWANGE_GEN_LOOP_LOCALS "ctx registers of call-free loops as locals for the length of the loop" ON)
option(GUWANGE_GEN_COLD_TRAPS "The trap helper out of line and cold" ON)
option(GUWANGE_GEN_PROMOTED_CALLS "Guarded direct calls at the promoted indirect call sites" ON)
option(GUWANGE_GEN_NARROW_LOOP_BARRIER "Loop barriers clobber the guest window only (OFF: all memory)" ON)
option(GUWANGE_GEN_DCBT_PREFETCH "The guest's dcbt / dcbtst hints as host prefetches" ON)
option(GUWANGE_GEN_VECTOR_LOCALS_BY_VALUE "Vector registers of the loop and leaf locals as values, not references" ON)
option(GUWANGE_GEN_VPERM1 "A vperm of one vector as a one-register lookup" ON)
option(GUWANGE_GEN_FP_SIGN_OPS "fneg / fabs / fnabs as float operations" ON)
option(GUWANGE_GEN_LEAF_DEAD "Leaf write-backs no caller can read left out" ON)
option(GUWANGE_GEN_LEAF_DEAD_POISON "PC test: the left-out leaf write-backs write poison" OFF)
option(GUWANGE_GEN_SINGLE_FLOATS "Single-precision arithmetic in float where the inputs are proven singles" ON)
option(GUWANGE_GEN_INLINE_MEMCALLS "memcpy / memset of a constant size up to 64 bytes inline" ON)
option(GUWANGE_GEN_VEC_UNALIGNED "stvlx / stvrx as a few stores instead of byte loops" ON)
option(GUWANGE_GEN_RO_CFG "Read-only constants found over the control-flow graph" ON)
option(GUWANGE_GEN_DEAD_DEFS "Guest register writes nothing reads left out" ON)
option(GUWANGE_GEN_DEAD_DEFS_POISON "PC test: the left-out register writes write poison" OFF)
option(GUWANGE_GEN_FLUSH_HOIST "Loop-header flush-mode checks in front of the loop" ON)
option(GUWANGE_GEN_LR_WRITE_SAME "Proven link-register write-backs compile to nothing (the PC build checks each)" ON)
option(GUWANGE_GEN_R1_LOCAL "Guest r1 as a local mirror (the PC build checks the assumed calls)" ON)
option(GUWANGE_GEN_VSEL_NEON "VMX vsel as one NEON bit select" ON)
option(GUWANGE_GEN_INLINE_LEAVES "Inline copies of tiny leaf functions in the files that call them" ON)
set(_guwange_gen_definitions "")
if(NOT GUWANGE_GEN_RO_CONSTANTS)
    list(APPEND _guwange_gen_definitions REX_RO_CONSTANTS_OFF)
endif()
if(NOT GUWANGE_GEN_FLOAT_LOADS)
    list(APPEND _guwange_gen_definitions REX_FLOAT_LOADS_INTEGER)
endif()
if(NOT GUWANGE_GEN_LEAF_LOCALS)
    list(APPEND _guwange_gen_definitions REX_LEAF_LOCALS_OFF)
endif()
if(NOT GUWANGE_GEN_LOOP_LOCALS)
    list(APPEND _guwange_gen_definitions REX_LOOP_LOCALS_OFF)
endif()
if(NOT GUWANGE_GEN_COLD_TRAPS)
    list(APPEND _guwange_gen_definitions "REX_TRAP_ATTR=")
endif()
if(NOT GUWANGE_GEN_PROMOTED_CALLS)
    list(APPEND _guwange_gen_definitions REX_PROMOTED_CALLS_OFF)
endif()
if(NOT GUWANGE_GEN_NARROW_LOOP_BARRIER)
    list(APPEND _guwange_gen_definitions REX_LOOP_BARRIER_FULL)
endif()
if(NOT GUWANGE_GEN_DCBT_PREFETCH)
    list(APPEND _guwange_gen_definitions "REX_PREFETCH(x,write)=")
endif()
if(GUWANGE_GEN_VECTOR_LOCALS_BY_VALUE)
    list(APPEND _guwange_gen_definitions REX_VECTOR_LOCALS_BY_VALUE)
endif()
if(NOT GUWANGE_GEN_VPERM1)
    list(APPEND _guwange_gen_definitions REX_VPERM1_OFF)
endif()
if(NOT GUWANGE_GEN_FP_SIGN_OPS)
    list(APPEND _guwange_gen_definitions REX_FP_SIGN_INTEGER)
endif()
if(NOT GUWANGE_GEN_LEAF_DEAD)
    list(APPEND _guwange_gen_definitions REX_LEAF_DEAD_KEEP)
elseif(GUWANGE_GEN_LEAF_DEAD_POISON)
    list(APPEND _guwange_gen_definitions REX_LEAF_DEAD_POISON)
endif()
if(NOT GUWANGE_GEN_SINGLE_FLOATS)
    list(APPEND _guwange_gen_definitions REX_SINGLE_FLOATS_OFF)
endif()
if(NOT GUWANGE_GEN_INLINE_MEMCALLS)
    list(APPEND _guwange_gen_definitions REX_INLINE_MEMCALLS_OFF)
endif()
if(NOT GUWANGE_GEN_VEC_UNALIGNED)
    list(APPEND _guwange_gen_definitions REX_VEC_UNALIGNED_LOOPS)
endif()
if(NOT GUWANGE_GEN_RO_CFG)
    list(APPEND _guwange_gen_definitions REX_RO_CFG_OFF)
endif()
if(NOT GUWANGE_GEN_DEAD_DEFS)
    list(APPEND _guwange_gen_definitions REX_DEAD_DEFS_KEEP)
elseif(GUWANGE_GEN_DEAD_DEFS_POISON)
    list(APPEND _guwange_gen_definitions REX_DEAD_DEFS_POISON)
endif()
if(NOT GUWANGE_GEN_FLUSH_HOIST)
    list(APPEND _guwange_gen_definitions REX_FLUSH_HOIST_OFF)
endif()
if(NOT GUWANGE_GEN_LR_WRITE_SAME)
    list(APPEND _guwange_gen_definitions REX_LR_WRITE_SAME_OFF)
elseif(NOT REXGLUE_PLATFORM_SWITCH)
    # The PC build compares every proven write-back with the value ctx.lr holds ("[verify] codegen checks").
    list(APPEND _guwange_gen_definitions REX_LR_WRITE_SAME_CHECK)
endif()
if(NOT GUWANGE_GEN_R1_LOCAL)
    list(APPEND _guwange_gen_definitions REX_R1_LOCAL_OFF)
elseif(NOT REXGLUE_PLATFORM_SWITCH)
    # The PC build compares the mirror with ctx.r1 after every kept or assumed call ("[verify] codegen checks").
    list(APPEND _guwange_gen_definitions REX_R1_CHECK)
endif()
if(NOT GUWANGE_GEN_VSEL_NEON)
    list(APPEND _guwange_gen_definitions REX_VSEL_OFF)
endif()
if(NOT GUWANGE_GEN_INLINE_LEAVES)
    list(APPEND _guwange_gen_definitions REX_INLINE_LEAVES_OFF)
endif()
# futari-nx's CPU round 1 (sasrt-nx's, through sa2-nx), the Switch's:
#   - the flush-mode checks the control flow fixes compile to nothing (tools/flush_mode_flow.py; the PC build keeps
#     and counts them); OFF defines REX_FLUSH_FLOW_OFF;
#   - guest compare-and-swap without the legacy builtin's trailing `dmb ish` (tools/guest_cas.py); OFF defines
#     REX_GUEST_CAS_LEGACY (the run-time key guwange_cpu_guest_cas_no_barrier puts the barrier back without a build);
#   - guest vector loads and stores reversed with REV64 + EXT instead of a table lookup (tools/vec_byteswap.py); OFF
#     defines REX_BSWAP128_TABLE;
#   - constant word shuffles as one permutation (tools/pch_neon_shuffles.py); OFF defines REX_SIMDE_SHUFFLES;
#   - vperm and the float-to-integer conversions as their NEON instruction (rex/ppc/intrinsics.h); OFF defines
#     REX_VMX_HELPERS_X86_FORM.
if(REXGLUE_PLATFORM_SWITCH)
    option(GUWANGE_SWITCH_FLUSH_FLOW "Flush-mode checks proven by control flow compile to nothing (Switch)" ON)
    option(GUWANGE_SWITCH_GUEST_CAS "Guest compare-and-swap without the trailing dmb (Switch)" ON)
    option(GUWANGE_SWITCH_VEC_BYTESWAP "Guest vector byte reversal as REV64 + EXT (Switch)" ON)
    option(GUWANGE_SWITCH_NEON_SHUFFLES "Constant word shuffles as one permutation (Switch)" ON)
    option(GUWANGE_SWITCH_NEON_VMX_HELPERS "vperm and float-to-integer conversions as NEON instructions (Switch)" ON)
    if(NOT GUWANGE_SWITCH_FLUSH_FLOW)
        list(APPEND _guwange_gen_definitions REX_FLUSH_FLOW_OFF)
    endif()
    if(NOT GUWANGE_SWITCH_GUEST_CAS)
        list(APPEND _guwange_gen_definitions REX_GUEST_CAS_LEGACY)
    endif()
    if(NOT GUWANGE_SWITCH_VEC_BYTESWAP)
        list(APPEND _guwange_gen_definitions REX_BSWAP128_TABLE)
    endif()
    if(NOT GUWANGE_SWITCH_NEON_SHUFFLES)
        list(APPEND _guwange_gen_definitions REX_SIMDE_SHUFFLES)
    endif()
    if(NOT GUWANGE_SWITCH_NEON_VMX_HELPERS)
        list(APPEND _guwange_gen_definitions REX_VMX_HELPERS_X86_FORM)
    endif()
endif()
foreach(target ${GUWANGE_GAME_TARGETS})
    if(TARGET ${target} AND _guwange_gen_definitions)
        target_compile_definitions(${target} PRIVATE ${_guwange_gen_definitions})
    endif()
endforeach()
message(STATUS "guwange: post-codegen steps ${GUWANGE_PERF_CODEGEN_POST}; game code definitions: "
               "${_guwange_gen_definitions}")

# Renut's Direct3D-level renderer (futari_renderer = "renut"; cmake/guwange_renut_gpu.cmake): Guwange's Direct3D
# natively through plume, no PM4 ring and no EDRAM model. Built in by default; off, the build has the emulated GPU only
# (PC).
option(FUTARI_RENUT_RENDERER "Renut's Direct3D-level renderer (futari_renderer = renut)" ON)
if(FUTARI_RENUT_RENDERER)
    include(cmake/guwange_renut_gpu.cmake)
endif()

if(REXGLUE_PLATFORM_SWITCH)
    # Link map: which object (game core) each function of a profile comes from (tools/switch_hot_functions.py).
    target_link_options(guwange PRIVATE "LINKER:-Map=${CMAKE_BINARY_DIR}/guwange.map")
    # The Switch build's fixed settings, compiled in (src/guwange_app.h BuiltInConfig; tools/make_guwange_configs.py
    # writes them). The SD card's guwange.toml holds the player's options and overrides these key by key.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/guwange_switch_defaults.toml" GUWANGE_BUILTIN_CONFIG)
    # And the player's guwange.toml, written next to the NRO when it is missing (src/guwange_app.h DefaultConfigFile).
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/guwange_switch.toml" GUWANGE_PLAYER_CONFIG)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/guwange_builtin_config.h.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/builtin_config/guwange_builtin_config.h" @ONLY)
    target_include_directories(guwange PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/builtin_config")
    target_compile_definitions(guwange PRIVATE GUWANGE_BUILTIN_CONFIG=1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/guwange_switch_defaults.toml" "${CMAKE_CURRENT_SOURCE_DIR}/guwange_switch.toml")
    # futari-nx's Switch compiler optimisations (sa2-nx's set, from nfsmw-nx / UnleashedRecomp-NX):
    #   - LTO: the recompiled default.xex code (guwange_recomp) and the app (renderer) are compiled to GIMPLE and
    #     optimised as one program at the link (-flto=N -flto-partition=balanced: N parallel LTRANS jobs, kept low for
    #     the build machine's memory). The SDK libraries and the four game cores (partially linked with ld -r +
    #     objcopy, which needs machine code) stay out.
    #   - -fipa-pta: interprocedural points-to analysis at the LTO link.
    #   - The link repeats -ffp-contract=off and -fno-strict-aliasing (the LTRANS stage must not fuse what the compile
    #     stage kept apart), and xxHash is built with XXH_FORCE_MEMORY_ACCESS=0 (nfsmw-nx's garbage cache keys).
    # OFF by default (the user, 2026-10-21): test builds go without it (the LTO link alone takes most of an hour); the
    # final build turns it on (tools/win/build_switch.ps1 -Lto on).
    option(GUWANGE_SWITCH_LTO "Link-time optimisation of the default.xex code and the app (Switch)" OFF)
    set(GUWANGE_SWITCH_LTO_JOBS "2" CACHE STRING "Parallel LTRANS jobs of the Switch LTO link")
    option(GUWANGE_SWITCH_IPA_PTA "-fipa-pta at the Switch LTO link" ON)
    target_compile_definitions(guwange PRIVATE XXH_FORCE_MEMORY_ACCESS=0)
    if(GUWANGE_SWITCH_LTO)
        set(_guwange_lto_compile -flto -fno-fat-lto-objects)
        set(_guwange_lto_link -flto=${GUWANGE_SWITCH_LTO_JOBS} -flto-partition=balanced -ffp-contract=off
                              -fno-strict-aliasing)
        if(GUWANGE_SWITCH_IPA_PTA)
            list(APPEND _guwange_lto_compile -fipa-pta)
            list(APPEND _guwange_lto_link -fipa-pta)
        endif()
        foreach(target guwange_recomp guwange)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE ${_guwange_lto_compile})
            endif()
        endforeach()
        target_link_options(guwange PRIVATE ${_guwange_lto_link})
        message(STATUS "guwange: Switch LTO on (${GUWANGE_SWITCH_LTO_JOBS} LTRANS jobs, -fipa-pta ${GUWANGE_SWITCH_IPA_PTA})")
    endif()
    # No x29 frame chain in the game code (futari-nx's CPU round 1): one more callee-saved register for the recompiled
    # code. The SDK and the app keep theirs (the console profiler walks x29, bounds-checked).
    option(GUWANGE_SWITCH_OMIT_FRAME_POINTER "Game code without the x29 frame chain (Switch)" ON)
    if(GUWANGE_SWITCH_OMIT_FRAME_POINTER)
        foreach(target ${GUWANGE_GAME_TARGETS})
            if(TARGET ${target} AND NOT target STREQUAL "guwange")
                target_compile_options(${target} PRIVATE -fomit-frame-pointer)
            endif()
        endforeach()
    endif()
    # Line tables for the app's own code (addr2line on the archived ELF); DWARF 4 (sa2-nx: DWARF 5 mapped some LTO
    # functions' lines to the wrong file). The generated game code stays without.
    option(GUWANGE_SWITCH_LINE_INFO "Line tables (-g1) for the app's own code in the Switch ELF" ON)
    if(GUWANGE_SWITCH_LINE_INFO)
        target_compile_options(guwange PRIVATE -g1 -gdwarf-4)
        target_link_options(guwange PRIVATE -g1 -gdwarf-4)
    endif()
    # Profile-guided optimisation (futari-nx's, sa2-nx's design; the user, 2026-10-22), for the generated default.xex
    # code and the app (the four game cores, partially linked with ld -r + objcopy, stay out as in futari-nx):
    #   GUWANGE_PGO=generate  an instrumented NRO that writes its branch counts to pgo/ next to it on the SD card
    #                         (src/guwange_pgo.cpp: registered in a section, not libgcov's exit-time files, which do not
    #                         fit the Switch). Value profiling is off: its shared lists are not safe without atomics.
    #   GUWANGE_PGO=use       the counts merged on the PC (tools/win/pgo_merge.ps1). Code the sessions did not run is
    #                         compiled as without a profile (-fprofile-partial-training), and functions changed since
    #                         the instrumented build keep no profile (-Wno-coverage-mismatch).
    # The profile is pgo/ at the root of the repository (GUWANGE_PGO_DIR): one .gcda per object, named by the object's
    # path in the build folder with # for each separator (-fprofile-prefix-path: the instrumented build's folder,
    # out/switch_pgo, and the release's, out/switch, give the same names).
    set(GUWANGE_PGO "" CACHE STRING "Profile-guided optimisation of the Switch build: empty, generate or use")
    set_property(CACHE GUWANGE_PGO PROPERTY STRINGS "" generate use)
    set(GUWANGE_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Folder of the PGO profile (.gcda files)")
    get_filename_component(_guwange_pgo_dir "${GUWANGE_PGO_DIR}" ABSOLUTE)
    file(TO_NATIVE_PATH "${_guwange_pgo_dir}" _guwange_pgo_dir)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _guwange_pgo_prefix)
    if(GUWANGE_PGO STREQUAL "generate")
        set(_guwange_pgo "-fprofile-generate=${_guwange_pgo_dir}" "-fprofile-prefix-path=${_guwange_pgo_prefix}"
                         -fno-profile-values -fprofile-update=single -fprofile-info-section=gcov_info)
        target_compile_definitions(guwange PRIVATE GUWANGE_PGO_GENERATE=1)
    elseif(GUWANGE_PGO STREQUAL "use")
        if(NOT EXISTS "${GUWANGE_PGO_DIR}")
            message(FATAL_ERROR "GUWANGE_PGO=use: no profile in ${GUWANGE_PGO_DIR}")
        endif()
        # -fprofile-correction: counts taken without atomic updates (-fprofile-update=single) while several game
        # threads ran the same code lose a few increments; GCC smooths them instead of stopping.
        set(_guwange_pgo "-fprofile-use=${_guwange_pgo_dir}" "-fprofile-prefix-path=${_guwange_pgo_prefix}"
                         -fprofile-correction -fno-profile-values -fprofile-partial-training -Wno-missing-profile
                         -Wno-coverage-mismatch)
    elseif(GUWANGE_PGO)
        message(FATAL_ERROR "GUWANGE_PGO must be empty, generate or use (got ${GUWANGE_PGO})")
    endif()
    if(GUWANGE_PGO)
        # Renut's renderer too (an object library of the app: the render thread's code).
        foreach(target guwange_recomp guwange guwange_renut_gpu)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE ${_guwange_pgo})
            endif()
        endforeach()
        target_link_options(guwange PRIVATE ${_guwange_pgo})
        message(STATUS "guwange: PGO ${GUWANGE_PGO}")
    endif()
    # The homebrew menu's entry: name, author, version and icon (app/guwange_icon.jpg, 256x256 JPEG).
    set(_guwange_nro_icon "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/guwange_icon.jpg")
        set(_guwange_nro_icon ICON "${CMAKE_CURRENT_SOURCE_DIR}/guwange_icon.jpg")
    endif()
    rexglue_switch_add_nro(guwange
        NAME "Guwange"
        AUTHOR "ChanseyIsTheBest"
        VERSION "1.0.1"
        ${_guwange_nro_icon})
endif()
