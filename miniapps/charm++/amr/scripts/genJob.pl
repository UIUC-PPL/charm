#!/usr/bin/perl

use strict;
use warnings;

die "usage: ./genJob.pl <runNum> <nodes> <ppn> <minutes> <depth> <blocksize> <iterations>" if (@ARGV < 5);

my ($run, $nodes, $ppn, $minutes, $depth, $blocksize, $iterations) = @ARGV;

my $runname = "titanAMRAdvection$run";
my $basepath = "\$WORKDIR/runs";

`mkdir -p $basepath/$runname`;
`cp ~/amr/advection $basepath/$runname/advection`;

my $cores = int($nodes) * int($ppn);

my $amrRev = `export GIT_DIR=~/amr/.git && git rev-parse HEAD`;
chomp $amrRev;
my $charmRev = `export GIT_DIR=~/charm/.git && git rev-parse HEAD`;
chomp $charmRev;

my $runScript = <<SCRIPT;
#PBS -l walltime=00:$minutes:00,size=$cores
#PBS -N AMRAdvection$run
#PBS -j oe
#PBS -A CHM022
#PBS -l gres=widow1%widow2%widow3

cd $basepath/$runname

date

export MPICH_MAX_THREAD_SAFETY=multiple
export MPICH_UNEX_BUFFER_SIZE=60M
export MPICH_PTL_UNEX_EVENTS=500000

aprun -n $cores ./advection $depth $blocksize $iterations +traceoff +balancer AmrLB
date
SCRIPT

open FILE, ">", "scripts/$runname.pbs" or die $!;
print FILE $runScript;
close FILE;

my $jobid=`qsub scripts/$runname.pbs`;
chomp $jobid;
if ($jobid =~ /(\d+)/) {
    $jobid = $1;
}
#print "running on $cores cores\n";
print "$amrRev \t $charmRev \t titan \t queued \t $jobid \t  $minutes \t $nodes \t $ppn \t $cores \t $depth \t $blocksize \t $iterations\n";
