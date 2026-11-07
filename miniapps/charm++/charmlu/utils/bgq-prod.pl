#!/usr/bin/perl
use strict;
use warnings;

my $mode     = "c32";
my $runnum   = "5001";
my $N        = 8;
my $matrix   = 92928;
my $BLK      = 484;
my $agglom   = 484;
my $mapargs  = "3 32 8 ";
my $peRotate = 0;
my $peStride = 8;
my $thresh   = 350;


print "Starting job $runnum on $N nodes, with SMT mode $mode\n";
#. ($mode eq "vn" ? $N * 4 : $N) . " processors\n";
print "inputs: $matrix $BLK $thresh $agglom $mapargs $peRotate $peStride\n";

`mkdir run-$runnum-$N-$BLK`;

print "Starting on $N nodes, " . ($mode eq "vn" ? $N * 4 : $N) . " processors\n";
#my $dcmf = 16*1024*1024;
`qsub -A CharmRTS --mode $mode -n $N -t 55 -o run-$runnum-$N-$BLK.out -e run-$runnum-$N-$BLK.err ./charmlu.prod $matrix $BLK $thresh $agglom $mapargs $peRotate $peStride run-$runnum-$N-$BLK +logsize 10000 +LBOff`;


