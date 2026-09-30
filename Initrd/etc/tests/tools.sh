#!/bin/sh
# Selbsttest der Werkzeuge sort uniq diff find du hexdump df tree cal uptime (Ergebnisse als T*.TXT in /disk)
cd /disk
echo -e "c\na\nb\na" > U1.TXT
sort U1.TXT > T1.TXT
sort -r U1.TXT | uniq > T2.TXT
sort U1.TXT | uniq -c > T3.TXT
echo -e "10\n9\n100" | sort -n > T4.TXT

echo -e "eins\nzwei\ndrei" > D1.TXT
echo -e "eins\nZWEI\ndrei\nvier" > D2.TXT
diff D1.TXT D2.TXT > T5.TXT
echo "rc=$?" >> T5.TXT
diff D1.TXT D1.TXT > T6.TXT
echo "rc=$?" >> T6.TXT

mkdir FT
mkdir FT/SUB
echo x > FT/A.TXT
echo yy > FT/SUB/B.TXT
find FT -name "*.TXT" | sort > T7.TXT
find FT -type d | sort > T8.TXT
du -s -b FT > T9.TXT
echo -n ABC | hexdump > TA.TXT
df > TB.TXT
tree FT > TC.TXT
cal 2 2026 > TD.TXT
uptime > TE.TXT
