cmake_minimum_required(VERSION 3.0.0)
project(msgex VERSION 0.1.0)

include(CTest)
enable_testing()

add_subdirectory(devils)


add_executable(msgex core/msgex.c)

target_link_libraries(msgex devils winmm ws2_32)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
