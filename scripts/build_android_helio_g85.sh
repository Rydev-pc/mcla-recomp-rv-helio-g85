#!/usr/bin/env bash
# Build script para MCLA Recomp RV — perfil Helio G85 (Mali-G52 MC2, 4 GB RAM, Android)
#
# Estratégia baseada no Skate 3 Mobile: não reescrever a renderização para Mali;
# manter o System Driver e reduzir orçamentos de textura/malha, resolução e
// qualidade baseado na quantidade de RAM em tempo de execução.
#
# Pré-requisitos:
#   - Android NDK r27+ (defina ANDROID_NDK_ROOT)
#   - Android SDK 35 (defina ANDROID_SDK_ROOT ou ANDROID_HOME)
#   - CMake 3.25+, Ninja, JDK 17+
#   - Gradle (via android/gradlew)
#
# Uso:
#   ANDROID_NDK_ROOT=/caminho/para/ndk \
#   ANDROID_SDK_ROOT=/caminho/para/sdk \
#   ./scripts/build_android_helio_g85.sh release [--install]

set -euo pipefail

BUILD_TYPE="${1:-release}"
INSTALL=false
shift || true
while [[ $# -gt 0 ]]; do
  case "$1" in
    --install) INSTALL=true; shift ;;
    *) echo "opção desconhecida: $1" >&2; exit 1 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ -z "${ANDROID_NDK_ROOT:-}" ]]; then
  echo "ERRO: defina ANDROID_NDK_ROOT antes de executar este script." >&2
  exit 1
fi
if [[ -z "${ANDROID_SDK_ROOT:-}" && -z "${ANDROID_HOME:-}" ]]; then
  echo "ERRO: defina ANDROID_SDK_ROOT (ou ANDROID_HOME) antes de executar este script." >&2
  exit 1
fi

PRESET="android-arm64-helio-g85-${BUILD_TYPE}"

echo "=== MCLA Recomp RV — Build Helio G85 / Mali-G52 ==="
echo "Projeto:  $PROJECT_ROOT"
echo "Preset:   $PRESET"
echo "NDK:      $ANDROID_NDK_ROOT"
echo "SDK:      ${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
echo

cd "$PROJECT_ROOT"

# Verificar dependências
for cmd in cmake ninja clang clang++ java gradle; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "AVISO: '$cmd' não encontrado no PATH (pode falhar no build)." >&2
  fi
done

echo "[1/4] Configurando CMake..."
cmake --preset "$PRESET"

echo "[2/4] Compilando bibliotecas nativas ARM64..."
cmake --build --preset "$PRESET" -j"$(nproc)"

echo "[3/4] Gerando APK com Gradle..."
if [[ -f "$PROJECT_ROOT/android/gradlew" ]]; then
  cd "$PROJECT_ROOT/android"
  if [[ "$BUILD_TYPE" == "release" ]]; then
    ./gradlew --no-daemon assembleRelease
    APK_SRC="app/build/outputs/apk/release/app-release.apk"
  else
    ./gradlew --no-daemon assembleDebug
    APK_SRC="app/build/outputs/apk/debug/app-debug.apk"
  fi
  cd "$PROJECT_ROOT"
else
  echo "AVISO: android/gradlew não encontrado. O .so foi compilado; empacote o APK manualmente."
  APK_SRC=""
fi

echo "[4/4] Pronto!"
if [[ -n "$APK_SRC" && -f "$PROJECT_ROOT/android/$APK_SRC" ]]; then
  mkdir -p "$PROJECT_ROOT/out"
  cp -f "$PROJECT_ROOT/android/$APK_SRC" "$PROJECT_ROOT/out/mcla-helio-g85-${BUILD_TYPE}.apk"
  echo "  APK: out/mcla-helio-g85-${BUILD_TYPE}.apk"
  if [[ "$INSTALL" == true ]]; then
    if command -v adb >/dev/null 2>&1; then
      echo "[5/5] Instalando APK no dispositivo..."
      adb install -r "$PROJECT_ROOT/out/mcla-helio-g85-${BUILD_TYPE}.apk"
    else
      echo "AVISO: adb não encontrado. Instale manualmente o APK em out/."
    fi
  fi
else
  echo "  Binário: out/build/$PRESET/libmain.so"
  echo "  (APK não gerado — Gradle não disponível ou falhou)"
fi
echo
echo "Nota: o APK precisa de assets do jogo (arquivos Xbox 360 legalmente obtidos)."
echo "      Não commite APKs, chaves de assinatura ou assets do jogo."
