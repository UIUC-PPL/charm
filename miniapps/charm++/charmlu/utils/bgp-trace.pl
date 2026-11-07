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
print "inputs: $matrix $BLK $thresh $agglom $mapargs $peRotate\n";

`mkdir run-$runnum-$N-$BLK`;

print "Starting on $N nodes, " . ($mode eq "vn" ? $N * 4 : $N) . " processors\n";
my $dcmf = 16*1024*1024;
`qsub --env "BG_MAXALIGNEXP=1000:DCMF_RECFIFO=$dcmf" -A PARTS --mode $mode -n $N -t 55 -o run-$runnum-$N-$BLK.out -e run-$runnum-$N-$BLK.error ./lu.trace $matrix $BLK $thresh $agglom $mapargs $peRotate +gz-trace +traceroot run-$runnum-$N-$BLK +logsize 10000 +LBOff`;
