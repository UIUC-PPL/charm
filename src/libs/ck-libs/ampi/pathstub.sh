#!/bin/bash
DIR="$(dirname "$0")"
FN="$(basename "$0")"

# detect and handle circular calls when:
# * building with the MPI machine layer
# * using Charmrun's ++mpiexec
if [[ "$FROM_CHARMC" = '1' || "$FROM_CHARMRUN" = '1' ]]; then
  # remove this stub's location from PATH
  IFS=: read -r -d '' -a path_array < <(printf '%s:\0' "$PATH")
  new_path_array=( )
  for i in "${!path_array[@]}"; do
    if [[ ! "${path_array[i]}" -ef "$DIR" ]]; then
      new_path_array+=( "${path_array[i]}" )
    fi
  done
  export PATH=$(IFS=: ; echo "${new_path_array[*]}")

  # relaunch the intended command
  "$FN" "$@"
  exit $?
fi

# pass control to the AMPI toolchain wrappers
"$DIR/../$FN.ampi" "$@"
