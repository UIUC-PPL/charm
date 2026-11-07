#!/usr/bin/perl

use strict;
use warnings;

my %blockTimingStart;
my %blockTimingStop;

die "usage: <filename> <dgemm-milliseconds> <x-axis> <y-axis>" if ($#ARGV + 1 < 2);

my ($file, $DGEMM) = @ARGV;
shift @ARGV;
shift @ARGV;

my $xaxis = 0;
my $yaxis = 0;

if ($#ARGV + 1 > 1) {
    ($xaxis, $yaxis) = @ARGV;
}

my $runName = `basename $file`;
$runName =~ s/(.*)\.[^\.]*/$1/;

my $temp = "tmp-" . $runName . ".dat";
chomp $temp;

open FILE, ">", "$temp" or die $!;
open RFILE, "<", "$file";

my $chareArray;
my $PEs = 0;

for (<RFILE>) {
    if (/Chare Array size: (\d+) X/) {
        $chareArray = $1;
    }
    if (/RESULT procs:\s+(\d+)/) {
        $PEs = $1;
    }
    if (/^Block (\d+) queueing local LU at internalStep (\d+), start time = \d+\.\d+, time = (\d+\.\d+)$/) {
        if ($1 eq $2) {
            $blockTimingStart{$1} = $3;
        }
    }
    if (/^Block (\d+) finished local LU at internalStep (\d+), time = (\d+\.\d+)$/) {
        if ($1 eq $2) {
            $blockTimingStop{$1} = $3;
        }
    }
}

if ($PEs == 0) {
    exit 1;
}

my $output = 0;
my $totalTime = 0;

for my $key (sort {$a <=> $b} (keys %blockTimingStart)) {
    if (exists $blockTimingStop{$key} and exists $blockTimingStart{$key}) {
        my $timeStop = $blockTimingStop{$key};
        my $timeStart = $blockTimingStart{$key};
        my $timeStopPrev = $blockTimingStop{$key-1};
        my $delay = 0;

        my $diff = $timeStop - $timeStart;
        if (exists $blockTimingStop{$key-1}) {
            $delay = $timeStart - $timeStopPrev;
        }
        if ($diff >= 0 and $delay >= 0) {
            my $DGEMMsecs = $DGEMM / 1000;
            my $TUmodel = (($chareArray-$key-1)*($chareArray-$key-1)/$PEs)*$DGEMMsecs;
            $output = 1;
            $totalTime += $diff;
            $totalTime += $delay;
            print FILE "$key $diff $delay $TUmodel\n";
        }
    }
}

close FILE;
close RFILE;

print "Total measured time = $totalTime\n";

if ($output == 0) {
    exit 1;
}

# Now output for gnuplot
my $plotCmds = <<END;
set terminal postscript color solid
END

if ($xaxis == 0) {
  $plotCmds .= "set autoscale x\n";
} else {
  $plotCmds .= "set xrange [0:$xaxis]\n";
}

if ($yaxis == 0) {
  $plotCmds .= "set autoscale y\n";
} else {
  $plotCmds .= "set yrange [0:$yaxis]\n";
}

my $opFile = $runName . ".eps";
$plotCmds .= <<END;
set terminal postscript eps color enhanced
set title '$runName'
set output '$opFile'
set ylabel 'Execution Time (seconds)'
set xlabel 'LU Step'
set pointsize 0.5
plot '$temp' using 1:2 title 'Active panel time' with points lt 1 pt 5, \\
     '$temp' using 1:3 title 'Time outside active panel' with points lt 2 pt 9, \\
     '$temp' using 1:4 title 'Trailing update time per step' with lines lw 2
END

open (OPIPE, "| gnuplot") or die $!;
print OPIPE "$plotCmds";

