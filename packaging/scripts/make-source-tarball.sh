#!/usr/bin/env bash
# build a vendored source tarball for distro packaging
#
# the vendor and shader trees are submodules, so git archive alone leaves the
# build without imgui, reshade or the built-in shaders. this copies the working
# tree instead, behind an explicit ignore list.
set -euo pipefail

usage() {
    echo "usage: $0 VERSION [OUTPUT_DIR]" >&2
    exit 2
}

[[ $# -ge 1 && $# -le 2 ]] || usage

version="${1#v}"
outdir="${2:-build}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
prefix="vkintox-$version"

[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
    echo "version must look like 0.2.0, got: $version" >&2
    exit 2
}

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

mkdir -p "$stage/$prefix"

tar -C "$repo_root" -chf - \
    --exclude='./.git' \
    --exclude='./.pre-commit-config.yaml' \
    --exclude='./.flatpak-builder' \
    --exclude='./build' \
    --exclude='./build-*' \
    --exclude='./.cache' \
    --exclude='./imgui.ini' \
    . | tar -C "$stage/$prefix" -xf -

mkdir -p "$outdir"
tarball="$outdir/$prefix.tar.xz"
tar -C "$stage" -cJf "$tarball" "$prefix"
sha256sum "$tarball"
