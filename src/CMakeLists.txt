cmake_minimum_required(VERSION 3.0.0)
project(devils VERSION 0.1.0)

include(CTest)
enable_testing()

add_subdirectory(devils)


add_executable(devils-cli core/client.c)

add_executable(devils-svr core/server.c)

target_link_libraries(devils-svr devils)

target_link_libraries(devils-cli devils)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
