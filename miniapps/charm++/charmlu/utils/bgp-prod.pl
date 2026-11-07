#!/usr/bin/perl

use strict;
use warnings;

my $mode     = "vn";
my $runnum   = "321";
my $N        = 512;
my $matrix   = 96000;
my $BLK      = 150;
my $agglom   = 150;
my $mapargs  = "3 256 16";
my $peRotate = 16;
my $peStride = 2;
my $thresh   = 310;


print "Starting job $runnum on $N nodes, " . ($mode eq "vn" ? $N * 4 : $N) . " processors\n";
print "inputs: $matrix $BLK $thresh $agglom $mapargs $peRotate $peStride\n";

my $dcmf = 16*1024*1024;
`qsub --env "BG_MAXALIGNEXP=1000:DCMF_RECFIFO=$dcmf" -A PARTS --mode $mode -n $N -t 35 -o run-$runnum-$N-$BLK.out -e run-$runnum-$N-$BLK.error ./lu.prod $matrix $BLK $thresh $agglom $mapargs $peRotate $peStride +LBOff`;
