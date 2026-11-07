#!/opt/local/bin/perl

use strict;
use warnings;

my ($output, $d1,$d2,$d3, $machine) = @ARGV;

my $xaxis = "";

if ($machine eq "titan") {
$xaxis = <<OUT
set xrange [10:4000]
set xtics autofreq nomirror (16,32,64,128,256,512,"1k" 1024, "2k" 2048)
OUT
} else {
$xaxis = <<OUT
set xrange [10:48000]
set xtics autofreq nomirror (16,32,64,128,256,512,"1k" 1024, "2k" 2048, "4k" 4096, "8k" 8096, "16k" 16384, "32k" 32768)
OUT
}

my $out = <<OUT;
set encoding utf8
set terminal post color font "Helvetica,24"
set border 1+2+4+8 lw 1.3

set style line 1 pt 1 lt 1 lc rgb "red" lw 4
set style line 2 pt 3 lt 1 lc rgb "#ff8800" lw 4
set style line 3 pt 4 lt 1 lc rgb "blue" lw 4
set style line 4 pt 6 lt 1 lc rgb "#006400" lw 4

set key top right

set logscale x
#set yrange [-100:4000]

$xaxis

set ylabel "Remeshing Latency Time (ms)"
set xlabel "Number of Cores"

set bars 4.0
set style fill empty
plot '$output/$d1' using 9:5:4:8:7 with candlesticks ls 1 lw 2 fs pattern 0 title "Depth Range 4-9" whiskerbars,\\
     '$output/$d1' using 9:6:6:6:6 with candlesticks ls 1 lw 2 lt -1 notitle,\\
     '$output/$d2' using 9:5:4:8:7 with candlesticks ls 4 lw 2 fs pattern 4 title "Depth Range 4-10" whiskerbars,\\
     '$output/$d2' using 9:6:6:6:6 with candlesticks ls 4 lw 2 lt -1 notitle,\\
     '$output/$d3' using 9:5:4:8:7 with candlesticks ls 3 lw 2 fs pattern 5 title "Depth Range 4-11" whiskerbars,\\
     '$output/$d3' using 9:6:6:6:6 with candlesticks ls 3 lw 2 lt -1 notitle
OUT

open FILE, ">", "temp.gnuplot";
print FILE $out;
close FILE;
