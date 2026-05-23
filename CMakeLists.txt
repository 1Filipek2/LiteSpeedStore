cmake_minimum_required(VERSION 3.10)
project(LiteSpeedStore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
enable_testing()

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

add_executable(LiteSpeedStore
    src/main.cpp
)
target_link_libraries(LiteSpeedStore PRIVATE litespeed_core)

add_executable(TestPersistence
    src/test_persistence.cpp
)
target_link_libraries(TestPersistence PRIVATE litespeed_core)
add_test(NAME TestPersistence COMMAND TestPersistence)
