#!/opt/local/bin/perl

use strict;
use warnings;

use Text::CSV;

my $parser = Text::CSV->new();
my %xmap;

for (<>) {
    if ($parser->parse($_)) {
        my @cols = $parser->fields();
        my ($runnum, $commit, $machine, $type, $status, $jobid, $hour, $min,
            $procs, $matrix, $block, $memory, $pivot, $mapping, $tilex, $tiley,
            $rotate, $stride,$t1, $send, $time, $peak) = @cols;
        my $gflopsMax = 10.39;
        my $gflops = ($peak / 100) * $gflopsMax;
        my $totalTflops = ($peak / 100) * $gflopsMax * $procs / 1000;
        my $aspect = $tiley / $tilex;
        my $upar;
        if (int($rotate) == 0) {
            $upar = $tiley;
        } else {
            $upar = $tilex / $rotate * $tiley;
        }
        my $percentU = $upar / $procs * 100.0;

        if ($stride > 12) {
            $stride = 12;
        }
        my $numActive = 12 / $stride;

        ### Replace first hash key with x to graph
        $xmap{$send}{$procs} = $gflops;

        ### Exclusive
        #$xmap{$procs}{$type} = $gflops;

        ### Weak scaling
        #$xmap{$procs}{0} = $totalTflops;
    }
}

my $temp = "tmp-graph.dat";
chomp $temp;
open DATFILE, ">", "$temp" or die $!;

my @plist = (132, 528, 2112);
my @exclusive = ("baseline", "noisolateactive", "isolateu", "redncb");
my @weak = (0);

for my $key (sort {$a <=> $b} (keys %xmap)) {
    print DATFILE "$key ";
    foreach my $p (@plist) {
        if (exists $xmap{$key}{$p}) {
            print DATFILE "$xmap{$key}{$p} ";
        } else {
            print DATFILE "nan ";
        }
    }
    print DATFILE "\n";
}

close DATFILE;

my $plotScript = <<END;
set terminal postscript color
set border 1+2+4+8 lw 1.3

set pointsize 1.5

set ylabel 'GFlops' font "Myriad Pro,20"

set style line 1 lt 2 lc rgb "red" lw 3
set style line 2 lt 1 lc rgb "blue" lw 3
set style line 3 lt 4 lc rgb "green" lw 3
set style line 4 lt 8 lc rgb "orange" lw 3

set key font "Myriad Pro,20" spacing 2

### Exclusive prio classes
# set logscale x
# set xlabel 'Number of processors' font "Myriad Pro,20"
# set xrange [100:2300]
# set yrange [5:7.8]
# set xtics axis in nomirror 132,4,2112 font "Myriad Pro,20"
# set ytics autofreq nomirror font "Myriad Pro,20"
# set mxtics 20
# set mytics 5
# plot '$temp' using 1:5 title 'Active panel and reduction callback isolated' with linespoints ls 32 lw 6 pt 7 lc rgb "#b22222", \\
#      '$temp' using 1:2 title 'Active panel isolated' with linespoints ls 2 lw 6 pt 5 lc rgb "#000080", \\
#      '$temp' using 1:4 title 'Active panel and U triangular solves isolated' with linespoints ls 3 lw 6 pt 9 lc rgb "#006400", \\
#      '$temp' using 1:3 title 'No isolation' with linespoints lt 2 lw 6 pt 13 lc rgb "#ff8c00"

### Send limit
set xlabel 'Locally incomplete message sends' font "Myriad Pro,20"
set xtics autofreq nomirror 1,1,5 font "Myriad Pro,20"
set ytics autofreq nomirror font "Myriad Pro,20"
set yrange [6:7.5]
set mytics 5
plot '$temp' using 1:2 title '132 processors' with linespoints ls 32 lw 6 pt 7 lc rgb "#b22222", \\
     '$temp' using 1:3 title '528 processors' with linespoints ls 2 lw 6 pt 5 lc rgb "#000080", \\
     '$temp' using 1:4 title '2112 processors' with linespoints ls 3 lw 6 pt 9 lc rgb "#006400"

### Aspect ratio
# set xlabel 'Aspect ratio' font "Myriad Pro,20"
# set xtics autofreq nomirror font "Myriad Pro,20"
# set ytics autofreq nomirror font "Myriad Pro,20"
# set xrange [0:8.5]
# set mxtics 5
# set mytics 5
# plot '$temp' using 1:2 title '132 processors' with linespoints ls 32 lw 6 pt 7 lc rgb "#b22222", \\
#      '$temp' using 1:3 title '528 processors' with linespoints ls 2 lw 6 pt 5 lc rgb "#000080", \\
#      '$temp' using 1:4 title '2112 processors' with linespoints ls 3 lw 6 pt 9 lc rgb "#006400"

### Stride
# set xlabel 'Active panel PEs/node' font "Myriad Pro,20"
# set xtics autofreq nomirror font "Myriad Pro,20"
# set ytics autofreq nomirror font "Myriad Pro,20"
# set mxtics 5
# set mytics 5
# plot '$temp' using 1:2 title '132 processors' with linespoints ls 32 lw 6 pt 7 lc rgb "#b22222", \\
#      '$temp' using 1:3 title '528 processors' with linespoints ls 2 lw 6 pt 5 lc rgb "#000080", \\
#      '$temp' using 1:4 title '2112 processors' with linespoints ls 3 lw 6 pt 9 lc rgb "#006400"

### Rotation
# set xlabel 'Percent of PEs performing triangluar solves' font "Myriad Pro,20"
# set xtics autofreq nomirror
# set ytics autofreq nomirror
# #set xrange [0:500]
# set mxtics 5
# set mytics 5
# plot '$temp' using 1:2 title '132 processors' with linespoints ls 32 lw 6 pt 7 lc rgb "#b22222", \\
#      '$temp' using 1:3 title '528 processors' with linespoints ls 2 lw 6 pt 5 lc rgb "#000080", \\
#      '$temp' using 1:4 title '2112 processors' with linespoints ls 3 lw 6 pt 9 lc rgb "#006400"

### Weak scaling
# set logscale xy
# set xlabel 'Number of PEs' font "Myriad Pro,20"
# set ylabel 'Total TFlops' font "Myriad Pro,20"
# set xrange [100:8500]
# #set yrange [5:7.5]
# set xtics axis in nomirror 128,8,8064 font "Myriad Pro,20"
# set ytics autofreq nomirror font "Myriad Pro,20"
# #set xrange [0:500]
# set mxtics 15
# set mytics 15
# plot '$temp' using 1:2 title 'Weak scaling matrix size with PEs' with linespoints ls 2 lw 6 pt 5 lc rgb "#000080"
END

my $opFile = "tmp-graph.ps";
open (OPIPE, "| gnuplot > $opFile && ps2pdf $opFile") or die $!;
print OPIPE "$plotScript";
