#!/bin/bash
# One-shot helper for the patched amdgpu on the BC-250: gets the SteamOS kernel's amdgpu sources, builds
# the module with bc250-amdgpu.patch and installs it. Run on the board as the user "deck" (not root);
# sudo is asked only for the install step.
#
# Usage: setup-amdgpu-bc250.sh [build|install|all]       (default: all)
#
# The kernel sources must match `uname -r` exactly. By default the script takes them from Valve's source
# package for the running kernel (linux-neptune-NN-VERSION-REL.src.tar.gz on the package mirror): a ~3.5 GB
# download that holds a bare git repository, from which only drivers/gpu/drm/amd is taken, at the commit
# `uname -r` names (the gHASH part). It needs about 4 GB free in the work folder while it runs.
# Other sources:
#   KERNEL_SRC_DIR=/path/to/linux-tree   a tree that holds drivers/gpu/drm/amd
#   KERNEL_SRC_URL=https://.../x.src.tar.gz   a Valve source package, or a tarball of a kernel tree
set -eu

MODE=${1:-all}
HERE=$(dirname "$(realpath "$0")")
KVER=$(uname -r)
W=${AMDGPU_BC250_WORKDIR:-$HOME/dev/amdgpu-bc250}
MIRROR=${KERNEL_SRC_MIRROR:-https://steamdeck-packages.steamos.cloud/archlinux-mirror/sources/jupiter-main}
NEED_KB=$((4 * 1024 * 1024))

die() { echo "ERROR: $*" >&2; exit 1; }

[ "$(id -u)" != 0 ] || die "run as the normal user, not root (sudo is used for the install step only)"

check_tools() {
	for t in make gcc patch zstd strip curl tar find git; do
		command -v "$t" >/dev/null || die "missing tool: $t (SteamOS: sudo steamos-readonly disable; sudo pacman -S --needed base-devel zstd curl git; sudo steamos-readonly enable)"
	done
	[ -d "/usr/lib/modules/$KVER/build" ] ||
		die "no kernel headers at /usr/lib/modules/$KVER/build (SteamOS: sudo steamos-readonly disable; sudo pacman -S linux-neptune-headers; sudo steamos-readonly enable)"
}

# Valve's source package: PKGBUILD + a bare git repository. Takes drivers/gpu/drm/amd at the running kernel's commit.
from_source_package() {
	local url=$1 dl=$W/download
	local avail
	avail=$(df -Pk "$W" | awk 'NR==2 {print $4}')
	[ "$avail" -ge "$NEED_KB" ] || die "less than 4 GB free in $W ($((avail / 1024)) MB)"
	rm -rf "$dl"; mkdir -p "$dl"
	echo "downloading and unpacking $url (about 3.5 GB, a few minutes) ..."
	curl -fL "$url" | tar xz -C "$dl"
	local repo
	repo=$(find "$dl" -maxdepth 3 -type d -name 'archlinux-linux-neptune' -print -quit)
	[ -n "$repo" ] || die "no git repository (archlinux-linux-neptune) in $url"
	local ref="" hash tag
	hash=$(echo "$KVER" | sed -n -E 's/.*-g([0-9a-f]{7,}).*/\1/p')
	if [ -n "$hash" ] && git --git-dir="$repo" cat-file -e "$hash^{commit}" 2>/dev/null; then
		ref=$hash
	else
		tag=$(sed -n 's/^_tag=//p' "$(dirname "$repo")/PKGBUILD" | head -1)
		[ -n "$tag" ] && git --git-dir="$repo" rev-parse -q --verify "$tag^{commit}" >/dev/null && ref=$tag
	fi
	[ -n "$ref" ] || die "the package has neither commit ${hash:-?} (from uname -r) nor the tag named in its PKGBUILD"
	echo "taking drivers/gpu/drm/amd at $ref"
	mkdir -p "$W/src.orig"
	git --git-dir="$repo" archive "$ref" drivers/gpu/drm/amd | tar x -C "$W/src.orig"
	rm -rf "$dl"
}

# A tarball of a kernel tree.
from_tree_tarball() {
	local url=$1 dl=$W/download
	rm -rf "$dl"; mkdir -p "$dl/tree"
	echo "downloading $url"
	curl -fL -o "$dl/src.tar" "$url"
	tar -xf "$dl/src.tar" -C "$dl/tree"
	from_tree "$(find "$dl/tree" -maxdepth 4 -type d -path '*/drivers/gpu/drm/amd' -print -quit | sed 's#/drivers/gpu/drm/amd$##')"
	rm -rf "$dl"
}

from_tree() {
	local tree=$1
	[ -n "$tree" ] && [ -d "$tree/drivers/gpu/drm/amd" ] || die "no drivers/gpu/drm/amd in ${tree:-the sources}"
	if [ -f "$tree/Makefile" ]; then
		local v p s
		v=$(sed -n 's/^VERSION = //p' "$tree/Makefile"); p=$(sed -n 's/^PATCHLEVEL = //p' "$tree/Makefile"); s=$(sed -n 's/^SUBLEVEL = //p' "$tree/Makefile")
		case "$KVER" in "$v.$p.$s"*) ;; *) die "the sources are $v.$p.$s, the running kernel is $KVER" ;; esac
	fi
	mkdir -p "$W/src.orig/drivers/gpu/drm"
	cp -r "$tree/drivers/gpu/drm/amd" "$W/src.orig/drivers/gpu/drm/"
}

fetch_sources() {
	if [ -d "$W/src.orig/drivers/gpu/drm/amd" ]; then
		echo "sources already in $W/src.orig, kept (delete that folder to fetch again)"
		return
	fi
	mkdir -p "$W"
	if [ -n "${KERNEL_SRC_DIR:-}" ]; then
		from_tree "$KERNEL_SRC_DIR"
	else
		local url=${KERNEL_SRC_URL:-}
		if [ -z "$url" ]; then
			# 7.2.7-valve2-1-neptune-72-g3150458... -> linux-neptune-72-7.2.7.valve2-1.src.tar.gz
			local ver rel nn
			ver=$(echo "$KVER" | sed -n -E 's/^([0-9.]+)-(valve[0-9]+)-([0-9]+)-neptune-([0-9]+)-.*/\1.\2/p')
			rel=$(echo "$KVER" | sed -n -E 's/^([0-9.]+)-(valve[0-9]+)-([0-9]+)-neptune-([0-9]+)-.*/\3/p')
			nn=$(echo "$KVER" | sed -n -E 's/^([0-9.]+)-(valve[0-9]+)-([0-9]+)-neptune-([0-9]+)-.*/\4/p')
			[ -n "$ver" ] || die "cannot read the kernel version from '$KVER'; give KERNEL_SRC_URL or KERNEL_SRC_DIR"
			url=$MIRROR/linux-neptune-$nn-$ver-$rel.src.tar.gz
			curl -fsI "$url" >/dev/null || die "no source package at $url
Give the sources of exactly $KVER with KERNEL_SRC_URL=... or KERNEL_SRC_DIR=..."
		fi
		case "$url" in
			*.src.tar.gz) from_source_package "$url" ;;
			*) from_tree_tarball "$url" ;;
		esac
	fi
	echo "sources in $W/src.orig"
}

build() {
	check_tools
	fetch_sources
	"$HERE/build-amdgpu-bc250.sh" "$W"
}

install_module() {
	[ -f "$W/amdgpu.ko.zst" ] || die "no $W/amdgpu.ko.zst, run: $0 build"
	AMDGPU_BC250_MODULE="$W/amdgpu.ko.zst" sudo -E "$HERE/install-amdgpu-bc250.sh"
	echo
	echo "Installed. Reboot now:      sudo systemctl reboot"
	echo "Then check (expect 2 0 2 0): cat /sys/module/amdgpu/parameters/{sched_policy,bc250_tlb_flush_type,bc250_gfx_queues,vm_update_mode}"
	echo "Roll back with:              sudo $HERE/uninstall-amdgpu-bc250.sh && sudo systemctl reboot"
}

case "$MODE" in
	build) build ;;
	install) install_module ;;
	all) build; install_module ;;
	*) die "usage: $0 [build|install|all]" ;;
esac
