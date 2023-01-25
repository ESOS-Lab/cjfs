#set term pdfcairo size 12in, 5in font "Helvetica,50"
set term eps size 12in, 3.5in font "Helvetica,35"

in1 = "handle_970pro_varmail_shared_new.csv"
out1 = "varmail_shared_970pro_coalescing_new.eps"

set xrange [:]
set yrange [0:]

#set xlabel "# of threads" font "Helvetica,50" offset 0,0.5
#set ylabel "Handles/Tx" font "Helvetica,50" offset 1,0
set xlabel "# of threads"
set ylabel "Handles/Tx"
set xtics nomirror
set ytics 30


set grid ytics
#set border 3 back

#unset key
set key left top samplen 1

#set key outside
#set key horizon
#set key right top font "Helvetica,50"
set style data histogram
set style fill pattern border -1
set style histogram cluster gap 1

#set lmargin 7

set output out1
plot	in1 using 2:xticlabels(1) title "CJFS without OC" lw 3 lt 1 lc rgb 'black',\
		in1 using 3 title "CJFS with OC" fs pattern 3 lw 3 lt 1 lc rgb 'black'
