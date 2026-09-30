#!/bin/sh
# Demo-Skript: sh /etc/demo.sh [name]
NAME=Welt
if_arg=$1
echo -e "\e[1;36mHallo $NAME!\e[0m  (Skript $0, $# Argument(e), erstes: $if_arg)"
ls / && echo "ls war erfolgreich"
false || echo "false hat einen Fehlerstatus, \$? = $?"
sleep 1 &
echo "Hintergrundjob gestartet"
wait
echo "fertig"
