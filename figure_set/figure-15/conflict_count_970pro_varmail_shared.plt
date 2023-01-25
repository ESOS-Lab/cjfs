#set term eps size 12in, 5in font "Helvetica,50"
set term eps size 12in, 3.5in font "Helvetica,35"

in1 = "conflict_count_970pro_varmail_shared_new.csv"
out1 = "varmail_shared_970pro_conflict_count_new.eps"

set xrange [:]
set yrange [0:30]

#set xlabel "# of threads" font "Helvetica,40" offset 0,0.5
set ylabel "Conflicts / Tx" font "Helvetica,40" offset 1,0
set xlabel "# of threads"
#set ylabel "Conflicts / Tx"
set xtics nomirror
set ytics 10
#set ytics 6


set grid ytics
#set border 3 back

#unset key
set key center horizontal top samplen 1

#set key outside
#set key horizon
#set key right top font "Helvetica,50"
set style data histogram
set style fill pattern border -1
set style histogram cluster gap 1

set lmargin 7

set output out1
plot	in1 using 2:xticlabels(1) title "EXT4" lw 3 lt 1 lc rgb 'black',\
		in1 using 3 title "BarFS" fs pattern 4 lw 3 lt 1 lc rgb 'black',\
		in1 using 4 title "CJFS-V3" fs pattern 6 lw 3 lt 1 lc rgb 'black',\
		in1 using 5 title "CJFS-V5" fs pattern 6 lw 3 lt 1 lc rgb 'black',\
		#in1 using 6 title "SpanFS" fs pattern 7 lw 3 lt 1 lc rgb 'black',\
