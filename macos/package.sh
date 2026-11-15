#!/usr/bin/env zsh
# build.sh 결과물을 Whisky 런타임 패키지(Libraries.tar.gz)로 묶는다.
#
# Libraries/
#   Wine/{bin,lib,share}   ← make install-lib 결과 + x86_64 의존 dylib + mono/gecko
#   DXVK/{x32,x64}         ← DXVK-macOS (Whisky 의 DXVK 토글이 bottle 에 복사)
#   winetricks, verbs.txt
#   WhiskyWineVersion.plist
#
# GPTK(D3DMetal)는 넣지 않는다: upstream Wine 의 winemac.drv 에 CrossOver 전용
# macdrv_functions 가 없어서 동작하지 않고, Wine 내장 d3d11/dxgi/d3d12 를 덮어써 망가뜨린다.
#
# 사용법: macos/package.sh [BUILD_ROOT]
set -euo pipefail

SRC="${0:A:h:h}"
BUILD_ROOT="${1:-${SRC:h:h}/build/wine}"
DEPS="$BUILD_ROOT/deps/wine"
OUT="$BUILD_ROOT/Libraries"

[[ -x "$BUILD_ROOT/install/bin/wine" ]] || { echo "먼저 macos/build.sh 실행"; exit 1; }

rm -rf "$OUT" "$BUILD_ROOT/Libraries.tar.gz"
mkdir -p "$OUT/Wine"
cp -a "$BUILD_ROOT/install/." "$OUT/Wine/"
rm -rf "$OUT/Wine/share/man" "$OUT/Wine/include"

# x86_64 의존 dylib (freetype, gnutls, SDL2, vulkan-loader, MoltenVK 와 그 의존성)
for f in "$DEPS"/lib/*.dylib; do cp -a "$f" "$OUT/Wine/lib/"; done
# 공유 mono / gecko (prefix 마다 설치하지 않음)
mkdir -p "$OUT/Wine/share/wine"
cp -a "$DEPS/share/wine/mono" "$DEPS/share/wine/gecko" "$OUT/Wine/share/wine/"

cp -a "$SRC/macos/DXVK" "$OUT/DXVK"
cp -a "$SRC/macos/WhiskyWineVersion.plist" "$OUT/"

curl -fsSL -o "$OUT/winetricks" https://raw.githubusercontent.com/Winetricks/winetricks/master/src/winetricks
chmod +x "$OUT/winetricks"
curl -fsSL -o "$OUT/verbs.txt" https://raw.githubusercontent.com/Winetricks/winetricks/master/files/verbs/all.txt

( cd "$BUILD_ROOT" && find Libraries -type f -exec shasum {} + > SUMS.sha && tar -czf Libraries.tar.gz Libraries )
echo "==> $BUILD_ROOT/Libraries.tar.gz ($(du -h "$BUILD_ROOT/Libraries.tar.gz" | cut -f1))"
