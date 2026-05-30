cmake_minimum_required(VERSION 3.16)
project(hftproject2026 LANGUAGES CXX)

# Use a consistent policy level so older fetched projects don't fail.
cmake_policy(VERSION 3.16)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put all runtime binaries in build/bin for easy discovery
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# On macOS, pick the active SDK via xcrun to avoid hardcoded -isysroot issues
if(APPLE)
  execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
                  OUTPUT_VARIABLE _sdk_path OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(_sdk_path)
    message(STATUS "Using macOS SDK: ${_sdk_path}")
    set(CMAKE_OSX_SYSROOT "${_sdk_path}" CACHE PATH "macOS SDK" FORCE)
  endif()
endif()

# Fetch nlohmann/json once for both subprojects
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.2
)
FetchContent_MakeAvailable(nlohmann_json)

# Add subprojects
add_subdirectory(hftserver2026)
add_subdirectory(hftclient2026)

# add tools (blast server)
add_subdirectory(tools)

# Convenience target to build both
add_custom_target(all_bins DEPENDS hftserver2026 hftclient2026)

