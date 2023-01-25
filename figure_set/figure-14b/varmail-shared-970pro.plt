#set term pdfcairo size 12in, 9.5in font "Helvetica, 70"
set term eps size 12in, 7.5in font "Helvetica, 70"

out = "mvsp-varmail-shared-970pro-fsync-latency-cdf-new.eps"

set style line 1 lt 1 lw 15 lc rgb "dark-violet"
set style line 2 lt 2 lw 15 lc rgb "royalblue"
set style line 3 lt 3 lw 15 lc rgb "dark-red"
set style line 4 lt 4 lw 15 lc rgb "forest-green"
set style line 5 lt 5 lw 15 lc rgb "goldenrod"
set style line 6 lt 5 lw 15 lc rgb "dark-blue"

#unset key
set key outside center top horizontal font "Helvetica, 70"
set key samplen 1
#set key outside top horizon font "Helvetica, 60"
#set key samplen 1 lw 7
#set key noenhanced

set grid y
set xlabel "latency(ms)" offset 1
set xtics nomirror
set xtics 3
set ylabel "CDF (X<x)" offset 1
set ytics nomirror
set ytics 0.2
set xrange [0:14]

stat "ext4_970pro_op40_new" using 1 name "EXT4"; #nooutput;
stat "barfs_970pro_op40_new" using 1 name "BarFS" nooutput;
stat "v3_970pro_op40_new" using 1 name "MV3" nooutput;
stat "v5_970pro_op40_new" using 1 name "MV5" nooutput;
#stat "fc_wcf_970pro_op40_new" using 1 name "Fast_Commit" nooutput;
#stat "spanfs_wcf_970pro_op40_new" using 1 name "SpanFS" nooutput;

#set logscale x


set output out

plot "ext4_970pro_op40_new" using ($1/1000/1000):(column(0)/EXT4_records) with lines ls 1 t "EXT4",\
	"barfs_970pro_op40_new" using ($1/1000/1000):(column(0)/BarFS_records) with lines ls 4 t "BarFS",\
	"v3_970pro_op40_new" using ($1/1000/1000):(column(0)/MV3_records) with lines ls 3 t "CJFS-V3",\
	"v5_970pro_op40_new" using ($1/1000/1000):(column(0)/MV5_records) with lines ls 5 t "CJFS-V5",\
	#"fc_wcf_970pro_op40_new" using ($1/1000/1000):(column(0)/Fast_Commit_records) with lines ls 4 t "Fast Commit",\
	#"spanfs_wcf_970pro_op40_new" using ($1/1000/1000):(column(0)/SpanFS_records) with lines ls 5 t "SpanFS",\
