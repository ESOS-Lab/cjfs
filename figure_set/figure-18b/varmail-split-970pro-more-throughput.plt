set term eps size 10in, 6in font "Helvetica, 60"

in1 = "varmail-split-970pro-new.csv"
out1 = "varmail_split_more_mvcc_970pro_iops_new.eps"

set xrange [:]
set yrange [0:40]

set xlabel "# of threads"  offset 1
#set ylabel "K ops/sec" offset 1
set xtics 10
set ytics 10 nomirror
set grid

set key outside top center horizontal font "Helvetica, 60" 
set key samplen 1

set output out1
plot	in1 using 1:($2/1000) notitle with lp ps 5 lw 15 pt 1 lc 1,\
		in1 using 1:($3/1000) t "BarFS" with lp ps 5 lw 15 pt 2 lc 2,\
		in1 using 1:($4/1000) notitle with lp ps 5 lw 15 pt 4 lc 4,\
		in1 using 1:($5/1000) notitle with lp ps 5 lw 15 pt 6 lc 6,\
		in1 using 1:($6/1000) notitle with lp ps 5 lw 15 pt 8 lc 7,\
