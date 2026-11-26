#!/bin/sh
# Installs monitorctl on an Apple Silicon Mac:
#
#   curl -fsSL https://felthosfoundry.com/monitorctl/install.sh | sh
#
# The script downloads the source from GitHub, builds it on this Mac, and copies the program to
# ~/.local/bin. It never asks for a password, and it never edits your shell's startup files.
#
# Settings, all optional:
#   PREFIX                     install to $PREFIX/bin in place of ~/.local/bin
#   MONITORCTL_SOURCE_TARBALL  build from this source tarball in place of downloading one
#   MONITORCTL_SOURCE_URL      download the source tarball from this HTTPS address
#
# All the work happens in main, which the last line calls. A download of this script that stops
# part way therefore runs nothing.
set -eu

DEFAULT_SOURCE_URL="https://github.com/felthosfoundry/monitorctl/archive/refs/heads/main.tar.gz"

say() { printf '%s\n' "$*"; }

fail() {
	printf 'monitorctl was not installed. %s\n' "$*" >&2
	exit 1
}

# Stops unless this is an Apple Silicon Mac with a compiler.
check_mac() {
	system=$(uname -s)
	[ "$system" = "Darwin" ] || fail "It runs only on macOS, and this system is $system."
	chip=$(uname -m)
	[ "$chip" = "arm64" ] || fail "It needs an Apple Silicon Mac, and this one reports $chip."
	xcode-select -p >/dev/null 2>&1 || fail "The Xcode command-line tools are missing. Install them with this command, then run the installer again:

  xcode-select --install"
}

# Puts the source in $work/source.
fetch_source() {
	if [ -n "${MONITORCTL_SOURCE_TARBALL:-}" ]; then
		tarball=$MONITORCTL_SOURCE_TARBALL
		[ -f "$tarball" ] || fail "The source tarball $tarball does not exist."
	else
		url=${MONITORCTL_SOURCE_URL:-$DEFAULT_SOURCE_URL}
		tarball=$work/source.tar.gz
		say "Downloading the source from $url"
		if ! curl --disable --fail --silent --show-error --location \
			--proto '=https' --proto-redir '=https' --connect-timeout 15 --max-time 120 \
			--output "$tarball" "$url"; then
			fail "The source could not be downloaded from $url"
		fi
	fi

	# GitHub packs the source inside one top folder, which is dropped here.
	mkdir "$work/source"
	tar -xzf "$tarball" -C "$work/source" --strip-components 1 2>/dev/null || true
	[ -f "$work/source/Makefile" ] && [ -f "$work/source/src/main.c" ] ||
		fail "$tarball is not the monitorctl source."
}

# Builds the program and checks that it runs. Nothing outside $work has changed yet, so a
# failure here leaves an older install as it was.
build() {
	say "Building monitorctl. This takes a few seconds."
	if ! make -C "$work/source" >"$work/build.log" 2>&1; then
		cat "$work/build.log" >&2
		fail "It could not be built. The compiler's messages are above."
	fi
	"$work/source/build/monitorctl" help >/dev/null 2>&1 || fail "The program was built, and it does not run."
}

# Copies the program to $bin. The copy gets a temporary name first and is then renamed, so an
# older install is replaced in one step.
install_program() {
	mkdir -p "$bin" 2>/dev/null || fail "The folder $bin could not be created. Set PREFIX to choose another place."
	if ! install -m 755 "$work/source/build/monitorctl" "$bin/.monitorctl.new" 2>/dev/null ||
		! mv -f "$bin/.monitorctl.new" "$bin/monitorctl" 2>/dev/null; then
		rm -f "$bin/.monitorctl.new"
		fail "The folder $bin could not be written to. Set PREFIX to choose another place."
	fi
}

# Says where the program is, and what to do when the shell cannot find it by name.
report() {
	say ""
	say "monitorctl is installed at $bin/monitorctl"
	case ":$PATH:" in
	*":$bin:"*) ;;
	*)
		say ""
		say "$bin is not on your PATH, so the shell cannot find monitorctl by name yet."
		say "For zsh, add the folder with this command, then open a new terminal window:"
		say ""
		say "  echo 'export PATH=\"$bin:\$PATH\"' >> ~/.zshrc"
		;;
	esac
	say ""
	say "Connect an external monitor, then find out what it supports:"
	say ""
	say "  monitorctl setup"
}

main() {
	check_mac
	bin=${PREFIX:-$HOME/.local}/bin

	work=$(mktemp -d "${TMPDIR:-/tmp}/monitorctl-install.XXXXXX")
	trap 'rm -rf "$work"' EXIT
	trap 'exit 1' HUP INT TERM

	fetch_source
	build
	install_program
	report
}

# The script itself may be arriving on standard input, so nothing it runs may read from there.
main "$@" </dev/null
