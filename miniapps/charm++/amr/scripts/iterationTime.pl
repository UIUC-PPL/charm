#!/usr/bin/perl

use strict;
use warnings;

for (<STDIN>) {
    if (/time for iteration (\d+) to (\d+): ([0-9.]+)/) {
	#print "$1 to $2 = $3\n";
	my $range = int($2) - int($1);
	my $avgTime = $3 / $range;
	print "$1 $avgTime\n";
    }
}

