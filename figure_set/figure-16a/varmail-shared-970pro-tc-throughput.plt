set term eps size 10in, 5in font "Helvetica, 60"

in1 = "varmail-shared-970pro-new.csv"
out1 = "varmail_shared_tc_mvcc_970pro_iops_new.eps"

set xrange [:]
set yrange [0:60]

set xlabel "# of threads"  offset 1
set ylabel "K ops/sec" offset 1
#set xtics (0 , 16 , 32,  48, 64, 80) nomirror
set xtics 10
set ytics 20 nomirror
set grid


#set border 3 back 
set key inside top center horizontal font "Helvetica, 60" 
set key samplen 2
#set key inside
#set key right bottom

set output out1
plot	in1 using 1:($2/1000) t "wo OC" with lp ps 5 lw 10 pt 1 lc 1,\
		in1 using 1:($3/1000) t "w OC" with lp ps 5 lw 10 pt 2 lc 2,\
