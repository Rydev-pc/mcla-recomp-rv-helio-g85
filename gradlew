#!/usr/bin/env bash
# Gradle wrapper for MCLA Recompiled Android
# In the Termux environment, this delegates to the system gradle if available,
# otherwise downloads the official Gradle distribution.
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GRADLE_VERSION="8.4"
GRADLE_URL="https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip"
WRAPPER_DIR="$APP_DIR/.gradle/wrapper"
WRAPPER_JAR="$WRAPPER_DIR/gradle-${GRADLE_VERSION}-bin.jar"

if command -v gradle >/dev/null 2>&1; then
  exec gradle "$@"
fi

if [[ ! -f "$WRAPPER_JAR" ]]; then
  echo "Gradle not found locally. Attempting to download $GRADLE_URL..." >&2
  mkdir -p "$WRAPPER_DIR"
  if command -v curl >/dev/null 2>&1; then
    curl -L -o "$WRAPPER_DIR/gradle.zip" "$GRADLE_URL"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$WRAPPER_DIR/gradle.zip" "$GRADLE_URL"
  else
    echo "ERROR: Neither curl nor wget is available. Install gradle or download it manually." >&2
    exit 1
  fi
  (cd "$WRAPPER_DIR" && unzip -q gradle.zip && rm -f gradle.zip)
fi

# Find the extracted gradle binary
GRADLE_BIN=$(find "$WRAPPER_DIR" -name gradle -type f 2>/dev/null | head -1)
if [[ -z "$GRADLE_BIN" ]]; then
  echo "ERROR: Could not locate gradle binary in $WRAPPER_DIR" >&2
  exit 1
fi

exec "$GRADLE_BIN" --project-dir "$APP_DIR" "$@"
