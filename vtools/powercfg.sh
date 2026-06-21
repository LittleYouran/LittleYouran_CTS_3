#!/system/bin/sh
switch_mode() {
    echo "$1" > /sdcard/Android/CTS/mode.txt
}

case $1 in
    "powersave" | "balance" | "performance" | "fast")
        switch_mode "$1" ;;
    *)
esac