#!/usr/bin/perl

use strict;
use warnings;

die "usage: test_lu.pl <exec> <execflags> <target>" if (@ARGV < 1);

# "test identifier" => [params...]
my %smalltests = (
    "48_1" => [48, 48, 500],
    "48_2" => [48, 24, 500],
    "48_3" => [48, 16, 500],
    "48_4" => [48, 12, 500]
    );

my %agglomtests = (
    "agglom_32_1" => [32, 8, 500, 9],
    "agglom_32_2" => [32, 8, 500, 8],
    "agglom_32_3" => [32, 8, 500, 7],
    "agglom_32_4" => [32, 8, 500, 4],
    "agglom_32_5" => [32, 8, 500, 3],
    "agglom_32_6" => [32, 8, 500, 2],
    "agglom_32_7" => [32, 8, 500, 1],
    "agglom_32_8" => [32, 4, 500, 2],
    "agglom_32_9" => [32, 4, 500, 1]
    );

my %mappingtests = (
    "mapping_512_2" => [512, 128, 500, 128, 2],
    "mapping_512_3" => [512, 128, 500, 128, 3],
    "mapping_512_4" => [512, 64, 500, 128, 3, 4, 1],
    "mapping_512_5" => [512, 64, 500, 128, 3, 4, 1, 1],
    "mapping_512_6" => [512, 64, 500, 128, 3, 4, 1, 2],
    "mapping_512_7" => [512, 64, 500, 128, 3, 1, 4, 2, 4],
    "mapping_512_8" => [512, 64, 500, 128, 3, 1, 4, 2, 2],
    "mapping_512_9" => [512, 64, 500, 128, 3, 1, 4, 2, 1],
    );

my %mediumtests = (
    "512_1" => [512, 256, 500],
    "512_2" => [512, 128, 500],
    "1024_3" => [1024, 256, 500],
    "1024_4" => [1024, 128, 500],
    "1024_5" => [1024, 64, 500]
    );

my %largetests = (
    "2048_1" => [2048, 256, 500],
    "2048_2" => [2048, 512, 500],
    "4096_3" => [4096, 256, 500],
    "4096_4" => [4096, 512, 500]
    );

my $passed = 0;
my $failed = 0;

run_test_type("small tests, with different block sizes", %smalltests);
run_test_type("small tests, with different pivot agglomeration batch sizes", %agglomtests);
run_test_type("medium tests, with different mappings", %mappingtests);
run_test_type("medium tests, with different block sizes", %mediumtests);
run_test_type("large tests, with different block sizes", %largetests);

sub run_test_type {
    my ($name, %tests) = @_;
    print "Starting $name\n";
    run_tests(%tests);
    print "\nREPORT: Passed $passed, failed $failed\n\n";
}

sub run_tests {
    my %tests = @_;
    for my $key (sort keys %tests) {
        print "Executing test $key...";
        my $ex =  join(' ', @ARGV) . " @{$tests{$key}}";
        my $res = `$ex 2>&1 | grep residual`;
        if ($res =~ /residual = ([\d\.]*)/ and $1 < 16) {
            print "PASSED r = $1\n";
            $passed++;
        } else {
            print "FAILED r = $1, config: $key, $ex\n";
            $failed++;
        }
    }
}
