#!/usr/bin/env bash
# Turns the profiles coverage/run.sh collected into <out-dir>/REPORT.md.
# See coverage/README.md.
#
# usage: coverage/report.sh <build-dir> <out-dir>
#
# Merges the profiles per tier and cumulatively (ci, ci+all, ci+all+ex, for
# the tiers present), runs llvm-cov over every test and example program
# built against <build-dir> plus libreconverse, and writes, in
# <out-dir>/report/:
#   <label>.profdata, <label>-report.txt (llvm-cov report),
#   <label>-summary.json (export -summary-only), <label>.lcov (export lcov),
#   objects.txt (the binaries measured), ck-convcore.txt,
#   html/ (llvm-cov show of the last label; only with COV_HTML=1).
#
# Environment: LLVM_PROFDATA, LLVM_COV override the tools (each may be a
# command with arguments, e.g. "xcrun llvm-cov").

die() { echo "report.sh: $*" >&2; exit 2; }
[[ $# -eq 2 ]] || die "usage: coverage/report.sh <build-dir> <out-dir>"
B=$(cd "$1" 2>/dev/null && pwd -P) || die "no build directory '$1'"
OUT=$(cd "$2" 2>/dev/null && pwd -P) || die "no output directory '$2'"
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
[[ -f $B/CMakeCache.txt ]] || die "$B is not a Charm++ build directory"
R=$OUT/report
mkdir -p "$R"

# The llvm tools must match the compiler that instrumented the build: try
# the variable, then the compiler's own directory, then xcrun (Apple clang,
# whose tools are not next to the /usr/bin shims), then PATH (plain, then
# suffixed with the compiler's major version).
cxx=$(grep -m1 '^CMAKE_CXX_COMPILER:' "$B/CMakeCache.txt" | cut -d= -f2)
find_tool() {
  local name=$1 override=$2 dir major
  if [[ -n $override ]]; then echo "$override"; return 0; fi
  if [[ -n $cxx ]] && command -v "$cxx" >/dev/null 2>&1; then
    dir=$(dirname "$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$(command -v "$cxx")")")
    if [[ -x $dir/$name ]]; then echo "$dir/$name"; return 0; fi
  fi
  if command -v xcrun >/dev/null 2>&1 && xcrun -f "$name" >/dev/null 2>&1; then
    echo "xcrun $name"; return 0
  fi
  if command -v "$name" >/dev/null 2>&1; then echo "$name"; return 0; fi
  major=$("$cxx" --version 2>/dev/null | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)
  if [[ -n $major ]] && command -v "$name-$major" >/dev/null 2>&1; then
    echo "$name-$major"; return 0
  fi
  return 1
}
read -r -a PROFDATA <<< "$(find_tool llvm-profdata "${LLVM_PROFDATA:-}")" || die "llvm-profdata not found (set LLVM_PROFDATA)"
read -r -a LLVMCOV <<< "$(find_tool llvm-cov "${LLVM_COV:-}")" || die "llvm-cov not found (set LLVM_COV)"
[[ ${#PROFDATA[@]} -gt 0 && ${#LLVMCOV[@]} -gt 0 ]] || die "llvm-profdata or llvm-cov not found"
echo "tools: ${PROFDATA[*]}, ${LLVMCOV[*]}"

# Cumulative labels over the tiers that have profiles, in tier order. A tier
# run against another build (rebuilt since, or a different one) would mix
# counters from different code into the merge, so it stops the report.
stamp=$("$SRC/coverage/buildstamp.sh" "$B")
labels=()
tiers=()
prev=""
for t in ci all ex; do
  find "$OUT/profraw/$t" -name '*.profraw' 2>/dev/null > "$R/$t.inputs"
  if [[ ! -s $R/$t.inputs ]]; then rm -f "$R/$t.inputs"; continue; fi
  [[ -f $OUT/stamp-$t.txt && -f $OUT/programs-$t.txt ]] ||
    die "tier $t has profiles but no stamp-$t.txt or programs-$t.txt; rerun coverage/run.sh --tier $t"
  [[ $(cat "$OUT/stamp-$t.txt") == "$stamp" ]] ||
    die "tier $t was run against a different build of $B; rerun it (coverage/run.sh --tier $t) or remove $OUT/profraw/$t"
  tiers+=("$t")
  if [[ -z $prev ]]; then label=$t; else label=$prev+$t; fi
  [[ -n $prev ]] && echo "$R/$prev.profdata" >> "$R/$t.inputs"
  "${PROFDATA[@]}" merge -sparse -f "$R/$t.inputs" -o "$R/$label.profdata" ||
    die "llvm-profdata merge failed for $label"
  labels+=("$label")
  prev=$label
done
[[ ${#labels[@]} -gt 0 ]] || die "no profiles under $OUT/profraw; run coverage/run.sh first"

# Every program run.sh built for the merged tiers (programs-<tier>.txt),
# plus libreconverse, which holds every reconverse object (charm's own
# libraries are static, so only what a program links is visible).
{
  for t in "${tiers[@]}"; do
    while read -r f; do [[ -f $f ]] && echo "$f"; done < "$OUT/programs-$t.txt"
  done | sort -u
  for f in "$B"/lib/libreconverse.dylib "$B"/lib/libreconverse.so; do
    [[ -f $f ]] && echo "$f"
  done
} > "$R/objects.txt"
OBJ=()
while read -r f; do OBJ+=(-object "$f"); done < "$R/objects.txt"
echo "objects: $(wc -l < "$R/objects.txt" | tr -d ' ')"

# Measured: Charm++ (src/ and the build's generated and copied headers) and
# reconverse (contrib/reconverse). Not measured: tests, examples, LCI,
# hwloc, bundled third-party code, system headers.
esc() { printf '%s' "$1" | sed 's/[][\.^$*+?(){}|]/\\&/g'; }
IGN="^($(esc "$SRC")|$(esc "$B"))/(tests|examples|benchmarks)/"
IGN+="|/contrib/reconverse/(tests|examples|benchmarks)/|/_deps/|/contrib/hwloc/|hwloc"
IGN+="|boost-context|concurrentqueue|^/usr/|^/opt/local/|/CommandLineTools/|/Xcode[^/]*[.]app/"
echo "$IGN" > "$R/ignore-regex.txt"

for label in "${labels[@]}"; do
  P=$R/$label.profdata
  "${LLVMCOV[@]}" report "${OBJ[@]}" -instr-profile="$P" -ignore-filename-regex="$IGN" \
    > "$R/$label-report.txt" 2> "$R/$label-report.err"
  "${LLVMCOV[@]}" export -summary-only "${OBJ[@]}" -instr-profile="$P" -ignore-filename-regex="$IGN" \
    > "$R/$label-summary.json" 2>> "$R/$label-report.err"
  "${LLVMCOV[@]}" export -format=lcov "${OBJ[@]}" -instr-profile="$P" -ignore-filename-regex="$IGN" \
    > "$R/$label.lcov" 2>> "$R/$label-report.err" || die "llvm-cov export failed for $label"
  echo "$label: $(tail -1 "$R/$label-report.txt" | awk '{print "regions", $4, "functions", $7, "lines", $10}') (all measured files, before header mapping)"
done
last=${labels[${#labels[@]}-1]}

c++ -O1 -o "$R/ck-convcore-summary" "$SRC/coverage/ck-convcore-summary.C" ||
  die "cannot compile coverage/ck-convcore-summary.C"
"$R/ck-convcore-summary" "$R/$last.lcov" > "$R/ck-convcore.txt"

if [[ ${COV_HTML:-0} = 1 ]]; then
  rm -rf "$R/html"
  "${LLVMCOV[@]}" show -format=html -output-dir="$R/html" "${OBJ[@]}" \
    -instr-profile="$R/$last.profdata" -ignore-filename-regex="$IGN" 2> /dev/null ||
    echo "report.sh: llvm-cov show failed; no HTML" >&2
fi

python3 "$SRC/coverage/covreport.py" --src "$SRC" --build "$B" --out "$OUT" \
  --labels "${labels[*]}" || die "covreport.py failed"
echo "wrote $OUT/REPORT.md"
