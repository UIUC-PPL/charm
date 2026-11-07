set terminal post color font "Helvetica,24"
set border 1+2+4+8 lw 1.3

set style line 1 pt 1 lt 2 lc rgb "red" lw 4
set style line 2 pt 3 lt 5 lc rgb "#ff8800" lw 4
set style line 3 pt 4 lt 8 lc rgb "blue" lw 4
set style line 4 pt 6 lt 3 lc rgb "#006400" lw 4


#set xlabel 'Number of Cores'
#set ylabel 'Execution Time (seconds)'
#set xtics autofreq nomirror (64,128,256,512,1024)

set yrange [0:20]

binwidth=10
bin(x,width)=width*floor(x/width)
set boxwidth 10*0.9
plot 'histoQD.128' using (bin($1,binwidth)):(1.0) smooth freq with boxes
