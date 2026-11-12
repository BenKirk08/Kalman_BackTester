cmake_minimum_required(VERSION 3.10)
project(BackTester)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#default to an optimised build. the project used to be built with no optimisation flags at all (CMAKE_BUILD_TYPE
#empty), which made it ~20x slower than it needed to be. -O3 comes from Release itself
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "build type (Release, Debug, RelWithDebInfo)" FORCE)
endif()

#tune for the machine it is built on. apple silicon spells it -mcpu=native, x86 -march=native.
#NOT -ffast-math: that lets the compiler reorder floating point sums, and the regression checks compare results
#against the legacy pipeline to 1e-9
#-DBACKTESTER_NATIVE=OFF builds for a generic CPU of the same family (what CI does). results are byte-reproducible
#on one machine and compiler, another CPU, compiler or maths library can move the last digit
option(BACKTESTER_NATIVE "tune for the CPU the program is built on (-mcpu=native / -march=native)" ON)
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mcpu=native" HAS_MCPU_NATIVE)
check_cxx_compiler_flag("-march=native" HAS_MARCH_NATIVE)
if(NOT BACKTESTER_NATIVE)
    #nothing added
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" AND HAS_MCPU_NATIVE)
    add_compile_options(-mcpu=native)
elseif(HAS_MARCH_NATIVE)
    add_compile_options(-march=native)
endif()

# developer builds, all OFF by default so the normal build (and every result) is exactly what it was:
#cmake -B build-dev -DBACKTESTER_WARNINGS=ON                         -> -Wall -Wextra -Wpedantic -Wshadow -Wconversion
#cmake -B build-dev -DBACKTESTER_WARNINGS=ON -DBACKTESTER_WERROR=ON  -> the same, and any warning stops the build (CI)
#cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBACKTESTER_SANITIZE=ON  -> AddressSanitizer + UndefinedBehaviorSanitizer
option(BACKTESTER_WARNINGS "build with -Wall -Wextra -Wpedantic -Wshadow -Wconversion" OFF)
option(BACKTESTER_WERROR "treat warnings as errors" OFF)
option(BACKTESTER_SANITIZE "build with AddressSanitizer and UndefinedBehaviorSanitizer (use a Debug build)" OFF)
if(BACKTESTER_WARNINGS)
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()
if(BACKTESTER_WERROR)
    add_compile_options(-Werror)
endif()
if(BACKTESTER_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=address,undefined") # add_link_options -> needs cmake 3.13
endif()

find_package(Threads REQUIRED)

#all project files
add_executable(BackTester
    main.cpp
    DataLoader.cpp
    Cointegration.cpp
    Kalman.cpp
    Signals.cpp
    Trading.cpp
    Evaluation.cpp
    Filters.cpp
    Backtest.cpp
    Metrics.cpp
    WalkForward.cpp
    Holdout.cpp
    Market.cpp
    Engine.cpp
    Screen.cpp
    Verify.cpp
    Bench.cpp
    Research.cpp
)
target_link_libraries(BackTester PRIVATE Threads::Threads)

#build with:   cmake -B build && cmake --build build -j
