#!/bin/bash
# ============================================================
#  setup_serial.sh  (旧 set.sh)
#  役割     : Arduino (/dev/ttyACM0) への読み書き権限を一時的に開放する。
#  実行場所 : ホスト
#  実行順序 : docker_enter.sh の前。USB 再接続のたびに必要な一時しのぎ。
#
#  NOTE: 恒久対策は setup_udev.sh による dialout グループ追加 +
#        再ログイン。グループが正しく効いていれば本スクリプトは不要。
#        互換のため残している。
# ============================================================
sudo chmod 777 /dev/ttyACM0
sudo chmod 777 /dev/video/pir_camera
