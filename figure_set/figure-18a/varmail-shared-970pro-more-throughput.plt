set term eps size 10in,6in font "Helvetica, 60"

in1 = "varmail-shared-970pro-new.csv"
out1 = "varmail_shared_more_mvcc_970pro_iops-new.eps"

set xrange [:]
set yrange [0:50]

set xlabel "# of threads"  offset 1
set ylabel "K ops/sec" offset 1
#set xtics (0 , 16 , 32,  48, 64, 80) nomirror
set xtics 10 nomirror
set ytics 25 nomirror
set grid

set key samplen 1
set key outside top center horizontal font "Helvetica, 60" 

set output out1
plot in1 using 1:($2/1000) t "EXT4" with lp ps 5 lw 15 pt 1 lc 1,\
		in1 using 1:($3/1000) notitle with lp ps 5 lw 15 pt 2 lc 2,\
		in1 using 1:($4/1000) notitle with lp ps 5 lw 15 pt 4 lc 4,\
		in1 using 1:($5/1000) notitle with lp ps 5 lw 15 pt 6 lc 6,\
		in1 using 1:($6/1000) notitle with lp ps 5 lw 15 pt 8 lc 7,\
