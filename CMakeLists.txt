cmake_minimum_required(VERSION 3.16)
project(RealtimeCollaborativeApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(COLLAB_WITH_POSTGRES "Enable PostgreSQL persistence via libpq" OFF)

add_executable(collab_server src/main.cpp)
target_include_directories(collab_server PRIVATE src)

add_executable(ot_tests tests/ot_tests.cpp)
target_include_directories(ot_tests PRIVATE src)

if (WIN32)
    target_link_libraries(collab_server PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(collab_server PRIVATE Threads::Threads)
endif()

if (COLLAB_WITH_POSTGRES)
    find_package(PostgreSQL REQUIRED)
    target_compile_definitions(collab_server PRIVATE COLLAB_WITH_POSTGRES)
    target_include_directories(collab_server PRIVATE ${PostgreSQL_INCLUDE_DIRS})
    target_link_libraries(collab_server PRIVATE ${PostgreSQL_LIBRARIES})
endif()

enable_testing()
add_test(NAME ot_tests COMMAND ot_tests)
