#!/usr/bin/perl

use strict;
use warnings;

for (<>) {
    chomp $_;
    $_=~ tr/\015//d;
    my @cols = split ',', $_;
    my ($runnum, $commit, $machine, $type, $status, $jobid, $hour, $min,
        $procs, $matrix, $block, $memory, $pivot,
        $mapping, $tilex, $tiley, $rotate, $stride) = @cols;
    if ($type eq "skip") {
        print "Skipping runnum = $runnum\n";
        next;
    } elsif ($runnum ne "" && $type ne "") {
        print "Generating script for params: @cols\n";
    } else {
        die "No runnum or type is empty";
    }

    my $events = 60000;
    $events = int(2.2*int($procs)) + 60000;

    open FILE, ">script-luexp-$runnum-$type.pbs";

    my $script = <<END;
#!/bin/bash
#PBS -N luexp-$runnum-$type
#PBS -j oe
#PBS -l gres=widow2%widow1%widow3,walltime=$hour:$min:00,size=$procs
#PBS -A csc076

export MPICH_UNEX_BUFFER_SIZE=60M
export MPICH_MAX_SHORT_MSG_SIZE=1k
export MPICH_PTL_UNEX_EVENTS=$events

date
aprun -n $procs /tmp/proj/csc076/lubin/test
date
aprun -n $procs -N 12 -cc cpu /tmp/proj/csc076/lubin/lu.$type $matrix $block $memory $pivot $mapping $tilex $tiley $rotate $stride +skip_cpu_topology +noAnytimeMigration
date
END

print FILE "$script";
close FILE;
}
