cmake_minimum_required(VERSION 3.10)
project(LiteSpeedStore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
enable_testing()

set(LITESPEED_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
)
FetchContent_MakeAvailable(Catch2)

option(SANITIZE_ADDRESS "Build with AddressSanitizer + UBSan" OFF)
option(SANITIZE_THREAD  "Build with ThreadSanitizer"          OFF)

if(SANITIZE_ADDRESS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(SANITIZE_THREAD)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

add_library(litespeed_core
    src/StorageEngine.cpp
    src/WAL.cpp
    src/Snapshot.cpp
)
target_include_directories(litespeed_core PUBLIC include)
target_link_libraries(litespeed_core PUBLIC Threads::Threads)
target_compile_options(litespeed_core PRIVATE ${LITESPEED_WARNING_FLAGS})

add_executable(LiteSpeedStore
    src/main.cpp
)
target_link_libraries(LiteSpeedStore PRIVATE litespeed_core)
target_compile_options(LiteSpeedStore PRIVATE ${LITESPEED_WARNING_FLAGS})

add_executable(TestPersistence
    src/test_persistence.cpp
)
target_link_libraries(TestPersistence PRIVATE litespeed_core Catch2::Catch2WithMain)
target_compile_options(TestPersistence PRIVATE ${LITESPEED_WARNING_FLAGS})
add_test(NAME TestPersistence COMMAND TestPersistence)

add_executable(Benchmark
    benchmark/bench_litespeed.cpp
)
target_link_libraries(Benchmark PRIVATE litespeed_core)
target_compile_options(Benchmark PRIVATE ${LITESPEED_WARNING_FLAGS})

# Verifier CLI + end-to-end tamper-evidence demo.
add_executable(litespeed-verify tools/verify.cpp)
target_link_libraries(litespeed-verify PRIVATE litespeed_core)
target_compile_options(litespeed-verify PRIVATE ${LITESPEED_WARNING_FLAGS})

add_executable(litespeed-demo tools/demo.cpp)
target_link_libraries(litespeed-demo PRIVATE litespeed_core)
target_compile_options(litespeed-demo PRIVATE ${LITESPEED_WARNING_FLAGS})

# Smoke test: the demo must end-to-end detect the injected tampering.
add_test(NAME Demo COMMAND litespeed-demo)
set_tests_properties(Demo PROPERTIES PASS_REGULAR_EXPRESSION "TAMPERING DETECTED")

# Fuzzing — requires clang: cmake -DFUZZ=ON -DCMAKE_CXX_COMPILER=clang++
option(FUZZ "Build libFuzzer harness (requires clang -fsanitize=fuzzer)" OFF)

if(FUZZ)
    set(FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)

    # Separate instrumented library so the normal build is unaffected.
    add_library(litespeed_core_fuzz
        src/StorageEngine.cpp
        src/WAL.cpp
        src/Snapshot.cpp
    )
    target_include_directories(litespeed_core_fuzz PUBLIC include)
    target_link_libraries(litespeed_core_fuzz PUBLIC Threads::Threads)
    target_compile_options(litespeed_core_fuzz PRIVATE ${LITESPEED_WARNING_FLAGS} ${FUZZ_FLAGS})

    add_executable(FuzzWAL fuzz/fuzz_wal.cpp)
    target_link_libraries(FuzzWAL PRIVATE litespeed_core_fuzz)
    target_compile_options(FuzzWAL PRIVATE ${FUZZ_FLAGS})
    target_link_options(FuzzWAL PRIVATE ${FUZZ_FLAGS})

    add_executable(FuzzSnapshot fuzz/fuzz_snapshot.cpp)
    target_link_libraries(FuzzSnapshot PRIVATE litespeed_core_fuzz)
    target_compile_options(FuzzSnapshot PRIVATE ${FUZZ_FLAGS})
    target_link_options(FuzzSnapshot PRIVATE ${FUZZ_FLAGS})
endif()
