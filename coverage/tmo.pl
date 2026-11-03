#!/usr/bin/perl
# usage: tmo.pl SECS cmd args...
# Runs cmd in its own process group. If it is still running after SECS
# seconds, kills the whole group (so lcrun and the processes it started go
# too) and exits 124, as GNU timeout does. Otherwise exits with cmd's status
# (128+N if a signal N ended it). Used where `timeout` is not installed.
use strict;
use warnings;

my $secs = shift @ARGV;
die "usage: tmo.pl SECS cmd args...\n" unless defined $secs && $secs =~ /^\d+$/ && @ARGV;

my $pid = fork();
die "fork: $!\n" unless defined $pid;
if ($pid == 0) {
    setpgrp(0, 0);
    exec @ARGV or die "exec $ARGV[0]: $!\n";
}

# An interrupt of this helper takes the group down with it.
for my $sig (qw(INT TERM HUP)) {
    $SIG{$sig} = sub { kill 'KILL', -$pid; waitpid($pid, 0); exit 130; };
}
$SIG{ALRM} = sub { kill 'KILL', -$pid; waitpid($pid, 0); exit 124; };
alarm $secs;
waitpid($pid, 0);
my $st = $?;
alarm 0;
exit(($st & 127) ? 128 + ($st & 127) : $st >> 8);
