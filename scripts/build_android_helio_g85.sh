#!/usr/bin/env bash
# Build script para MCLA Recomp RV — perfil Helio G85 (Mali-G52 MC2, 4 GB RAM, Android)
#
# Pré-requisitos:
#   - Android NDK r25+ (defina ANDROID_NDK_HOME)
#   - CMake 3.25+
#   - Ninja
#   - llvm-mingw (para a toolchain do SDK ReXGlue, se necessário)
#
# Uso:
#   ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/26.1.10909125 \
#   ./scripts/build_android_helio_g85.sh release
#
# O script gera um APK em out/build/android-arm64-helio-g85-release/
# e assina com a chave de debug do Android (chaves de release não são commitedas).

set -euo pipefail

BUILD_TYPE="${1:-release}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
  echo "ERRO: defina ANDROID_NDK_HOME antes de executar este script." >&2
  exit 1
fi

PRESET="android-arm64-helio-g85-${BUILD_TYPE}"

echo "=== MCLA Recomp RV — Build Helio G85 / Mali-G52 ==="
echo "Projeto: $PROJECT_ROOT"
echo "Preset:  $PRESET"
echo "NDK:     $ANDROID_NDK_HOME"
echo

cd "$PROJECT_ROOT"

echo "[1/4] Configurando CMake..."
cmake --preset "$PRESET"

echo "[2/4] Compilando..."
cmake --build --preset "$PRESET" -j"$(nproc)"

echo "[3/4] Gerando APK..."
# O SDK ReXGlue produz o libmain.so em out/build/<preset>/
# O Android Gradle (se disponível) empacota o APK. Caso contrário, avise o usuário.
if command -v gradle >/dev/null 2>&1; then
  cd android
  gradle assembleDebug
  cd "$PROJECT_ROOT"
else
  echo "AVISO: Gradle não encontrado. O .so foi compilado; empacote o APK manualmente."
fi

echo "[4/4] Pronto!"
echo "  Binário: out/build/$PRESET/libmain.so"
echo "  Nota: o APK precisa de assets do jogo (arquivos Xbox 360 legalmente obtidos)."
echo "        Não commite APKs, chaves de assinatura ou assets do jogo."
