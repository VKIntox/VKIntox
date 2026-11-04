#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/vkintox-arm64"
rootfs="${VKINTOX_ARM64_ROOTFS:-$cache_dir/ArchLinuxARM-aarch64-latest.tar.gz}"
rootfs_url="${VKINTOX_ARM64_ROOTFS_URL:-https://archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz}"
image="${VKINTOX_ARM64_IMAGE:-localhost/vkintox-arm64-arch}"
library="${VKINTOX_ARM64_LIBRARY:-$repo_root/build-arm64/src/libvkintox.so}"

die() { echo "arm64-smoke: $*" >&2; exit 1; }

command -v podman >/dev/null 2>&1 || die "podman is not installed"
[[ -f "$library" ]] || die "layer library not found: $library"
library="$(cd "$(dirname "$library")" && pwd)/$(basename "$library")"
case "$library" in
    "$repo_root"/*) library_in_container="/src/${library#"$repo_root"/}" ;;
    *) die "layer library must be inside $repo_root" ;;
esac
readelf -h "$library" | grep -q 'Machine:.*AArch64' ||
    die "not an aarch64 binary: $library"

if [[ "$(uname -m)" != "aarch64" ]] && ! grep -qs '^enabled' /proc/sys/fs/binfmt_misc/qemu-aarch64; then
    die "no aarch64 binfmt handler; install qemu-user-static-binfmt"
fi

if ! podman image exists "$image"; then
    mkdir -p "$cache_dir"
    if [[ ! -s "$rootfs" ]]; then
        echo "downloading $rootfs_url"
        curl -fL --retry 3 -o "$rootfs.part" "$rootfs_url"
        mv "$rootfs.part" "$rootfs"
    fi

    echo "importing $rootfs"
    podman import --arch arm64 "$rootfs" "$image"
fi

if ! podman run --rm --platform linux/arm64 "$image" test -x /usr/bin/weston >/dev/null 2>&1; then
    deps="vkintox-arm64-deps-$$"
    echo "installing runtime dependencies in $image"
    podman run --name "$deps" --platform linux/arm64 "$image" /bin/bash -c '
        set -euo pipefail
        if [[ ! -d /etc/pacman.d/gnupg ]]; then
            pacman-key --init
            pacman-key --populate archlinuxarm
        fi
        pacman --disable-sandbox -Syy --noconfirm
        pacman --disable-sandbox -S --noconfirm --needed \
            vulkan-icd-loader \
            vulkan-swrast \
            vulkan-tools \
            libx11 \
            libxi \
            libxtst \
            wayland \
            libxkbcommon \
            glib2 \
            xorg-server-xvfb \
            weston \
            jq
    '
    podman commit "$deps" "$image"
    podman rm "$deps"
fi

podman run --rm --platform linux/arm64 \
    --security-opt label=disable \
    -v "$repo_root:/src" \
    -w /src \
    -e VKINTOX_ARM64_LIBRARY="$library_in_container" \
    "$image" /bin/bash -c '
        set -euo pipefail
        ulimit -c 0

        runtime=/tmp/vkintox-arm64-xdg
        mkdir -p "$runtime"
        chmod 700 "$runtime"
        export XDG_RUNTIME_DIR="$runtime"
        weston --backend=headless --socket=wayland-vkintox \
            --width=800 --height=600 --idle-time=0 \
            --log=/tmp/vkintox-arm64-weston.log &
        weston_pid=$!
        trap "kill $weston_pid 2>/dev/null || true" EXIT

        for _ in $(seq 100); do
            [[ -S "$runtime/wayland-vkintox" ]] && break
            sleep 0.1
        done
        if [[ ! -S "$runtime/wayland-vkintox" ]]; then
            cat /tmp/vkintox-arm64-weston.log >&2
            exit 1
        fi

        unset DISPLAY
        export WAYLAND_DISPLAY=wayland-vkintox
        icd="$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -1)"
        VK_ICD_FILENAMES="$icd" src/tests/smoke_vkcube.sh "$VKINTOX_ARM64_LIBRARY" 30
    '
