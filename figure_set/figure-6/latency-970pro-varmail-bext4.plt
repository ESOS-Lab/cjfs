set term eps size 10in, 5in font "Helvetica, 55"

in1 = "barfs-970pro_new.csv"
out1 = "varmail_shared_970pro_latency_barfs_new.eps"

set xrange [10:]
set yrange [0:]
#set ytics 0.4

set xlabel "# of threads"  font "Helvetica, 60" offset 1
set ylabel "Latency(ms)"font "Helvetica, 60" offset 1
#set xtics (0 , 16 , 32,  48, 64, 80) nomirror
set xtics 10 nomirror
set ytics 10

unset key
#set border 3 back 
#set key horizon
#set key outside right font "Helvetica, 55"
#set key outside
#set key spacing 1
#set key samplen 1

set output out1
plot	in1 using 1:($2) t "create" with lp ps 5 lw 15 pt 1 lc 1,\
		in1 using 1:($3) t "unlink" with lp ps 5 lw 15 pt 2 lc 2,\
		in1 using 1:($4) t "fsync" with lp ps 5 lw 15 pt 3 lc 3,\
