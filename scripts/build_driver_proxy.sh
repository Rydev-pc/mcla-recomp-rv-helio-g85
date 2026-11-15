#!/usr/bin/env bash
# Build the Vulkan driver proxy (libvulkan.so) for Android arm64-v8a.
# This proxy intercepts Vulkan calls from libmain.so and routes them through
# libadrenotools to either the system driver or a custom driver package.
#
# Usage:
#   ANDROID_NDK_ROOT=/path/to/ndk ./scripts/build_driver_proxy.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
NATIVE_DIR="$PROJECT_ROOT/native"
BUILD_DIR="$PROJECT_ROOT/out/build/driver_proxy"
OUTPUT_DIR="$PROJECT_ROOT/app/src/main/jniLibs/arm64-v8a"

if [[ -z "${ANDROID_NDK_ROOT:-}" ]]; then
  echo "ERRO: defina ANDROID_NDK_ROOT antes de executar este script." >&2
  exit 1
fi

echo "=== Building MCLA Vulkan Driver Proxy (arm64-v8a) ==="
echo "Project:  $PROJECT_ROOT"
echo "NDK:      $ANDROID_NDK_ROOT"
echo

# Clone libadrenotools if not vendored
if [[ ! -d "$NATIVE_DIR/vendor/libadrenotools" ]]; then
  echo "[1/3] Cloning libadrenotools..."
  mkdir -p "$NATIVE_DIR/vendor"
  git clone --depth=1 https://github.com/bylaws/libadrenotools.git "$NATIVE_DIR/vendor/libadrenotools"
fi

echo "[2/3] Configuring CMake..."
cmake -S "$NATIVE_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release

echo "[3/3] Building..."
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

echo
echo "=== Copying libvulkan.so to jniLibs ==="
mkdir -p "$OUTPUT_DIR"
cp -f "$BUILD_DIR/libvulkan.so" "$OUTPUT_DIR/"
echo "  $OUTPUT_DIR/libvulkan.so"
echo
echo "Done! The proxy is ready to be loaded by MclaActivity."
