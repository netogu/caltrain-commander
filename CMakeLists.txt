cmake_minimum_required(VERSION 3.16)
project(caltrain_commander C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CURSES_NEED_NCURSES TRUE)
find_package(Curses REQUIRED)
find_package(Threads REQUIRED)

# Schedule model + loader + fetch: no UI dependencies, so it is testable alone.
add_library(gtfs STATIC src/csv.c src/gtfs.c src/fetch.c)
target_include_directories(gtfs PUBLIC src)

add_executable(caltrain src/main.c src/ui.c)
target_include_directories(caltrain PRIVATE ${CURSES_INCLUDE_DIRS})
target_link_libraries(caltrain PRIVATE gtfs ${CURSES_LIBRARIES} Threads::Threads)

add_executable(test_gtfs tests/test_gtfs.c)
target_link_libraries(test_gtfs PRIVATE gtfs)

foreach(t gtfs caltrain test_gtfs)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${t} PRIVATE -Wall -Wextra)
  endif()
endforeach()

enable_testing()
add_test(NAME gtfs COMMAND test_gtfs ${CMAKE_SOURCE_DIR}/tests/fixture)
