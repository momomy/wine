#!/usr/bin/env zsh
# 애플 Game Porting Toolkit(D3DMetal) 재배포본을 받아 package.sh 가 쓰는 배치로 추린다.
# 출처: Gcenx/game-porting-toolkit 릴리스(애플 GPTK 의 바이너리 배포). 그 번들의 Wine(7.7 기반)은 쓰지 않고
# D3DMetal 관련 파일만 꺼낸다.
#   <OUT>/lib/external/{D3DMetal.framework,libd3dshared.dylib}
#   <OUT>/lib/wine/x86_64-windows/<이름>.dll
#   <OUT>/lib/wine/x86_64-unix/<이름>.so   (→ ../../external/libd3dshared.dylib 심볼릭 링크)
#
# 사용법: macos/fetch-gptk.sh [BUILD_ROOT]     (GPTK_VERSION 기본 3.0-3) → <BUILD_ROOT>/../gptk3/redist
set -euo pipefail

SRC="${0:A:h:h}"
BUILD_ROOT="${1:-${SRC:h:h}/build/winecx}"
VER="${GPTK_VERSION:-3.0-3}"
DIR="${BUILD_ROOT:h}/gptk3"
URL="https://github.com/Gcenx/game-porting-toolkit/releases/download/Game-Porting-Toolkit-$VER/game-porting-toolkit-$VER.tar.xz"
NAMES=(d3d10 d3d11 d3d12 dxgi atidxx64 nvapi64 nvngx nvngx-on-metalfx)

mkdir -p "$DIR/dl" "$DIR/x"
TAR="$DIR/dl/game-porting-toolkit-$VER.tar.xz"
[[ -f "$TAR" ]] || curl -fL -o "$TAR" "$URL"
[[ -d "$DIR/x/Game Porting Toolkit.app" ]] || tar -xJf "$TAR" -C "$DIR/x"

W="$DIR/x/Game Porting Toolkit.app/Contents/Resources/wine/lib"
OUT="$DIR/redist"
rm -rf "$OUT"
mkdir -p "$OUT/lib/wine/x86_64-unix" "$OUT/lib/wine/x86_64-windows"
cp -a "$W/external" "$OUT/lib/"
for n in $NAMES; do
  cp -a "$W/wine/x86_64-unix/$n.so" "$OUT/lib/wine/x86_64-unix/"
  cp -a "$W/wine/x86_64-windows/$n.dll" "$OUT/lib/wine/x86_64-windows/"
done
cp "$W/external/D3DMetal.framework/Versions/A/Resources/LICENSE" "$OUT/LICENSE" 2>/dev/null || true
echo "==> $OUT (D3DMetal $(plutil -extract CFBundleShortVersionString raw "$W/external/D3DMetal.framework/Resources/Info.plist"))"
