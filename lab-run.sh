#!/bin/sh
# Dev loop against the virtual-mpc-sample emulator (https://github.com/orrpunk-art/virtual-mpc-sample):
# copy the binary (and a WAD, once) to /run/doom in the guest, stop the stock app, run DOOM,
# restart the stock app when it exits. Point LAB_DIR at your virtual-mpc-sample checkout
# (default: a sibling directory of this repo).
#   ./lab-run.sh [wad]        default wad: wads/freedoom1.wad; runs in the background in the guest
#   ./lab-run.sh --log        tail DOOM's log in the guest
#   ./lab-run.sh --stop       quit DOOM (SIGTERM, a clean exit) and let the stock app come back
#   MPCDOOM_DEBUG=1 ./lab-run.sh   also log every control-surface byte and key event
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
LAB=${LAB_DIR:-$HERE/../virtual-mpc-sample}
SSH="ssh -i $LAB/out/keys/id_ed25519 -p 2022 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@127.0.0.1"
SCP="scp -i $LAB/out/keys/id_ed25519 -P 2022 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
case "${1:-}" in
  --log)  exec $SSH "tail -n 40 /run/doom/doom.log" ;;
  --stop) exec $SSH "killall -TERM doom || true" ;;
esac
WAD=${1:-$HERE/wads/freedoom1.wad}
$SSH "mkdir -p /run/doom"
$SCP "$HERE/build/doom" root@127.0.0.1:/run/doom/doom.new
$SSH "mv -f /run/doom/doom.new /run/doom/doom && chmod +x /run/doom/doom"
if ! $SSH "[ -s /run/doom/$(basename "$WAD") ]"; then $SCP "$WAD" "root@127.0.0.1:/run/doom/$(basename "$WAD")"; fi
# run detached: stop ac50, DOOM, start ac50 on any exit; log to /run/doom/doom.log
$SSH "nohup sh -c 'trap \"systemctl start ac50\" EXIT; systemctl stop ac50; cd /run/doom && MPCDOOM_DEBUG=${MPCDOOM_DEBUG:-} ./doom -iwad $(basename "$WAD")' > /run/doom/doom.log 2>&1 < /dev/null &"
echo "DOOM started in the guest; ./lab-run.sh --log for output, ./lab-run.sh --stop to quit"
