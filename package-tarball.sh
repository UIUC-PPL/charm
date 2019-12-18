#!/bin/bash

# Print commands as they run
set -v

# Halt on error
set -e

# Set 'release' mode if requested, influences commitid.sh
export RELEASE="0"
if [[ $# -gt 0 ]]
then
    arg=$1
    shift

    if [ "$arg" = "--release" ]
    then
        echo Saw $arg
        RELEASE="1"
    else
        echo "Unrecognized argument '$arg'"
        exit 1
    fi

    if [[ $# -gt 0 ]]
    then
        echo "Unrecognized argument '$arg'"
        exit 1
    fi
fi

# Make sure the working copy and index are completely clean
git diff --quiet --exit-code HEAD || { echo "Error: Working directory is not clean"; exit 1; }

# Emit a static indicator of the original commit
pushd src/scripts
SRCBASE=`pwd` ./commitid.sh
rm VERSION.new
popd

# Refresh autoconf/automake files
./refresh-configure.sh
git add -f src/scripts/VERSION
git add -f src/aclocal.m4 src/configure src/conv-autoconfig.h.in

# Stage all of the modified files
git add -u

# Get an identifier for the current state of the code
object_id=`git write-tree`

# Construct the target file/folder name
version="charm-$(cat src/scripts/VERSION)"

# Generate the distribution tarball
git archive --format=tar.gz --prefix="$version/" -o $version.tar.gz $object_id

# And clean up the mess we made
git reset --hard
