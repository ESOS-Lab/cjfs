set term pdfcairo size 10in, 6in font "Helvetica, 55"

in1 = "970pro_new.csv"
out1 = "ext4-970pro-coalescing-new.eps"

set xrange [:]
set yrange [0:]

set xlabel "# of threads"  offset 1
set ylabel "# of handles / tx" offset 1
#set xtics (0 , 16 , 32,  48, 64, 80) nomirror
set xtics ( "10" 0, "20" 2, "30" 4, "40" 6) nomirror
set ytics 40 nomirror
set yrange [:]
set grid


#set border 3 back 
set key inside left font "Helvetica, 55" 
set style data histograms
set key samplen 2
#set key inside
#set key right bottom

set style histogram cluster gap 0.5


set output out1
plot	in1 using ($2) t "Handles/TX" lw 3 lt 1 lc rgb 'black',\
