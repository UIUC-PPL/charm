#!/opt/local/bin/perl

use strict;
use warnings;

my ($output, $d1) = @ARGV;

my $out = <<OUT;
set encoding utf8
set terminal post color font "Helvetica,24"
set border 1+2+4+8 lw 1.3

#set logscale y

set style line 1 pt 1 lt 2 lc rgb "red" lw 4
set style line 2 pt 3 lt 5 lc rgb "#ff8800" lw 4
set style line 3 pt 4 lt 8 lc rgb "blue" lw 4
set style line 4 pt 6 lt 3 lc rgb "#006400" lw 4

binwidth=20
bin(x,width)=width*floor(x/width)
set boxwidth 20*0.9
plot '$output/$d1' using (bin(\$1,binwidth)):(1.0) smooth freq with boxes
OUT

open FILE, ">", "temp.gnuplot";
print FILE $out;
close FILE;


