cmake_minimum_required(VERSION 3.15)
project(LockDetector CXX)

if(NOT WIN32)
  message(FATAL_ERROR "LockDetector is Windows only")
endif()

set(DETOURS_ROOT "" CACHE PATH "Path to a checkout of microsoft/Detours")
if(NOT EXISTS "${DETOURS_ROOT}/src/detours.h")
  message(FATAL_ERROR "Set -DDETOURS_ROOT=<Detours checkout> (src/detours.h not found)")
endif()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)  # keep PDBs in Release so CI crash traces are symbolized
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
  set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Detours is built straight from source (same file set as its Makefile).
add_library(detours STATIC
  ${DETOURS_ROOT}/src/creatwth.cpp
  ${DETOURS_ROOT}/src/detours.cpp
  ${DETOURS_ROOT}/src/disasm.cpp
  ${DETOURS_ROOT}/src/image.cpp
  ${DETOURS_ROOT}/src/modules.cpp
  ${DETOURS_ROOT}/src/disolx64.cpp
  ${DETOURS_ROOT}/src/disolx86.cpp
  ${DETOURS_ROOT}/src/disolia64.cpp
  ${DETOURS_ROOT}/src/disolarm.cpp
  ${DETOURS_ROOT}/src/disolarm64.cpp)
target_include_directories(detours PUBLIC ${DETOURS_ROOT}/src)

add_library(LockDetector SHARED
  LockDetector/LockDetector.cpp
  LockDetector/LockTracker.cpp
  LockDetector/LockHooks.cpp
  LockDetector/LockDetector.def)
target_compile_definitions(LockDetector PRIVATE
  _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(LockDetector PRIVATE detours)

add_executable(LockViewer WIN32 LockViewer/LockViewer.cpp)
target_compile_definitions(LockViewer PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(LockViewer PRIVATE comctl32)
if(MSVC)
  set_target_properties(LockViewer PROPERTIES LINK_FLAGS "/ENTRY:wWinMainCRTStartup")
endif()

add_executable(InversionDemo Examples/InversionDemo/InversionDemo.cpp)
target_compile_definitions(InversionDemo PRIVATE _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN)
target_link_libraries(InversionDemo PRIVATE dbghelp)
