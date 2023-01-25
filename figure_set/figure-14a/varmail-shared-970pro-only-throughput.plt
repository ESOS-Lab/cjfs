#set term pdfcairo size 12in, 10in font "Helvetica, 70"
set term eps size 12in, 7.5in font "Helvetica, 70"

in1 = "varmail-shared-970pro_new.csv"
out1 = "varmail_shared_only_mvcc_970pro_iops_new.eps"

set xrange [:]
set yrange [0:50]

set xlabel "# of threads"  offset 1
set ylabel "K ops/sec" offset 1
set xtics 10
set ytics 10 nomirror
set grid


#set key inside left font "Helvetica, 65" 
set key outside center top horizontal font "Helvetica, 70"
set key samplen 1

set output out1
plot in1 using 1:($2/1000) t "EXT4" with lp ps 5 lw 10 pt 1 lc 1,\
		in1 using 1:($3/1000) t "BarFS" with lp ps 5 lw 10 pt 2 lc 2,\
		in1 using 1:($4/1000) t "CJFS-V3" with lp ps 5 lw 10 pt 4 lc 4,\
		in1 using 1:($5/1000) t "CJFS-V5" with lp ps 5 lw 10 pt 8 lc 6,\
		#in1 using 1:($4/1000) t "Fast Commit" with lp ps 5 lw 10 pt 3 lc 3,\
		#in1 using 1:($7/1000) t "SpanFS" with lp ps 5 lw 10 pt 6 lc 6,\
		#in1 using 1:($4/1000) t "CJFS-V1" with lp ps 5 lw 10 pt 3 lc 3,\
