#!/bin/sh
# Selbsttest der Shell-Skripte (wird von "make efi TESTS=script" ausgefuehrt; auch ein Beispiel fuer die Syntax).
# Aufruf: sh /etc/tests/shell.sh eins zwei drei   -> Ergebnisse in /disk/SR.TXT

ERG=/disk/SR.TXT
: > $ERG
out() { echo "$@" >> $ERG; }

x=5
if [ $x -gt 3 ]; then out gross; else out klein; fi
if test "$x" = 4; then
    out vier
elif [ $x -eq 5 ]; then
    out fuenf
fi

i=0
while [ $i -lt 3 ]; do
    i=$((i + 1))
done
out "i=$i"

for w in a b c; do out "w=$w"; done

n=0
until [ $n -ge 2 ]; do n=$((n+1)); done
out "n=$n"

summe=0
for k in 1 2 3 4; do summe=$((summe + k * 2)); done
out "summe=$summe"
out "rechnen: $(( (7 + 3) * 2 / 4 % 3 ))"

zaehle() { echo $#; }
out "args=$(zaehle a "b c" d)"

# Rekursion mit Befehlsersetzung
fak() {
    if [ $1 -le 1 ]; then
        echo 1
    else
        echo $(( $1 * $(fak $(( $1 - 1 ))) ))
    fi
}
out "fak5=$(fak 5)"

for i in 1 2 3 4 5; do
    if [ $i -eq 2 ]; then continue; fi
    if [ $i -eq 4 ]; then break; fi
    out "loop=$i"
done

ret() { return 7; }
ret
out "ret=$?"

echo fehler 2> /disk/SE.TXT >&2
gibtsnicht 2>> /disk/SE.TXT
out "status=$?"

{ echo eins; echo zwei; } > /disk/SB.TXT
out "zeilen=$(cat /disk/SB.TXT | wc)"

leer=""
out "vorgabe=${leer:-std}"
out "laenge=${#x}"
out "p=$1 $# $@"
shift
out "nach_shift=$1"

cd /disk
for f in SB*.TXT; do out "glob=$f"; done
out "quote='$x' \$x"
! false && out "negiert"
echo "a b" | while read p q; do out "read=$q"; done
out ende
