#!/opt/local/bin/perl

use strict;
use warnings;

my ($output, $name1,$run1, $keyPos, $ymax) = @ARGV;

my $out = <<OUT;
set encoding utf8
set terminal post color font "Helvetica,24"
set border 1+2+4+8 lw 1.3

set style line 1 pt 1 lt 1 lc rgb "red" lw 4
set style line 2 pt 3 lt 1 lc rgb "#ff8800" lw 4
set style line 3 pt 4 lt 1 lc rgb "blue" lw 4
set style line 4 pt 6 lt 1 lc rgb "#006400" lw 4
set style line 5 pt 8 lt 6 lc rgb "#b03060" lw 4
set style line 6 pt 2 lt 5 lc rgb "#8299AA" lw 4
set style line 7 pt 5 lt 7 lc rgb "#000000" lw 4
set style line 8 pt 9 lt 8 lc rgb "brown" lw 4

set key top $keyPos

#set logscale x
set yrange [0:$ymax]
set xrange [-50:2150]

#set xtics autofreq nomirror (16,32,64,128,256,512,1024,2048)

set xlabel 'Iteration'
set ylabel 'Iteration Time (ms)'
#set xtics autofreq nomirror (16,32,64,128,256,512,1024,2048,4096,8192,16384,32768)
#set ytics autofreq 10
#set mytics 5
#set yrange [0.1:1000]
#set xrange [8:40000]

plot '$output/$name1.128.$run1' using 1:2 with points ls 1 title "128 cores",\\
     '$output/$name1.256.$run1' using 1:2 with points ls 2 title "256 cores",\\
     '$output/$name1.512.$run1' using 1:2 with points ls 3 title "512 cores",\\
     '$output/$name1.1024.$run1' using 1:2 with points ls 4 title "1024 cores",\\
     '$output/$name1.2048.$run1' using 1:2 with points ls 5 title "2048 cores",\\
     '$output/$name1.4096.$run1' using 1:2 with points ls 6 title "4096 cores"
#'$output/$name1.32.$run1' using 1:2 with points ls 8 title "Cray XK6 32 cores",\\
#'$output/$name1.64.$run1' using 1:2 with points ls 7 title "Cray XK6 64 cores",\\
OUT

open FILE, ">", "temp.gnuplot";
print FILE $out;
close FILE;
