#!/bin/sh
# Build the static aarch64 DOOM binary with the lab's cross toolchain and assemble the SD
# autorun package dist/doom-autorun.zip. Needs Docker with the mpclab-tools image (built by
# `python3 lab.py build` in virtual-mpc-sample) and curl + unzip for the Freedoom data.
set -eu
cd "$(dirname "$0")"
ENGINE=doomgeneric/doomgeneric
FREEDOOM_VER=0.13.0

# 1. engine + platform layer -> build/doom (static, glibc; the device is glibc)
mkdir -p build
SRCS=$(ls $ENGINE/*.c | grep -v -E '/doomgeneric_|/i_sdl|/i_allegro')
docker run --rm -v "$PWD":/w -w /w mpclab-tools aarch64-linux-gnu-gcc -O2 -static \
    -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DFEATURE_SOUND \
    -Wall -Wno-unused-result -Wno-unused-but-set-variable -Wno-format-truncation \
    -I$ENGINE -Isrc $SRCS src/*.c -o build/doom -lpthread -lm
ls -la build/doom

# 2. Freedoom (BSD) data, fetched once into wads/ (gitignored)
if [ ! -s wads/freedoom1.wad ]; then
    mkdir -p wads
    curl -sSL -o wads/freedoom.zip "https://github.com/freedoom/freedoom/releases/download/v$FREEDOOM_VER/freedoom-$FREEDOOM_VER.zip"
    (cd wads && unzip -o -q freedoom.zip && cp freedoom-$FREEDOOM_VER/freedoom1.wad freedoom-$FREEDOOM_VER/freedoom2.wad . && cp freedoom-$FREEDOOM_VER/COPYING.txt FREEDOOM-COPYING.txt)
fi

# 3. the card package: autorun/ (script + binary) and doom/ (WADs), plus a READ ME
rm -rf dist && mkdir -p dist/card/autorun dist/card/doom
cp autorun/autorun.json autorun/doom.sh build/doom dist/card/autorun/
cp wads/freedoom1.wad dist/card/doom/
cp wads/FREEDOOM-COPYING.txt dist/card/doom/
cp "READ ME.txt" dist/card/
python3 - <<'PY'
import hashlib
SALT = b"f7bed1c9-7e90-4731-9194-c996666b4331"
h = hashlib.sha256()
h.update(open("dist/card/autorun/autorun.json", "rb").read())
h.update(open("dist/card/autorun/doom.sh", "rb").read())
h.update(SALT)
open("dist/card/autorun/autorun.check", "w").write(h.hexdigest())
print("autorun.check", h.hexdigest())
PY
(cd dist/card && rm -f ../doom-autorun.zip && zip -qr ../doom-autorun.zip autorun doom "READ ME.txt")
ls -la dist/doom-autorun.zip
