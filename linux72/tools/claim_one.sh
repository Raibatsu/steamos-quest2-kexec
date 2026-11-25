#!/bin/bash
# claim_one.sh STEP_KB PASSES  - one dry-run claim on the booted, rooted Horizon (files must already be pushed by claim_sweep.sh); prints the summary lines
adb shell "su -c 'lsmod | grep -q quest_kexec && rmmod quest_kexec; dmesg -C; STEP=$1 PASSES=$2 sh /data/local/tmp/kxc.sh 5120 dry fan'" 2>&1 | grep -E "extra claim|tried/|census|failed|error" | sed 's/^\[ *[0-9.]*\] quest_kexec: //' | cut -c1-210
