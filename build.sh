#!/usr/bin/env bash
set -euo pipefail

# Print usage if --help or -h is passed
if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    echo "Usage: ./build.sh [options]"
    echo "Options:"
    echo "  --clean       Remove the build directory before compiling"
    echo "  --test        Run the CTest suite after a successful build"
    echo "  -h, --help    Show this help message"
    exit 0
fi

# Directory of the script
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
export PATH="${PROJECT_DIR}/odb_gcc_bin:${PATH}"

# Check for --clean
if [[ "${1:-}" == "--clean" ]]; then
    echo "Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
    # Shift arguments only if there was one
    if [[ $# -gt 0 ]]; then shift; fi
fi

# Create build directory
mkdir -p "${BUILD_DIR}"

echo "Configuring project with CMake..."
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DNOVABOOT_BUILD_TESTS=ON

echo "Building project..."
cmake --build "${BUILD_DIR}" -j"$(nproc)"

# Check for --test
if [[ "${1:-}" == "--test" ]]; then
    echo "Running tests..."
    cd "${BUILD_DIR}"
    ctest --output-on-failure
fi

echo "Build complete! Artifacts are in ${BUILD_DIR}/"
