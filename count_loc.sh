#!/usr/bin/env bash
# count_loc.sh — counts non-blank lines of code in our own source files,
# split into: client, server, doubleratchet library.
#
# Excluded on purpose: build output, third-party/vendor code, generated
# files (Qt .ts translations, generated EmojiData.js), binaries, keystores,
# assets (fonts/icons/sounds), and build/project config (CMakeLists.txt,
# go.mod, gradle files, IDE settings, etc).
#
# Usage: ./count_loc.sh

set -euo pipefail
cd "$(dirname "$0")"

# Reads a NUL-separated file list on stdin, prints "<files> <lines>"
# where <lines> is the number of non-blank lines across all of them.
count_group() {
    local files=0 lines=0 n
    while IFS= read -r -d '' f; do
        n=$(grep -c '[^[:space:]]' -- "$f") || true
        files=$((files + 1))
        lines=$((lines + n))
    done
    printf '%d %d\n' "$files" "$lines"
}

print_row() {
    printf '  %-24s %5s files   %8s lines\n' "$1" "$2" "$3"
}

grand_files=0
grand_lines=0

# ---------------------------------------------------------------------------
# Client: Qt/QML UI (root *.qml/*.js/*.cpp), C++ backend (src/), Android glue
# (android/src/**/*.java), and the emoji-table generator script (tools/).
# ---------------------------------------------------------------------------
echo "=== Client ==="

read -r cf cl < <(
    find client/src -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
    | count_group
)
print_row "src/ (C++)" "$cf" "$cl"
grand_files=$((grand_files + cf)); grand_lines=$((grand_lines + cl))

read -r qf ql < <(
    find client -maxdepth 1 -type f \( -name '*.qml' -o -name '*.js' -o -name '*.cpp' \) \
        ! -name 'EmojiData.js' -print0 \
    | count_group
)
print_row "root (QML/JS/C++)" "$qf" "$ql"
grand_files=$((grand_files + qf)); grand_lines=$((grand_lines + ql))

read -r jf jl < <(
    find client/android/src -type f -name '*.java' -print0 \
    | count_group
)
print_row "android/src (Java)" "$jf" "$jl"
grand_files=$((grand_files + jf)); grand_lines=$((grand_lines + jl))

read -r pf pl < <(
    find client/tools -type f -name '*.py' -print0 \
    | count_group
)
print_row "tools/ (Python)" "$pf" "$pl"
grand_files=$((grand_files + pf)); grand_lines=$((grand_lines + pl))

client_files=$((cf + qf + jf + pf))
client_lines=$((cl + ql + jl + pl))
print_row "TOTAL" "$client_files" "$client_lines"
echo

# ---------------------------------------------------------------------------
# Server: Go backend (no vendor/ dir, nothing generated in this repo).
# ---------------------------------------------------------------------------
echo "=== Server ==="

read -r sf sl < <(
    find server -type f -name '*.go' -print0 \
    | count_group
)
print_row "*.go" "$sf" "$sl"
print_row "TOTAL" "$sf" "$sl"
grand_files=$((grand_files + sf)); grand_lines=$((grand_lines + sl))
echo

# ---------------------------------------------------------------------------
# Double Ratchet library: C++ sources at the crate root (build/ and .vscode/
# excluded).
# ---------------------------------------------------------------------------
echo "=== Double Ratchet library ==="

read -r df dl < <(
    find doubleratchet -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
    | count_group
)
print_row "*.cpp/*.h" "$df" "$dl"
print_row "TOTAL" "$df" "$dl"
grand_files=$((grand_files + df)); grand_lines=$((grand_lines + dl))
echo

echo "=== Grand total ==="
print_row "client + server + doubleratchet" "$grand_files" "$grand_lines"
