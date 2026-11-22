#!/usr/bin/env bash
# line-endings.sh: check the tracked files' line endings against the repository's rule (#639).
#
# The rule: C++ and CMake files (*.h, *.hpp, *.cpp, *.cc, *.cmake and CMakeLists.txt) are CRLF; the shell scripts
# (*.sh and the pre-push hook), everything under .github/ and README.md are LF; and no file mixes the two. Other files
# keep what they have. It reads what is committed, git's index, and converts nothing. There is no .gitattributes on
# purpose: an eol setting there would make git convert files as it checks them out, and each one's next commit would
# be a whole-file diff, which hides the real change from review and from git blame.
#
# usage: scripts/line-endings.sh [DIR]   checks the git work tree at DIR (default: this script's repository)
#
# scripts/lint.sh runs it, so Format Check's lint job does.
#
# Exit codes: 0 every tracked file keeps the rule; 1 a file breaks it, each one named; 2 DIR isn't a git work tree,
# or git can't list its files

set -uo pipefail

dir=${1:-"$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"}
if ! listing=$(git -C "$dir" ls-files --eol 2>/dev/null); then
    echo "line-endings: $dir isn't a git work tree, or git can't list its files" >&2
    exit 2
fi

# Each line of the listing is "i/<index> w/<worktree> attr/<attributes>", a tab, and the path
bad=$(awk -F '\t' '
    {
        split($1, columns, " ")
        index_eol = columns[1]
        path = $2
    }
    index_eol == "i/mixed" {
        print "mixed CRLF and LF: " path
        next
    }
    path ~ /\.(h|hpp|cpp|cc|cmake)$/ || path ~ /(^|\/)CMakeLists\.txt$/ {
        if (index_eol != "i/crlf") print "not CRLF, as every C++ and CMake file is: " path
        next
    }
    path ~ /\.sh$/ || path ~ /^scripts\/hooks\// || path ~ /^\.github\// || path == "README.md" {
        if (index_eol != "i/lf" && index_eol != "i/none") print "not LF, as the scripts, .github/ and README.md are: " path
        next
    }
' <<< "$listing")

if [[ -n "$bad" ]]; then
    echo "$bad" >&2
    echo "line-endings: $(wc -l <<< "$bad") file(s) break the rule; convert them in a commit that changes nothing else," \
        "as git diff --ignore-cr-at-eol shows" >&2
    exit 1
fi
echo "line-endings: every tracked file keeps the rule"
