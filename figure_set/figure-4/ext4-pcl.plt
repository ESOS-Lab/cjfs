set term eps size 12in, 5in font "Helvetica, 50"

in1 = "970pro_new.csv"
out1 = "ext4-page-conflict-new.eps"

set xrange [:]
set yrange [0:24]

set xlabel "# of threads"
set ylabel "Conflicts/Tx" offset 1

#set xtics (0 , 16 , 32,  48, 64, 80) nomirror
set xtics ( "10" 0, "20" 2, "30" 4, "40" 6) nomirror
set ytics 8 nomirror
set yrange [:]
set grid


#set border 3 back 
set key center outside top horizontal font "Helvetica, 50" 
#unset key
set style data histograms
#set key samplen 1
#set key inside
#set key right bottom

set style histogram cluster gap 1


set output out1
plot	in1 using ($2) t "EXT4" fs pattern 0 lw 3 lt 1 lc rgb 'black',\
	in1 using ($3) t "BarrierFS" fs pattern 3 lw 3 lt 1 lc rgb 'black',\
	#in1 using ($4) t "Fast Commit" fs pattern 3 lw 3 lt 1 lc rgb 'red',\
	#in1 using ($5) t "SpanFS" fs pattern 3 lw 3 lt 1 lc rgb 'magenta',\
