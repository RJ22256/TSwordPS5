# Offline Eden shader translator (src/shader_recompiler, SPIR-V backend only) for comparing it with Ryujinx's.
# Sources come from refs/eden (GPL-3.0-or-later / GPL-2.0-or-later) and refs/sirit; common/settings.h, logging.h and
# assert.h are replaced by the stand-ins in shim/.
cmake_minimum_required(VERSION 3.20)
project(eden_xlate CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REFS ${CMAKE_CURRENT_LIST_DIR}/../../refs)
set(EDEN ${REFS}/eden/src)

set(SPIRV-Headers_DIR ${CMAKE_CURRENT_LIST_DIR}/cmake)
set(SIRIT_USE_SYSTEM_SPIRV_HEADERS ON CACHE BOOL "" FORCE)
set(SIRIT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${REFS}/sirit sirit EXCLUDE_FROM_ALL)

find_package(fmt REQUIRED)
find_package(Boost REQUIRED)

file(GLOB_RECURSE RECOMPILER_SOURCES ${EDEN}/shader_recompiler/*.cpp)
list(FILTER RECOMPILER_SOURCES EXCLUDE REGEX "/backend/(glsl|glasm)/")

add_executable(eden_xlate eden_xlate.cpp ${RECOMPILER_SOURCES})
target_include_directories(eden_xlate BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/shim)
target_include_directories(eden_xlate PRIVATE ${EDEN})
target_link_libraries(eden_xlate PRIVATE sirit::sirit fmt::fmt Boost::headers)
target_compile_options(eden_xlate PRIVATE -O2 -march=x86-64-v3 -fbracket-depth=1024 -w -include ${CMAKE_CURRENT_LIST_DIR}/shim/eden_fmt_enums.h)

# sirit v1.0.5 uses std::abort without <cstdlib> (MSYS2 CLANG64's libc++ does not pull it in) and builds with
# -Werror; both are adjusted from here instead of editing the reference clone.
foreach(t siritobj sirit)
  if(TARGET ${t})
    target_compile_options(${t} PRIVATE -include cstdlib -Wno-error)
  endif()
endforeach()
