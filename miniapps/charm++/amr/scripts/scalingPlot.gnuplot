set terminal post color font "Helvetica,24"
set border 1+2+4+8 lw 1.3

set pointsize 1

set style line 1 pt 1 lt 2 lc rgb "red" lw 4
set style line 2 pt 3 lt 5 lc rgb "#ff8800" lw 4
set style line 3 pt 4 lt 8 lc rgb "blue" lw 4
set style line 4 pt 6 lt 3 lc rgb "#006400" lw 4
# set style line 4 pt 6 lt 4 lc rgb "#ff8800" lw 4
# set style line 5 pt 8 lt 6 lc rgb "#b03060" lw 4
# set style line 6 pt 2 lt 5 lc rgb "#8299AA" lw 4
# set style line 7 pt 5 lt 7 lc rgb "#000000" lw 4
# set style line 8 pt 9 lt 8 lc rgb "brown" lw 4
set style line 10 pt 0 lw 2 lc rgb "black"

set key top right
#set grid mytics ytics

set logscale xy

set xlabel 'Number of Cores'
set ylabel 'Execution Time (seconds)'
#set xtics autofreq nomirror (64,128,256,512,1024)
set ytics autofreq 10
set mytics 5
set yrange [1:500]
set xrange [32:33000]
plot 'scaling_data8' using 1:2 with linespoints ls 1 title "depth8",\
     'scaling_data8' using 1:3 with linespoints ls 10 title "",\
     'scaling_data9' using 1:2 with linespoints ls 2 title "depth9",\
     'scaling_data9' using 1:3 with linespoints ls 10 title "",\
     'scaling_data10' using 1:2 with linespoints ls 3 title "depth10",\
     'scaling_data10' using 1:3 with linespoints ls 10 title "",\
     'scaling_data11' using 1:2 with linespoints ls 4 title "depth11",\
     'scaling_data11' using 1:3 with linespoints ls 10 title ""
