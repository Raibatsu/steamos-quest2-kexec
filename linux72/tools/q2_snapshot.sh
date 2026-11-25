#!/bin/sh
# q2_snapshot.sh - capture what the kernel and userspace look like, to compare Horizon (Quest OS 4.19) with our Linux 7.2.
# Runs as root on EITHER system (POSIX sh, only reads; the only writes are the ones named below under 'trace' and they are undone at the end).
#   q2_snapshot.sh snapshot <outdir>              static state: dmesg, logcat, props, processes, irqs, clocks, regulators, gpios, pinctrl, power supplies, remoteprocs,
#                                                 ALSA/mixer state, PMIC regmaps, syncboss sysfs, loaded modules, device nodes
#   q2_snapshot.sh dyndbg <outdir> <pattern>...   switch on kernel dynamic debug for the given 'file X'/'module X' patterns (Horizon has Meta's pr_debug in
#                                                 techpack/audio and syncboss), then `dyndbg-off` restores them
#   q2_snapshot.sh audio <outdir> [secs]          dynamic-debug the audio stack (q6afe/q6adm/q6asm/q6core, machine driver, cm710x, ak4331), play a short test
#                                                 tone if tinyplay/aplay exist, capture the log, clear the debug flags again
#   q2_snapshot.sh ctl <outdir> [secs]            watch the controller/tracking stack for [secs]: processes with /dev/syncboss* open, their syscalls (strace if
#                                                 present, else ftrace syscall events by pid), logcat for tracking/syncboss tags, syncboss dynamic debug
#   q2_snapshot.sh trace <outdir> <secs> <regex>   strace (pushed to /data/local/tmp/strace) every process whose name/cmdline matches the regex, all ioctl/read/write data
#   q2_snapshot.sh dyndbg-off                     clear every flag set by this script
# Nothing here writes to any partition or device register. Output stays on the device until the host pulls it (see horizon_probe.sh); never commit it.
OUT=${2:-/data/local/tmp/q2snap}
mode=$1
mkdir -p "$OUT" 2>/dev/null
have() { command -v "$1" >/dev/null 2>&1; }
[ -x /data/local/tmp/strace ] && PATH=$PATH:/data/local/tmp
cap() { name=$1; shift; { "$@"; } > "$OUT/$name" 2>&1; }       # cap <file> <command...>
capsh() { name=$1; shift; sh -c "$*" > "$OUT/$name" 2>&1; }      # capsh <file> '<shell string>'
DBG=/sys/kernel/debug
[ -d $DBG/tracing ] || mount -t debugfs none $DBG 2>/dev/null
TR=$DBG/tracing; [ -d $TR ] || TR=/sys/kernel/tracing   # Horizon has tracefs but no debugfs

snapshot() {
  cap uname uname -a
  capsh dmesg 'dmesg'
  capsh logcat 'have() { command -v $1 >/dev/null 2>&1; }; have logcat && logcat -d -b all 2>&1 | tail -n 20000'
  capsh props 'getprop 2>/dev/null'
  capsh ps 'ps -A -o PID,PPID,USER,NAME,ARGS 2>/dev/null || ps -ef'
  capsh interrupts 'cat /proc/interrupts'
  capsh iomem 'cat /proc/iomem'
  capsh modules 'cat /proc/modules'
  capsh mounts 'cat /proc/mounts'
  capsh devices 'ls -la /dev /dev/snd /dev/input /dev/block/by-name 2>/dev/null'
  capsh clk_summary "cat $DBG/clk/clk_summary"
  capsh regulator_summary "cat $DBG/regulator/regulator_summary"
  capsh gpio "cat $DBG/gpio"
  capsh pinctrl "for f in $DBG/pinctrl/*/pinmux-pins $DBG/pinctrl/*/pins; do echo == \$f; cat \$f; done 2>/dev/null"
  capsh power_supply 'for d in /sys/class/power_supply/*; do echo == $d; for f in $d/*; do [ -f $f ] && echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; done'
  capsh remoteproc 'for d in /sys/class/remoteproc/*; do echo == $d $(cat $d/name) $(cat $d/state); done; ls /sys/bus/msm_subsys/devices 2>/dev/null'
  capsh alsa 'cat /proc/asound/cards /proc/asound/devices /proc/asound/pcm 2>&1; ls /proc/asound; have() { command -v $1 >/dev/null 2>&1; }; have tinymix && tinymix 2>&1 | head -2000; have amixer && amixer contents 2>&1 | head -2000'
  capsh asoc "ls $DBG/asoc 2>/dev/null; for f in $DBG/asoc/*/dapm/* $DBG/asoc/dais $DBG/asoc/components $DBG/asoc/platforms; do [ -f \$f ] && { echo == \$f; cat \$f; }; done 2>/dev/null | head -3000"
  capsh regmap_list "ls $DBG/regmap"
  capsh regmap_pmic "for d in $DBG/regmap/*-0[0-9]; do echo == \$d; cat \$d/registers 2>/dev/null; done"
  capsh syncboss_sysfs 'for d in /sys/devices/virtual/misc/syncboss* /sys/class/syncboss*; do [ -e $d ] && { echo == $d; find $d -maxdepth 3 -type f 2>/dev/null | while read f; do echo "$f=$(head -c 200 $f 2>/dev/null | tr "\n" " ")"; done; }; done'
  capsh syncboss_users 'for p in /proc/[0-9]*; do ls -l $p/fd 2>/dev/null | grep -q "syncboss" && echo "$(basename $p) $(cat $p/comm) $(tr "\0" " " < $p/cmdline | head -c 200)"; done'
  capsh input 'cat /proc/bus/input/devices'
  capsh dt_sound 'find /proc/device-tree -path "*sound*" -name "qcom,*" 2>/dev/null | head -50'
  capsh fw_files 'ls -la /vendor/firmware /vendor/firmware_mnt/image /lib/firmware /odm/firmware 2>/dev/null | head -300'
  capsh lsof_audio 'for p in /proc/[0-9]*; do ls -l $p/fd 2>/dev/null | grep -q "snd\|adsprpc\|audio" && echo "$(basename $p) $(cat $p/comm)"; done'
}

dd_set() {   # dd_set <pattern>: turn on one dynamic-debug pattern, remember it
  [ -w $DBG/dynamic_debug/control ] || { echo "no dynamic_debug control (CONFIG_DYNAMIC_DEBUG / debugfs?)" >&2; return 1; }
  echo "$1 +pflmt" > $DBG/dynamic_debug/control 2>/dev/null && echo "$1" >> /data/local/tmp/q2snap.dyndbg 2>/dev/null || echo "$1" >> /tmp/q2snap.dyndbg
}
dd_off() {
  for f in /data/local/tmp/q2snap.dyndbg /tmp/q2snap.dyndbg; do
    [ -f $f ] && while read p; do echo "$p -p" > $DBG/dynamic_debug/control 2>/dev/null; done < $f && rm -f $f
  done
}

audio() {
  secs=${1:-8}
  cap dmesg_before dmesg
  for p in 'file q6afe.c' 'file q6adm.c' 'file q6asm.c' 'file q6core.c' 'file msm-pcm-routing-v2.c' 'file kona.c' 'file cm710x.c' 'file ak4331.c' \
           'file audio-ext-clk-up.c' 'file msm-dai-q6-v2.c' 'file q6common.c' 'file adsp_loader.c' 'module cm710x' 'module ak4331'; do dd_set "$p"; done
  capsh controls_before 'have() { command -v $1 >/dev/null 2>&1; }; have tinymix && tinymix 2>&1 | head -2000'
  # the host wrapper pushes tone.wav (1 kHz, 48 kHz S16 stereo, 3 s); without it, make the headset play any sound while this runs
  PP=''
  if [ -f "$OUT/tone.wav" ]; then
    if have tinyplay; then tinyplay "$OUT/tone.wav" -D 0 -d 0 >"$OUT/play.log" 2>&1 & PP=$!
    elif have aplay; then aplay "$OUT/tone.wav" >"$OUT/play.log" 2>&1 & PP=$!
    else echo "no tinyplay/aplay: make the headset play any sound now, capturing for $secs s" > "$OUT/play.log"; fi
  else echo "no tone.wav: make the headset play any sound now, capturing for $secs s" > "$OUT/play.log"; fi
  sleep "$secs"; [ -n "$PP" ] && kill $PP 2>/dev/null
  cap dmesg_after dmesg
  capsh controls_after 'have() { command -v $1 >/dev/null 2>&1; }; have tinymix && tinymix 2>&1 | head -2000'
  diff "$OUT/dmesg_before" "$OUT/dmesg_after" 2>/dev/null | grep '^>' | sed 's/^> //' > "$OUT/dmesg_new"
  dd_off
  echo "audio trace: $OUT/dmesg_new ($(wc -l < "$OUT/dmesg_new" 2>/dev/null) new lines)"
}

ctl() {
  secs=${1:-20}
  pids=''
  for p in /proc/[0-9]*; do ls -l $p/fd 2>/dev/null | grep -q syncboss && pids="$pids $(basename $p)"; done
  echo "syncboss users: $pids" > "$OUT/ctl_pids"
  for p in 'module syncboss' 'file syncboss_spi.c' 'file syncboss_stream.c' 'file syncboss_camera.c'; do dd_set "$p"; done
  cap dmesg_before dmesg
  have logcat && logcat -c 2>/dev/null
  LC=""
  have logcat && { logcat -b all -v threadtime > "$OUT/logcat_ctl" 2>&1 & LC=$!; }
  SP=''
  for pid in $pids; do
    if have strace; then strace -f -tt -s 96 -xx -e trace=openat,ioctl,read,write,pread64,pwrite64,sendmsg,recvmsg -p $pid -o "$OUT/strace_$pid" 2>/dev/null & SP="$SP $!"
    elif [ -d $TR ]; then
      echo $pid >> $TR/set_event_pid 2>/dev/null
      for e in sys_enter_ioctl sys_enter_write sys_enter_read sys_enter_openat; do echo 1 > $TR/events/syscalls/$e/enable 2>/dev/null; done
      echo 1 > $TR/tracing_on 2>/dev/null
    fi
  done
  sleep "$secs"
  [ -n "$SP" ] && kill $SP 2>/dev/null
  [ -n "$LC" ] && kill $LC 2>/dev/null
  if [ -d $TR ] && ! have strace; then
    cat $TR/trace > "$OUT/ftrace_syscalls" 2>/dev/null
    echo 0 > $TR/tracing_on 2>/dev/null
    for e in sys_enter_ioctl sys_enter_write sys_enter_read sys_enter_openat; do echo 0 > $TR/events/syscalls/$e/enable 2>/dev/null; done
    : > $TR/set_event_pid 2>/dev/null; : > $TR/trace 2>/dev/null
  fi
  cap dmesg_after dmesg
  diff "$OUT/dmesg_before" "$OUT/dmesg_after" 2>/dev/null | grep '^>' | sed 's/^> //' > "$OUT/dmesg_new"
  dd_off
  echo "ctl trace: $OUT ($(ls "$OUT" | wc -l) files)"
}

# trace <secs> <name-regex> : strace every process whose comm/cmdline matches, with generous string sizes (ioctl/read/write data included)
trace() {
  secs=${1:-20}; re=$(echo "${2:-trackingservice}" | tr "," "|")
  have strace || { echo "no strace on this system (push tools/strace-aarch64 to /data/local/tmp/strace)" >&2; return 1; }
  SP=''
  for p in /proc/[0-9]*; do
    pid=$(basename $p); n="$(cat $p/comm 2>/dev/null) $(tr '\0' ' ' < $p/cmdline 2>/dev/null)"
    case "$n" in *strace*|*q2_snapshot*) continue ;; esac
    echo "$n" | grep -Eq "$re" || continue
    echo "$pid $n" >> "$OUT/trace_pids"
    strace -f -ttt -s 512 -xx -e trace=openat,ioctl,read,write,pread64,pwrite64,sendmsg,recvmsg,writev,readv -p $pid -o "$OUT/strace_$pid" 2>"$OUT/strace_$pid.err" & SP="$SP $!"
  done
  [ -n "$SP" ] || { echo "no process matching '$re'" >&2; return 1; }
  sleep "$secs"; kill $SP 2>/dev/null; sleep 1
  capsh controls_after 'have() { command -v $1 >/dev/null 2>&1; }; have tinymix && tinymix 2>&1'
  echo "trace: $(wc -l < "$OUT/trace_pids") processes, $(du -sh "$OUT" | cut -f1)"
}

# trace-start <regex> : like trace, but returns at once; the straces keep running until trace-stop (no timer: the human decides when the action is done)
trace_start() {
  re=$(echo "${1:-trackingservice}" | tr "," "|")
  have strace || { echo "no strace on this system" >&2; return 1; }
  : > "$OUT/trace_pids"; : > "$OUT/strace.pids"
  capsh controls_before 'have() { command -v $1 >/dev/null 2>&1; }; have tinymix && tinymix 2>&1'
  capsh power_before 'for d in /sys/class/power_supply/*; do echo == $d; for f in $d/*; do [ -f $f ] && echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; done'
  for p in /proc/[0-9]*; do
    pid=$(basename $p); n="$(cat $p/comm 2>/dev/null) $(tr '\0' ' ' < $p/cmdline 2>/dev/null)"
    case "$n" in *strace*|*q2_snapshot*) continue ;; esac
    echo "$n" | grep -Eq "$re" || continue
    echo "$pid $n" >> "$OUT/trace_pids"
    nohup strace -f -ttt -s 512 -xx -e trace=openat,ioctl,read,write,pread64,pwrite64,sendmsg,recvmsg,writev,readv -p $pid -o "$OUT/strace_$pid" >/dev/null 2>"$OUT/strace_$pid.err" &
    echo $! >> "$OUT/strace.pids"
  done
  echo "tracing $(wc -l < "$OUT/trace_pids") processes"
}
trace_stop() {
  [ -f "$OUT/strace.pids" ] && kill $(cat "$OUT/strace.pids") 2>/dev/null; sleep 1
  capsh controls_after 'have() { command -v $1 >/dev/null 2>&1; }; have tinymix && tinymix 2>&1'
  capsh power_after 'for d in /sys/class/power_supply/*; do echo == $d; for f in $d/*; do [ -f $f ] && echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; done'
  echo "stopped; $(du -sh "$OUT" | cut -f1)"
}

case "$mode" in
  snapshot) snapshot ;;
  dyndbg) shift 2; for p in "$@"; do dd_set "$p"; done ;;
  dyndbg-off) dd_off ;;
  audio) audio "$3" ;;
  ctl) ctl "$3" ;;
  trace) trace "$3" "$4" ;;
  trace-start) trace_start "$3" ;;
  trace-stop) trace_stop ;;
  *) sed -n 2,17p "$0"; exit 1 ;;
esac
echo "done: $OUT"
