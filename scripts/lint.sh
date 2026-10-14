#!/usr/bin/env bash
# lint.sh: lint the shell scripts, the workflows and the Dockerfiles (#174).
#
# Each linter runs from its official image, pinned by digest, so what checks the code
# changes only when someone changes a digest here:
#   - shellcheck, over every tracked shell script;
#   - actionlint, over the workflows, which also runs shellcheck on their run: blocks;
#   - hadolint, over the Dockerfiles.
# The containers get a copy of the tracked files, read-only, with no network and no
# capabilities, so the checkout is neither relabelled nor reachable from them.
#
# CI runs this on every push (.github/workflows/format.yml). Run it the same way:
#   scripts/lint.sh
# It needs podman. The first run pulls the images from docker.io, about 145 MB; later
# runs use the local copies.
#
# To update a linter, pull its new tag and put the digest of the image's index here:
# podman manifest inspect lists the platform manifests the index holds, and the image's
# other repo digest is the index's.
#
# Exit codes: 0 = clean, 1 = a linter found something, 2 = usage or setup error

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHELLCHECK=docker.io/koalaman/shellcheck:v0.11.0@sha256:61862eba1fcf09a484ebcc6feea46f1782532571a34ed51fedf90dd25f925a8d
ACTIONLINT=docker.io/rhysd/actionlint:1.7.12@sha256:b1934ee5f1c509618f2508e6eb47ee0d3520686341fec936f3b79331f9315667
HADOLINT=docker.io/hadolint/hadolint:v2.15.1@sha256:32dac94127fd60b7b7e3fbfc65e1383b9b5e25c9bfd7b8536de7a539fe68a12d

case "${1:-}" in
    "") ;;
    -h|--help)
        sed -n '2,/^$/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
        exit 0 ;;
    *)
        echo "lint: unknown argument '$1' (usage: $0 [--help])" >&2
        exit 2 ;;
esac
command -v podman >/dev/null 2>&1 || { echo "lint: podman is not installed" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK:?}"' EXIT
cd "$ROOT" || exit 2
git ls-files -z | xargs -0 cp --parents -t "$WORK" || { echo "lint: couldn't copy the tracked files" >&2; exit 2; }
# actionlint's image runs as a user of its own, which has to be able to read the copy
chmod -R a+rX "$WORK"

failed=0
setup=0
# Runs an image over the copy, and records whether it found something or couldn't run
lint() {
    local rc=0
    podman run --rm --network=none --read-only --cap-drop=all --security-opt=no-new-privileges \
        -v "$WORK:/mnt:ro,Z" -w /mnt "$@" || rc=$?
    if (( rc >= 125 )); then
        echo "lint: podman couldn't run $1 (exit $rc)" >&2
        setup=1
    elif (( rc != 0 )); then
        failed=1
    fi
}

mapfile -t scripts < <(git ls-files '*.sh' scripts/hooks/pre-push)
echo "=== shellcheck: ${#scripts[@]} scripts"
lint "$SHELLCHECK" "${scripts[@]}"

mapfile -t workflows < <(git ls-files '.github/workflows/*.yml')
echo "=== actionlint: ${#workflows[@]} workflows"
# The copy isn't a git repository, so actionlint is given its configuration and the workflows by name
lint "$ACTIONLINT" -config-file .github/actionlint.yaml "${workflows[@]}"

mapfile -t dockerfiles < <(git ls-files 'docker/Dockerfile.*')
echo "=== hadolint: ${#dockerfiles[@]} Dockerfiles"
lint "$HADOLINT" hadolint --no-color "${dockerfiles[@]}"

if (( setup )); then
    exit 2
fi
if (( failed )); then
    echo "lint: FAILED" >&2
    exit 1
fi
echo "lint: clean"
