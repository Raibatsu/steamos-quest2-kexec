#!/bin/bash
# q2exp: staged escalation for the vrcompositor CPU freeze. Runs in Stage 2 as root.
# Start with everything suspicious OFF, then re-enable one thing per stage.
# First stage that freezes = culprit (previous stage survived). Markers go to /dev/kmsg.
P=/var/lib/q2
K() { echo "q2exp: $*" > /dev/kmsg; }
sec() { cut -d. -f1 /proc/uptime; }

# ---- save originals so everything is restored at the end
declare -A ORIG
for f in vrhz vrdirect vrasync compositor_tu; do
  if [ -e $P/$f ]; then ORIG[$f]=$(cat $P/$f); else ORIG[$f]="<absent>"; fi
done
K "originals: $(for f in "${!ORIG[@]}"; do printf '%s=%s ' $f "${ORIG[$f]}"; done)"
restore() {
  for f in "${!ORIG[@]}"; do
    if [ "${ORIG[$f]}" = "<absent>" ]; then rm -f $P/$f; else echo "${ORIG[$f]}" > $P/$f; fi
  done
  rm -f /run/q2exp_nocap
}
setf() { echo "$2" > $P/$1; }

# ---- GPU power pins
GPUDF=""
for d in /sys/class/devfreq/*; do case "$(basename $d)" in *gpu*) GPUDF=$d;; esac; done
K "gpu devfreq: ${GPUDF:-none} avail: $(cat $GPUDF/available_frequencies 2>/dev/null)"
FMIN=$(tr ' ' '\n' < $GPUDF/available_frequencies 2>/dev/null | grep -v '^$' | sort -n | head -1)
FMAX=$(tr ' ' '\n' < $GPUDF/available_frequencies 2>/dev/null | grep -v '^$' | sort -n | tail -1)
pin_devfreq() { [ -n "$GPUDF" ] || return; echo $FMIN > $GPUDF/max_freq 2>/dev/null; echo $FMIN > $GPUDF/min_freq 2>/dev/null; K "devfreq pinned to $FMIN (cur $(cat $GPUDF/cur_freq))"; }
unpin_devfreq() { [ -n "$GPUDF" ] || return; echo $FMAX > $GPUDF/max_freq 2>/dev/null; echo $FMIN > $GPUDF/min_freq 2>/dev/null; K "devfreq released ($FMIN..$FMAX)"; }
pm() { for d in 3d00000.gpu 3d6a000.gmu; do f=/sys/bus/platform/devices/$d/power/control; if [ -w $f ]; then echo $1 > $f; K "runtime pm $d control=$1 status=$(cat /sys/bus/platform/devices/$d/power/runtime_status)"; else K "runtime pm $d: no control file"; fi; done; }

# ---- CAP_SYS_NICE neutraliser (q2vrd calls setcap through PATH)
cat > /usr/local/bin/setcap <<'EOS'
#!/bin/sh
if [ -e /run/q2exp_nocap ]; then
  for a in "$@"; do f="$a"; done
  /usr/bin/setcap -r "$f" 2>/dev/null
  exit 0
fi
exec /usr/bin/setcap "$@"
EOS
chmod +x /usr/local/bin/setcap

fail() {
  K "FAIL at stage $1 ($2): $3"
  K "this is the first stage that froze; the previous stage ran clean"
  restore
  sleep 25; timeout 10 sync; K "warm-resetting back to Horizon"; sleep 1
  echo 1 > /proc/sys/kernel/sysrq
  echo b > /proc/sysrq-trigger
  exit 1
}
PAT='no IMU data|detected stalls|Watchdog detected|BUG:|Oops|Kernel panic'
cnt() { dmesg | grep -c -E "$PAT"; }

stage() { # name desc
  local name=$1 desc=$2 base
  K "stage $name BEGIN: $desc"
  base=$(cnt)
  q2vr start
  local t=0
  while [ "$(q2vr status)" != running ] && [ $t -lt 60 ]; do sleep 2; t=$((t+2)); done
  K "stage $name: SteamVR $(q2vr status) after ${t}s"
  t=0
  while [ $t -lt 50 ]; do
    sleep 2; t=$((t+2))
    if [ "$(cnt)" -gt "$base" ]; then
      fail "$name" "$desc" "after ${t}s of running"
    fi
  done
  K "stage $name PASSED (50s clean)"
  q2vr stop
  t=0; while [ "$(q2vr status)" != stopped ] && [ $t -lt 60 ]; do sleep 2; t=$((t+2)); done
  sleep 8
}

systemctl start q2-desktop q2-headtrack q2-vr 2>/dev/null
sleep 10
touch /run/q2exp_nocap

# S0: everything suspicious off
setf vrhz 45; setf vrdirect 0; setf vrasync 0; setf compositor_tu nobin
pm on; pin_devfreq
stage S0 "pm=on devfreq=min vrhz=45 vrdirect=0 async=0 tu=nobin nocap"
unpin_devfreq
stage S1 "+ devfreq free"
pm auto
stage S2 "+ runtime pm auto"
setf vrhz "${ORIG[vrhz]/<absent>/90}"
stage S3 "+ vrhz=${ORIG[vrhz]}"
setf vrdirect 1
stage S4 "+ vrdirect=1 (GPU-direct scanout)"
setf vrasync 1
stage S5 "+ compositor async"
setf compositor_tu nobin,hiprio
stage S6 "+ TU_DEBUG hiprio"
setf compositor_tu nobin,hiprio,flushall
stage S7 "+ TU_DEBUG flushall (= original tu)"
rm -f /run/q2exp_nocap
stage S8 "+ CAP_SYS_NICE (= full original config)"

K "ALL STAGES PASSED: freeze did NOT reproduce this boot"
restore
K "originals restored"
