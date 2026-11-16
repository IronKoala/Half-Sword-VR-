#!/bin/bash
# Runs one unattended self-test of the build in this checkout and saves its log as selfN.log.
# Usage: selftest.sh N [extra ini lines separated by |]
N="$1"; EXTRA="$2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
D="/c/Games/Steam/steamapps/common/Half Sword/HalfSwordUE5/Binaries/Win64/ue4ss/Mods/HalfSwordVR"
T=/c/dev/hsvr-test/surgery
if tasklist 2>/dev/null | grep -qi "HalfSwordUE5"; then echo "GAME IS RUNNING - not starting a test"; exit 1; fi
cp "$ROOT"/build-mod/HalfSwordVR/Game__Shipping__Win64/main.dll "$D/dlls/main.dll" || exit 1
sed 's/^dev_fake_headset=0/dev_fake_headset=1/' $T/HalfSwordVR.ini.matthew > "$D/HalfSwordVR.ini"
printf 'dev_fake_hands=1\r\ndev_fake_still=1\r\ndev_selftest=1\r\ndev_script=25:open /Game/Maps/Arenas/Map_Arena_Cellar;170:quit\r\n' >> "$D/HalfSwordVR.ini"
if [ -n "$EXTRA" ]; then echo "$EXTRA" | tr '|' '\n' | sed 's/$/\r/' >> "$D/HalfSwordVR.ini"; fi
rm -f "$D/selftest_result.txt"
cmd //c start "" "steam://rungameid/2397300"
sleep 30
for i in $(seq 1 60); do
  sleep 5
  if ! tasklist 2>/dev/null | grep -qi "HalfSwordUE5"; then break; fi
done
if tasklist 2>/dev/null | grep -qi "HalfSwordUE5"; then echo "game still running after timeout; closing it"; taskkill //IM HalfSwordUE5-Win64-Shipping.exe //F >/dev/null 2>&1; sleep 3; fi
cp "$D/HalfSwordVR.log" $T/self$N.log
# Matthew's own settings back, whatever happened, and the last build that passed (main.safe.dll):
# the build under test only stays in the game folder if it is copied to main.safe.dll after passing.
cp $T/HalfSwordVR.ini.matthew "$D/HalfSwordVR.ini"
if [ -f $T/main.safe.dll ]; then cp $T/main.safe.dll "$D/dlls/main.dll"; fi
echo "---- result of run $N"
cat "$D/selftest_result.txt" 2>/dev/null || echo "no result file (the test did not finish)"
grep -c "" $T/self$N.log
