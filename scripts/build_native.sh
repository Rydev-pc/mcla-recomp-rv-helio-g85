#!/usr/bin/env bash
# Build the MCLA game native library (libmain.so) for Android arm64-v8a.
#
# This script compiles the MCLA recompiled game code (from mcla-recomp/)
# together with the RexGlue SDK, SDL3, and the driver proxy, producing
# libmain.so staged in app/src/main/jniLibs/arm64-v8a/.
#
# Usage:
#   ANDROID_NDK_ROOT=/path/to/ndk \
#   ANDROID_SDK_ROOT=/path/to/sdk \
#   REXSDK_DIR=/path/to/rexglue-sdk \
#   SDL3_DIR=/path/to/sdl3-install \
#   ./scripts/build_native.sh [release|debug]
set -euo pipefail

BUILD_TYPE="${1:-release}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ -z "${ANDROID_NDK_ROOT:-}" ]]; then
  echo "ERRO: defina ANDROID_NDK_ROOT antes de executar este script." >&2
  exit 1
fi
if [[ -z "${ANDROID_SDK_ROOT:-}" && -z "${ANDROID_HOME:-}" ]]; then
  echo "ERRO: defina ANDROID_SDK_ROOT (ou ANDROID_HOME)." >&2
  exit 1
fi
if [[ -z "${REXSDK_DIR:-}" ]]; then
  echo "ERRO: defina REXSDK_DIR (caminho para o rexglue-sdk)." >&2
  exit 1
fi
if [[ -z "${SDL3_DIR:-}" ]]; then
  echo "ERRO: defina SDL3_DIR (caminho para a instalação do SDL3)." >&2
  exit 1
fi

OUTPUT_DIR="$PROJECT_ROOT/app/src/main/jniLibs/arm64-v8a"
BUILD_DIR="$PROJECT_ROOT/out/build/mcla-android-$BUILD_TYPE"

echo "=== Building MCLA Native Library (arm64-v8a, $BUILD_TYPE) ==="
echo "Project:   $PROJECT_ROOT"
echo "NDK:       $ANDROID_NDK_ROOT"
echo "SDK:       ${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
echo "RexGlue:   $REXSDK_DIR"
echo "SDL3:      $SDL3_DIR"
echo "Build:     $BUILD_TYPE"
echo

mkdir -p "$OUTPUT_DIR"
mkdir -p "$BUILD_DIR"

cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_shared \
  -DREXSDK_DIR="$REXSDK_DIR" \
  -DSDL3_DIR="$SDL3_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DMCLA_TARGET_PROFILE=helio-g85

echo
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

echo
echo "=== Staging libraries to jniLibs ==="
cp -f "$BUILD_DIR/libmain.so" "$OUTPUT_DIR/" 2>/dev/null || echo "  AVISO: libmain.so não encontrado (verifique o build)"
echo "  $OUTPUT_DIR/libmain.so"
echo
echo "Done! Native library staged."
