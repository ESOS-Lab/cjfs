set term pdfcairo size 10in, 5in font "Helvetica, 55"

in1 = "barfs-970pro_new.csv"
out1 = "barfs-970pro-locked-latency-new.eps"

set xlabel "# of threads"  offset 1
set ylabel "Latency(ms)" offset 1

#set xtics (0 , 16 , 32,  48, 64, 80) nomirror
set xtics ( "10" 0, "20" 2, "30" 4, "40" 6) nomirror
set ytics 2 nomirror
set yrange [:8]
set grid

#unset key
#set border 3 back 
set key inside horizon font "Helvetica, 55" 
set style data histograms
set key samplen 2
#set key inside
#set key right bottom

set style histogram cluster gap 0.5


set output out1
plot	in1 using ($2/1000) t "Locked" fs pattern 0 lw 3 lt 1 lc rgb 'black',\
	in1 using ($3/1000) t "Total" fs pattern 3 lw 3 lt 1 lc rgb 'black',\
