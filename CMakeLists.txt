cmake_minimum_required(VERSION 3.10)
project(LiteSpeedStore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(LiteSpeedStore 
    src/main.cpp 
    src/StorageEngine.cpp
    src/WAL.cpp
)

add_executable(TestPersistence
    src/test_persistence.cpp
    src/StorageEngine.cpp
    src/WAL.cpp
)
target_link_libraries(TestPersistence PRIVATE Threads::Threads)

find_package(Threads REQUIRED)
target_link_libraries(LiteSpeedStore PRIVATE Threads::Threads)