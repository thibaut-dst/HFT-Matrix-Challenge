#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=build
BIN_DIR=${BUILD_DIR}/bin

echo "Cleaning previous build..."
rm -rf "${BUILD_DIR}"

echo "Configuring..."
# If older fetched CMakeLists cause policy errors, the -DCMAKE_POLICY_VERSION_MINIMUM fallback helps.
cmake -S . -B "${BUILD_DIR}" -DCMAKE_POLICY_VERSION_MINIMUM=3.5

echo "Building..."
cmake --build "${BUILD_DIR}" -j$(nproc 2>/dev/null || echo 4)

echo "Copying binaries to project root for convenience..."
cp -f "${BIN_DIR}/hftserver2026" ./hftserver2026 || true
cp -f "${BIN_DIR}/hftclient2026" ./hftclient2026 || true
chmod +x ./hftserver2026 ./hftclient2026 || true

echo "Build complete."
echo "Server: ${BIN_DIR}/hftserver2026 (also copied to ./hftserver2026)"
echo "Client: ${BIN_DIR}/hftclient2026 (also copied to ./hftclient2026)"

