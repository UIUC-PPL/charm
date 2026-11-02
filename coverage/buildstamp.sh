#!/usr/bin/env bash
# Prints one line identifying the libraries of a Charm++ build, so that
# coverage/report.sh merges only profiles collected against the build it
# measures. See coverage/README.md.
#
# usage: coverage/buildstamp.sh <build-dir>

[[ $# -eq 1 && -d $1/lib ]] || { echo "usage: coverage/buildstamp.sh <build-dir>" >&2; exit 2; }
find "$1/lib" -maxdepth 1 -type f \( -name '*.a' -o -name 'libreconverse.*' \) | sort |
  while read -r f; do cksum < "$f"; done | cksum
