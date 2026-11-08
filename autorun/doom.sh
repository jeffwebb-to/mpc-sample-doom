#!/bin/sh
# DOOM on the MPC Sample - SD card autorun. Runs as root at every power-on with the card in.
# Copies the static DOOM binary to /run (tmpfs: gone at power-off, nothing on eMMC is touched),
# stops the stock sampler app cleanly, runs DOOM with a WAD read straight off the card, and
# restarts the sampler app when DOOM exits, however it exits. Card out + power-cycle = stock.
set -eu
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)       # .../autorun on the card (the path has a space on the device)
CARD=$(dirname -- "$D")
RUN=/run/doom
LOG=/run/doom.log

[ "$(id -u)" = 0 ] || exit 1
[ -s "$D/doom" ] || exit 1
grep -q ' /run tmpfs ' /proc/mounts || exit 1

# WAD: the first of these found in the card's doom/ folder (drop in your own doom.wad / doom2.wad)
WAD=
for w in doom.wad DOOM.WAD doom2.wad DOOM2.WAD doom1.wad DOOM1.WAD freedoom1.wad freedoom2.wad; do
    if [ -s "$CARD/doom/$w" ]; then WAD="$CARD/doom/$w"; break; fi
done
if [ -z "$WAD" ]; then echo "doom: no WAD in $CARD/doom" > "$LOG"; exit 0; fi

mkdir -p "$RUN"
# new inode, then rename: never overwrite a binary that might still be mapped by a running copy
cp "$D/doom" "$RUN/doom.new" && chmod 755 "$RUN/doom.new" && mv -f "$RUN/doom.new" "$RUN/doom"
if pidof doom >/dev/null 2>&1; then echo "doom: already running" >> "$LOG"; exit 0; fi

# Whatever happens from here on, the sampler app comes back.
trap 'systemctl start ac50' EXIT
systemctl stop ac50
cd "$RUN"
echo "doom: starting with $WAD" > "$LOG"
./doom -iwad "$WAD" >> "$LOG" 2>&1 || echo "doom: exit status $?" >> "$LOG"
