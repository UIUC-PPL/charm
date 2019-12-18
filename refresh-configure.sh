#!/bin/bash

# Process autoconf/automake files

set -o errexit -o nounset

# Get in position to process build scripts
cd src/scripts

# Symlink hwloc in temporarily
ln -s ../../contrib/hwloc hwloc

# Run autotools
autoreconf
autoheader
rm -rf autom4te.cache

# Remove symlink
rm hwloc
