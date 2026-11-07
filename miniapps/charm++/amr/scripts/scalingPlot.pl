#!/usr/bin/perl

use POSIX;
use strict;
use warnings;

my $id = shift @ARGV;
my @files = @ARGV;
my %scaling;

for my $file (@files) {
    open FILE, "<", $file;

    my $proc = -1;
    my $time = 0.0;
    my $minDepth = -1;
    my $maxDepth = -1;
    my $blockSize = -1;
    my $maxIter = -1;
    my $workUnits = -1;

    for (<FILE>) {
        if (/time for iteration (\d+) to (\d+): ([0-9.]+)/) {
            $scaling{STEP_TIME}{$proc}{$1} = $3*1000;
        }
        if (/(\d+) processors/) {
            $proc = $1;
        }
        if (/minDepth = (\d+)/) {
            $minDepth = $1;
        }
        if (/maxDepth = (\d+)/) {
            $maxDepth = $1;
        }
        if (/maxIter = (\d+)/) {
            $maxIter = $1;
        }
        if (/blockSize = (\d+)/) {
            $blockSize = $1;
        }
        if (/simulation time: ([0-9.]+)/) {
            $time = $1;
        }
        if (/total work units = (\d+)/) {
            $workUnits = $1;
        }
        if (/iteration (\d+), QD latency = ([0-9.]+)/) {
            #my $latency = $2*1000000;
            my $latency = $2*1000;
            if ($latency > 6000) {
                print STDERR "$file: WARNING: high (over 6000us) qd latency of $latency\n";
            } else {
                
            }
            push @{$scaling{QD_LATENCY}{$proc}}, $latency;
        }
        if (/iteration (\d+), Remesh latency = ([0-9.]+)/) {
            #my $latency = $2*1000000;
            my $latency = $2*1000;
            # if ($latency > 6000) {
            #     print STDERR "$file: WARNING: high (over 6000us) qd latency of $latency\n";
            # } else {
                
            # }
            push @{$scaling{REMESH_LATENCY}{$proc}}, $latency;
        }
        if (/Cascade lengths: ([0-9, ]+)/) {
            my @clen = split /, /, $1;
            shift @clen; shift @clen; shift @clen; shift @clen;
            my @clen2;
            for (my $i = 0; $i < @clen; $i+=3) {
                push @clen2, $clen[$i];
            }
            $scaling{CLEN}{$proc} = \@clen2;
        }
    }

    next if ($time == 0.0);

    my $timePerSec = $maxIter / $time;
    my $wuPerSec = $workUnits / $time;
    my $procSecs = $time * $proc;
    my $procTSSec = $timePerSec * $proc;

    $scaling{MAX_ITER}{$proc} = $maxIter;
    $scaling{MIN_DEPTH}{$proc} = $minDepth;
    $scaling{MAX_DEPTH}{$proc} = $maxDepth;
    $scaling{TIME}{$proc} = $time;
    $scaling{STEPS_PER_SEC}{$proc} = $timePerSec;
    $scaling{WORK_UNITS}{$proc} = $workUnits;
    $scaling{WORK_UNITS_PER_SEC}{$proc} = $wuPerSec;
    $scaling{PROCESSOR_SECS}{$proc} = $procSecs;
    $scaling{PROCESSOR_TS_SECS}{$proc} = $procTSSec;
    $scaling{TS_PROC_SEC}{$proc} = $timePerSec / $proc;
    $scaling{WU_PROC_SEC}{$proc} = $wuPerSec / $proc;

    print "$file: $proc, \t time $time, \t depth {$minDepth,$maxDepth}, \t iter $maxIter, \t b $blockSize, \t ts/sec $timePerSec \t WUs $workUnits \t WUs/sec $wuPerSec \t psec $procSecs \t p-ts/sec $procTSSec \t ts/proc/sec $scaling{TS_PROC_SEC}{$proc} \t wu/proc/sec $scaling{WU_PROC_SEC}{$proc} \n";
    #print "$proc $time\n";

    close FILE;
}

if (!(exists $scaling{TIME})) {exit 0};

my $perfect = 0.0;
# output strong scaling time
my %timings = %{$scaling{TIME}};
open SCALING_TIMES, ">", "strongScalingTimes.$id";
for my $key (sort {$a <=> $b} (keys %timings)) {
    next if ($key == -1);
    my $val = $timings{$key};
    if ($perfect == 0.0) {$perfect = $val * $key;}
    my $ideal = $perfect / $key;
    print SCALING_TIMES "$key $val $ideal\n";
}
close SCALING_TIMES;

$perfect = 0.0;
# output timesteps per second

my %timesteps = %{$scaling{STEPS_PER_SEC}};
for my $proc (sort {$a <=> $b} (keys %timesteps)) {
    next if ($proc == -1);
    my $time = $timesteps{$proc};
    if ($perfect == 0.0 &&
        (!((substr $id,0,3) eq "bgq") || $proc > 16)) {
        print STDERR "$id: setting perfect at $proc\n";
        $perfect = $time / $proc;
    }
}

open TIMESTEPS_PER_SEC, ">", "timestepPerSec.$id";
for my $key (sort {$a <=> $b} (keys %timesteps)) {
    next if ($key == -1 || $key == 1);
    my $val = $timesteps{$key};
    #if ($perfect == 0.0) {$perfect = $val / $key;}
    my $ideal = $perfect * $key;
    my $eff = $val / $ideal * 100;
    print TIMESTEPS_PER_SEC "$key $val $ideal $eff\n";
}
close TIMESTEPS_PER_SEC;

my %stepProcTime = %{$scaling{STEP_TIME}};
for my $proc (sort {$a <=> $b} (keys %stepProcTime)) {
    next if ($proc == -1 || $proc == 1);
    open STEP_TIME_FILE, ">", "stepTime.$proc.$id";
    my %timeHash = %{$stepProcTime{$proc}};
    for my $iter (sort {$a <=> $b} (keys %timeHash)) {
        my $time = $timeHash{$iter};
        print STEP_TIME_FILE "$iter $time\n";
    }
    close STEP_TIME_FILE;
}

$perfect = 0.0;
# output timesteps per second
my %wu = %{$scaling{WORK_UNITS_PER_SEC}};
open FILE, ">", "workUnitsPerSec.$id";
for my $key (sort {$a <=> $b} (keys %wu)) {
    next if ($key == -1);
    my $val = $wu{$key};
    if ($perfect == 0.0) {$perfect = $val / $key;}
    my $ideal = $perfect * $key;
    print FILE "$key $val $ideal\n";
}
close FILE;

$perfect = 0.0;
# output TS_PROC_SEC
%wu = %{$scaling{TS_PROC_SEC}};
open FILE, ">", "timestepPerProcSecs.$id";
for my $key (sort {$a <=> $b} (keys %wu)) {
    next if ($key == -1);
    my $val = $wu{$key};
    print FILE "$key $val\n";
}
close FILE;

$perfect = 0.0;
# output WU_PROC_SEC
%wu = %{$scaling{WU_PROC_SEC}};
open FILE, ">", "workUnitPerProcSecs.$id";
for my $key (sort {$a <=> $b} (keys %wu)) {
    next if ($key == -1);
    my $val = $wu{$key};
    print FILE "$key $val\n";
}
close FILE;

my %qds = %{$scaling{QD_LATENCY}};
for my $key (sort {$a <=> $b} (keys %qds)) {
    next if ($key == -1);
    open FILE, ">", "histoQD.$id";
    my $str = join "\n", @{$qds{$key}};
    print FILE "$str\n";
    close FILE;
}

# if the remeshing latency is in the file
if (exists $scaling{REMESH_LATENCY}) {
    my %remesh = %{$scaling{REMESH_LATENCY}};
    for my $key (sort {$a <=> $b} (keys %remesh)) {
        next if ($key == -1);
        open FILE, ">", "histoRemesh.$key.$id";
        my $str = join "\n", @{$remesh{$key}};
        print FILE "$str\n";
        close FILE;
    }
}

open FILE, ">", "QDcandle.$id";
for my $proc (sort {$a <=> $b} (keys %qds)) {
    next if ($proc == -1 or $proc == 1);
    my ($min, $max) = ($scaling{MIN_DEPTH}{$proc}, $scaling{MAX_DEPTH}{$proc});
    my @qdSorted = sort {$a <=> $b} @{$scaling{QD_LATENCY}{$proc}};
    die "incorrect number of QD values" if (@qdSorted + 5 < $scaling{MAX_ITER}{$proc} / 3);
    my ($qdMin, $qdMax, $qdMed, $qd5th, $qd95th) = ($qdSorted[0],
                                                    $qdSorted[@qdSorted - 1],
                                                    $qdSorted[@qdSorted / 2],
                                                    $qdSorted[@qdSorted * 0.05],
                                                    $qdSorted[@qdSorted * 0.95]);
    print "$id: {min=$qdMin, max=$qdMax, med=$qdMed, 5th=$qd5th, 95th=$qd95th}\n";
    my $w = 0.19;
    my $pos = $proc + $proc * $w * ($max - 10);
    print FILE "$proc $min $max $qdMin $qd5th $qdMed $qd95th $qdMax $pos\n";
}
close FILE;

# if the remeshing latency is in the file
if (exists $scaling{REMESH_LATENCY}) {
    my %remesh = %{$scaling{REMESH_LATENCY}};
    open FILE, ">", "RMcandle.$id";
    for my $proc (sort {$a <=> $b} (keys %remesh)) {
        next if ($proc == -1 or $proc == 1);
        my ($min, $max) = ($scaling{MIN_DEPTH}{$proc}, $scaling{MAX_DEPTH}{$proc});
        my @rmSorted = sort {$a <=> $b} @{$scaling{REMESH_LATENCY}{$proc}};
        my @qdSorted = sort {$a <=> $b} @{$scaling{QD_LATENCY}{$proc}};
        die "mismatch size" if (@rmSorted != @qdSorted);
        die "incorrect number of RM values" if (@rmSorted + 5 < $scaling{MAX_ITER}{$proc} / 3);
        my ($rmMin, $rmMax, $rmMed, $rm5th, $rm95th) = ($rmSorted[0],
                                                        $rmSorted[@rmSorted - 1],
                                                        $rmSorted[@rmSorted / 2],
                                                        $rmSorted[@rmSorted * 0.05],
                                                        $rmSorted[@rmSorted * 0.95]);
        my $qdMed = $qdSorted[@qdSorted / 2];
        my $diffMicro = ($rmMed - $qdMed) * 1000;
        print "$id: {min=$rmMin, max=$rmMax, med=$rmMed, 5th=$rm5th, 95th=$rm95th}\n";
        my $w = 0.19;
        my $pos = $proc + $proc * $w * ($max - 10);
        my $rmMedMicro = $rmMed * 1000;
        print FILE "$proc $min $max $rmMin $rm5th $rmMed $rm95th $rmMax $pos $rmMedMicro $diffMicro\n";
    }
    close FILE;
}
