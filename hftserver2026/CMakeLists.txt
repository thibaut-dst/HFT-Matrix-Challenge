cmake_minimum_required(VERSION 3.16)
project(hftserver2026 LANGUAGES CXX)

add_executable(hftserver2026 main.cpp)

# Make the json header available
target_link_libraries(hftserver2026 PRIVATE nlohmann_json::nlohmann_json)

# Optional: warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "AppleClang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  target_compile_options(hftserver2026 PRIVATE -Wall -Wextra -Wpedantic)
endif()

