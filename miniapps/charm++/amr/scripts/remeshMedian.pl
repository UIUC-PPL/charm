#!/opt/local/bin/perl

use strict;
use warnings;

my ($output, $d1,$d2,$d3, $machine, $ymax) = @ARGV;

my $xaxis = "";

if ($machine eq "titan") {
$xaxis = <<OUT
set xrange [20:4000]
set xtics autofreq nomirror (32,64,128,256,512,"1k" 1024, "2k" 2048)
OUT
} else {
$xaxis = <<OUT
set xrange [40:48000]
set xtics autofreq nomirror (64,128,256,512,"1k" 1024, "2k" 2048, "4k" 4096, "8k" 8096, "16k" 16384, "32k" 32768)
OUT
}

my $out = <<OUT;
set encoding utf8
set terminal post color font "Helvetica,24"
set border 1+2+4+8 lw 1.3

set style line 1 pt 1 lt 1 lc rgb "red" lw 4
set style line 2 pt 3 lt 4 lc rgb "#ff8800" lw 4
set style line 3 pt 4 lt 1 lc rgb "blue" lw 4
set style line 4 pt 6 lt 1 lc rgb "#006400" lw 4
#set style line 4 pt 6 lt 4 lc rgb "#ff8800" lw 4
set style line 5 pt 8 lt 6 lc rgb "#b03060" lw 4
set style line 6 pt 2 lt 5 lc rgb "#8299AA" lw 4


set key top right

set logscale x
set yrange [0:$ymax]

$xaxis

set ylabel "Median Remeshing Latency (µs)"
set xlabel "Number of Cores"

set bars 4.0
set style fill empty
plot '$output/$d1' using 1:10 with linespoints ls 1 lw 2 title "Depth Range 4-9",\\
     '$output/$d2' using 1:10 with linespoints ls 4 lw 2 title "Depth Range 4-10",\\
     '$output/$d3' using 1:10 with linespoints ls 3 lw 2 title "Depth Range 4-11",\\
     '$output/$d1' using 1:11 with linespoints ls 2 lw 2 title "RM-TD w/Depths 4-9",\\
     '$output/$d2' using 1:11 with linespoints ls 5 lw 2 title "RM-TD w/Depths 4-10",\\
     '$output/$d3' using 1:11 with linespoints ls 6 lw 2 title "RM-TD w/Depths 4-11"
OUT

open FILE, ">", "temp.gnuplot";
print FILE $out;
close FILE;
