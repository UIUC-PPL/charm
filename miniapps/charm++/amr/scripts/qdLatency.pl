#!/usr/bin/perl

use strict;
use warnings;

for (<STDIN>) {
    if (/iteration (\d+), QD latency = ([0-9.]+)/) {
	#print "$1 to $2 = $3\n";
	my $latency = $2*1000*1000;
	print "$1 $latency\n";
    }
}
