cmake_minimum_required(VERSION 3.16)
project(hftclient2026 LANGUAGES CXX)

add_executable(hftclient2026 main.cpp)

target_link_libraries(hftclient2026 PRIVATE nlohmann_json::nlohmann_json)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "AppleClang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  target_compile_options(hftclient2026 PRIVATE -Wall -Wextra -Wpedantic)
endif()

