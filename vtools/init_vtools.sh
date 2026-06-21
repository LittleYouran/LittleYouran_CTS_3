BASEDIR="$(dirname $(readlink -f "$0"))"
cp -af $BASEDIR/powercfg.sh /data/powercfg.sh
cp -af $BASEDIR/powercfg.json /data/powercfg.json
chmod 755 /data/powercfg.sh
mkdir /sdcard/Android/CTS
cur_powermode="/sdcard/Android/CTS/mode.txt"
if [ ! -f "$cur_powermode" ]; then
	touch "$cur_powermode"
	echo "powersave" > "$cur_powermode"
fi
