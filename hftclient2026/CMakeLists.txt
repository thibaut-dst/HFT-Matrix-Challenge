cmake_minimum_required(VERSION 3.16)
project(hftclient2026 LANGUAGES CXX)

add_executable(hftclient2026 main.cpp)

target_link_libraries(hftclient2026 PRIVATE nlohmann_json::nlohmann_json)

# ── Aggressive optimisation flags ────────────────────────────────────────────
target_compile_options(hftclient2026 PRIVATE
    -O3
    -march=native
    -funroll-loops
    -ffast-math
    -Wall -Wextra
)

# ── OpenMP (parallel tile loops in matmul) ────────────────────────────────────
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    target_link_libraries(hftclient2026 PRIVATE OpenMP::OpenMP_CXX)
    message(STATUS "OpenMP found — parallel matmul enabled")
else()
    message(WARNING "OpenMP NOT found — matmul will be single-threaded")
endif()

# ── pthreads ──────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(hftclient2026 PRIVATE Threads::Threads)
