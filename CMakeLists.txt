# =============================================================================
# Trails in the Sky FC - static recompilation host (32-bit, MSVC x86)
#
#   py -3 run_lift.py --all  # -> src/recomp/gen/  (needs work/functions.json)
#   build.cmd                # vcvarsall amd64_x86 + CMake + Ninja -> build/trails.exe
#   build\trails.exe --run
# =============================================================================
cmake_minimum_required(VERSION 3.20)
# The C runtime linked in (/MT): trails.exe runs on a PC without the x86
# Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(trails C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "the host must be 32-bit: use build.cmd (vcvarsall amd64_x86)")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "runtime/native32 is MSVC x86 inline assembly")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(PCRECOMP "${CMAKE_SOURCE_DIR}/pcrecomp" CACHE PATH "pcrecomp checkout")
set(GEN_DIR "${CMAKE_SOURCE_DIR}/src/recomp/gen" CACHE PATH "the lifted C (run_lift.py --out)")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "${GEN_DIR} is empty -- lift the game first: py -3 run_lift.py --all")
endif()

add_executable(trails
  src/runtime/host.c
  src/runtime/profile.c
  src/runtime/voice_out.c
  src/mods/vfs.c
  src/mods/voice.c
  src/common/crt.c
  src/common/hot.c
  src/common/frame_option.c
  src/ui/settings_menu.cpp
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_dx9.cpp
  third_party/imgui/backends/imgui_impl_win32.cpp
  ${PCRECOMP}/runtime/native32/native32.c
  ${PCRECOMP}/runtime/recomp32/image_loader.c
  ${PCRECOMP}/runtime/recomp32/recomp_trace.c
  ${GEN_SOURCES}
)
target_include_directories(trails PRIVATE
  src/runtime src/mods src/ui third_party/imgui ${GEN_DIR} ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
# RECOMP_RETADDR=0u: a stack imbalance during bring-up leaks a harmless 0, not
# 0xDEAD0000 into a size argument.
target_compile_definitions(trails PRIVATE RECOMP_RETADDR=0u _CRT_SECURE_NO_WARNINGS
                                        RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS)
# Linked high and not relocatable, with a small first stack, so the guest's
# 0x00400000..0x01955000 is free (ed6_win_DX9.exe has no relocations): the
# lifted code runs on a thread with a large stack (host.c, guest_main).
target_link_options(trails PRIVATE /BASE:0x60000000 /FIXED:NO /DYNAMICBASE:NO
                                   /NXCOMPAT /LARGEADDRESSAWARE /STACK:0x40000
                                   /MANIFESTUAC:NO)
set(GEN_OPT "/O2" CACHE STRING "optimisation for the generated C")
# Visual Studio 2026's compiler (19.50 and later) drops the sign test of a
# 16-bit value shifted to the top of a register (found on Red Alert 2).
if(MSVC_VERSION GREATER_EQUAL 1950)
  string(APPEND GEN_OPT ";/d2SSAOptimizer-")
endif()
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;${GEN_OPT}")
set_source_files_properties(src/runtime/host.c src/runtime/profile.c src/runtime/voice_out.c
                            src/mods/vfs.c src/mods/voice.c src/common/crt.c src/common/hot.c
                            src/common/frame_option.c
                            PROPERTIES COMPILE_OPTIONS "/W3")
target_link_libraries(trails PRIVATE user32 gdi32 kernel32 winmm ole32 dbghelp psapi dsound shell32)
# The exe's own imports are system DLLs and come from the system folder, also
# when it is run from a folder that holds a mod's dinput8.dll or winmm.dll.
target_link_options(trails PRIVATE /DEPENDENTLOADFLAG:0x800)

option(FC_TRACE "record a ring buffer of lifted function entries" OFF)
if(FC_TRACE)
  target_compile_definitions(trails PRIVATE RECOMP_TRACE=1)
endif()
