# Stage 0 of the arm64 plan: arm64run, a standalone loader that runs an arm64 iOS-simulator Mach-O on x86_64 Linux
# in dynarmic, with libSystem imports bound to native x86 thunks. Stage 1 moves this into mldr.
cmake_minimum_required(VERSION 3.20)
project(arm64run CXX)
set(CMAKE_CXX_STANDARD 20)
set(DYNARMIC_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../external/dynarmic CACHE PATH "dynarmic source")
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS A64 CACHE STRING "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory(${DYNARMIC_SRC} dynarmic EXCLUDE_FROM_ALL)
add_executable(arm64run arm64run.cpp macho.cpp thunks.cpp lse.cpp)
target_compile_options(arm64run PRIVATE -O2 -mcx16 -Wall)
target_link_libraries(arm64run PRIVATE dynarmic pthread)
