set term eps size 11in, 7in font "Helvetica, 60"

in1 = "970pro_new.csv"
out1 = "barfs_970pro_iops_new.eps"

set xrange [:]
set yrange [0:]

set xlabel "# of threads"  offset 1
#set ylabel "Throughput (kops/s)" offset 5,-0.3
#set xtics (0 , 16 , 32,  48, 64, 80) nomirror
set xtics 10 nomirror
set ytics 30
set grid


#set border 3 back 
set key inside left top vertical  font "Helvetica, 60"
set key at 3,115
set key samplen 1
#set key maxrows 1
#set key inside
#set key right bottom
#unset key

set output out1
plot	in1 using 1:($2/1000) notitle with lp ps 5 lw 15 pt 1 lc 1,\
		in1 using 1:($3/1000) notitle with lp ps 5 lw 15 pt 2 lc 2,\
		in1 using 1:($4/1000) t "Dbench" with lp ps 5 lw 15 pt 3 lc 3,\
		in1 using 1:($5/1000) t "OLTP-Insert" with lp ps 5 lw 15 pt 6 lc 4,\
