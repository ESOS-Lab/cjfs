#set term pdfcairo size 12in, 10in font "Helvetica, 70"
set term eps size 10in, 6in font "Helvetica, 60"

in1 = "970pro-oltp-insert.csv"
out1 = "970pro_oltp_insert.eps"

set xrange [:]
set yrange [0:4]

set xlabel "# of threads"  offset 1
#set ylabel "K ops/sec" offset 1
set xtics 10
set ytics 1 nomirror
set grid


#set key inside left font "Helvetica, 65" 
set key outside center top horizontal font "Helvetica, 60"
set key samplen 1

set output out1
plot	in1 using 1:($2/1000) notitle  with lp ps 5 lw 15 pt 1 lc 1,\
		in1 using 1:($3/1000) notitle with lp ps 5 lw 15 pt 2 lc 2,\
		in1 using 1:($4/1000) notitle with lp ps 5 lw 15 pt 4 lc 4,\
		in1 using 1:($5/1000) t "CJFS" with lp ps 5 lw 15 pt 6 lc 6,\
		in1 using 1:($6/1000) notitle with lp ps 4 lw 15 pt 8 lc 7,\
