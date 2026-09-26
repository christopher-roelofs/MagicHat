#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
mkdir -p android/deps out/android
archive=out/android/SDL2-2.32.10.tar.gz
if [[ ! -f "$archive" ]]; then
    curl -fL https://www.libsdl.org/release/SDL2-2.32.10.tar.gz -o "$archive"
fi
printf '%s  %s\n' 5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165 "$archive" | sha256sum -c -
if [[ ! -d android/deps/SDL2-2.32.10 ]]; then
    tar -xzf "$archive" -C android/deps
fi
mkdir -p android/app/src/main/assets/licenses
cp android/deps/SDL2-2.32.10/LICENSE.txt android/app/src/main/assets/licenses/SDL2.txt
# The fonts the control rail was baked from travel with the app that shows it.
cp assets/licenses/FontAwesome.txt assets/licenses/DejaVuSansMono.txt \
   android/app/src/main/assets/licenses/
./android/gradlew -p android --max-workers=4 assembleDebug
cp android/app/build/outputs/apk/debug/app-debug.apk out/android/magichat-arm64-debug.apk
printf 'APK: %s/out/android/magichat-arm64-debug.apk\n' "$root"
