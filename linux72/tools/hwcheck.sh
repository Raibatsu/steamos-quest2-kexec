echo "##DMESG-WIFI/BT"; dmesg | grep -i -E "ath1|wlan|wcn|cnss|ath11k|bluetooth|hci_|btqca|qca" | tail -8 | cut -c1-150
echo "##BT-tools"; which hciconfig bluetoothctl btmgmt rfkill iw 2>&1 | head; ls /sys/class/bluetooth /sys/class/ieee80211 /sys/class/rfkill 2>&1 | head
echo "##SENSORS"; ls /sys/bus/iio/devices 2>&1 | head -5; for d in /sys/bus/iio/devices/iio:device*; do echo "$d $(cat $d/name 2>/dev/null)"; done 2>/dev/null | head
grep -E "^N: Name" /proc/bus/input/devices | cut -c1-70
ls /dev/syncboss* 2>&1 | tr "\n" " "; echo; dmesg | grep -i -E "prox|als|apds|vcnl|tmd|lsm6|icm|bmi|ak09" | tail -5 | cut -c1-140
echo "##BATTERY"; for p in /sys/class/power_supply/*; do echo "$p: $(cat $p/type 2>/dev/null) $(cat $p/status 2>/dev/null) cap=$(cat $p/capacity 2>/dev/null) v=$(cat $p/voltage_now 2>/dev/null) i=$(cat $p/current_now 2>/dev/null) t=$(cat $p/temp 2>/dev/null)"; done 2>&1 | head -8
dmesg | grep -i -E "battery|charger|fuel|bms|smb|qcom,fg|power_supply" | tail -5 | cut -c1-140
echo "##AUDIO-DMESG"; dmesg | grep -i -E "snd|asoc|wcd|lpass|wsa|q6|apr|audio" | tail -8 | cut -c1-140; ls /dev/snd 2>&1 | head -3
echo "##END"
