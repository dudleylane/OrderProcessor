#!/usr/bin/env bash
# line-length.sh: fail on a line of C++ longer than .clang-format's ColumnLimit, 120 columns (#615).
#
# clang-format breaks code at the limit, but .clang-format has ReflowComments off, so it leaves a long comment as it is,
# and the format check (clang-format-22 --dry-run -Werror) passed comments far past the limit. Turning ReflowComments on
# would rewrite more than the long lines: it also respaces comments such as //8 and reindents commented-out code. So the
# limit is checked here instead, over the files the format check reads.
#
# usage: scripts/line-length.sh [DIR]   checks the tree at DIR (default: this script's repository)
#
# It counts characters, not bytes, with the CR that ends a CRLF line taken off. Format Check's clang-format job and the
# pre-push hook run it after clang-format.
#
# Exit codes: 0 every line is within the limit; 1 a line is over it, each one named with its file, line and length;
# 2 DIR has no .clang-format with a ColumnLimit

set -uo pipefail

dir=${1:-"$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"}
cd "$dir" 2>/dev/null || { echo "line-length: can't enter $dir" >&2; exit 2; }
limit=$(sed -n 's/^ColumnLimit:[[:space:]]*\([0-9][0-9]*\).*/\1/p' .clang-format 2>/dev/null)
if [[ -z "$limit" ]]; then
    echo "line-length: $dir/.clang-format gives no ColumnLimit" >&2
    exit 2
fi

files=()
for top in src test app bench; do
    [[ -d "$top" ]] || continue
    while IFS= read -r -d '' file; do
        files+=("$file")
    done < <(find "$top" \( -name '*.h' -o -name '*.cpp' \) -print0)
done
if (( ${#files[@]} == 0 )); then
    echo "line-length: no C++ files under src, test, app or bench"
    exit 0
fi

# In a UTF-8 locale awk's length() counts characters, so an em dash is one column, as clang-format counts it
LC_ALL=C.UTF-8 awk -v limit="$limit" '
    {
        sub(/\r$/, "")
        if (length($0) > limit) {
            printf "%s:%d: %d characters, over the %d of .clang-format'"'"'s ColumnLimit\n", FILENAME, FNR, length($0), limit
            over++
        }
    }
    END {
        if (over) {
            printf "line-length: %d line(s) over %d columns; wrap each by hand\n", over, limit
            exit 1
        }
    }
' "${files[@]}" >&2 || exit 1
echo "line-length: every line of ${#files[@]} files is within ${limit} columns"
