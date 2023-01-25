set term pdfcairo size 10in,5in font "Helvetica, 55"

out = "prob-barfs-varmail-shared-970pro-fsync-latency-cdf-new.eps"

set style line 1 lt 1 lw 4 lc rgb "dark-violet"
set style line 2 lt 2 lw 4 lc rgb "royalblue"
set style line 3 lt 3 lw 4 lc rgb "dark-red"
set style line 4 lt 4 lw 4 lc rgb "forest-green"
set style line 5 lt 5 lw 4 lc rgb "goldenrod"

unset key
set key outside
set key inside right bottom font "Helvetica, 55"
set key samplen 1
#set key noenhanced

set grid y
set xlabel "latency(ms)"
set ylabel "CDF (X<x)" offset 1,0
set xtics nomirror
#set xtics 4
set ytics nomirror
set ytics 0.2
set xrange [0:12]

stat "barfs_970pro_op10_new" using 1 name "threads10" nooutput;
stat "barfs_970pro_op20_new" using 1 name "threads20" nooutput;
stat "barfs_970pro_op30_new" using 1 name "threads30" nooutput;
stat "barfs_970pro_op40_new" using 1 name "threads40" nooutput;

#set logscale x

set output out

plot "barfs_970pro_op10_new" using ($1/1000/1000):(column(0)/threads10_records) with lines ls 1 t "thr 10",\
      "barfs_970pro_op20_new" using ($1/1000/1000):(column(0)/threads20_records) with lines ls 2 t "thr 20",\
      "barfs_970pro_op30_new" using ($1/1000/1000):(column(0)/threads30_records) with lines ls 3 t "thr 30",\
      "barfs_970pro_op40_new" using ($1/1000/1000):(column(0)/threads40_records) with lines ls 4 t "thr 40",\
