set term eps size 10in, 6.5in font "Helvetica, 60"

in1 = "970pro_new.csv"
out1 = "970pro_iops_new.eps"

set xrange [:]
set yrange [0:240]

set xlabel "# of threads"  offset 1
set ylabel "Throughput (kops/s)" offset 1
#set xtics (0 , 16 , 32,  48, 64, 80) nomirror
set xtics 10 nomirror
set ytics 40
set grid


#set border 3 back 
set key inside top left font "Helvetica, 60" 
#set key at -1,100
set key samplen 1
#set key maxrows 1
#set key inside
#set key right bottom

set output out1
plot	in1 using 1:($2/1000) t "Varmail-shared" with lp ps 3 lw 7 pt 1 lc 1,\
		in1 using 1:($3/1000) t "Varmail-split" with lp ps 3 lw 7 pt 2 lc 2,\
		in1 using 1:($4/1000) t "Dbench" with lp ps 3 lw 7 pt 3 lc 3,\
		in1 using 1:($5/1000) t "MDtest" with lp ps 3 lw 7 pt 4 lc 4,\
