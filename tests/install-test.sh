#!/bin/sh
# Tests for install.sh. They need no monitor and no network.
# Each test runs the real script with its own empty home folder, and takes the source from a
# tarball of this working tree in place of the download.
# Run from the repository root: sh tests/install-test.sh
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT="$ROOT/install.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

checks=0
failures=0

check() { # check <what is being checked> <command...>
	what=$1
	shift
	checks=$((checks + 1))
	if ! "$@"; then
		failures=$((failures + 1))
		printf 'FAIL %s: %s\n' "$test_name" "$what"
	fi
}

# The checks read the files that the last run_install wrote.
said() { grep -qF -- "$1" "$run/out"; }
not_said() { ! grep -qF -- "$1" "$run/out"; }
runs() { "$1" help >/dev/null 2>&1; }
exited() { [ "$(cat "$run/status")" = "$1" ]; }
temp_is_empty() { [ -z "$(ls -A "$run/tmp")" ]; }
home_is_empty() { [ -z "$(ls -A "$run/home")" ]; }

# Packs a folder the way GitHub does: everything sits inside one top folder.
pack() { # pack <folder> <tarball>
	tar -czf "$2" -C "$(dirname "$1")" "$(basename "$1")"
}

mkdir -p "$WORK/good/monitorctl-main"
cp -R "$ROOT/Makefile" "$ROOT/src" "$WORK/good/monitorctl-main/"
pack "$WORK/good/monitorctl-main" "$WORK/good.tar.gz"

mkdir -p "$WORK/broken/monitorctl-main"
cp -R "$ROOT/Makefile" "$ROOT/src" "$WORK/broken/monitorctl-main/"
echo 'this is not C' >>"$WORK/broken/monitorctl-main/src/main.c"
pack "$WORK/broken/monitorctl-main" "$WORK/broken.tar.gz"

mkdir -p "$WORK/other/something-else"
echo 'hello' >"$WORK/other/something-else/notes.txt"
pack "$WORK/other/something-else" "$WORK/other.tar.gz"

# Starts a test: a fresh home folder, a fresh temporary folder, and a folder of pretend commands
# that is searched before the real ones.
begin() {
	test_name=$1
	run="$WORK/run-$checks-$failures-$(echo "$1" | tr ' ' '-')"
	mkdir -p "$run/home" "$run/tmp" "$run/stubs"
}

# Replaces one command for this test with a script that prints a line and exits with a status.
stub() { # stub <command> <output> <exit status>
	printf '#!/bin/sh\necho "%s"\nexit %s\n' "$2" "$3" >"$run/stubs/$1"
	chmod +x "$run/stubs/$1"
}

# Runs the installer. Extra arguments are NAME=value settings for the run.
run_install() {
	env HOME="$run/home" TMPDIR="$run/tmp" PATH="$run/stubs:$PATH" "$@" sh "$SCRIPT" >"$run/out" 2>&1
	echo $? >"$run/status"
}

begin "installs from the source"
run_install MONITORCTL_SOURCE_TARBALL="$WORK/good.tar.gz"
check "exits 0" exited 0
check "the program is in ~/.local/bin" test -x "$run/home/.local/bin/monitorctl"
check "the program runs" runs "$run/home/.local/bin/monitorctl"
check "says where it was installed" said "$run/home/.local/bin/monitorctl"
check "points at setup" said "monitorctl setup"
check "says the folder is not on the PATH" said "is not on your PATH"
check "gives the line that adds it" said "export PATH=\"$run/home/.local/bin:\$PATH\""
check "leaves no temporary files" temp_is_empty
check "does not write shell startup files" test ! -e "$run/home/.zshrc"

begin "installs to PREFIX"
mkdir -p "$run/prefix/bin"
run_install MONITORCTL_SOURCE_TARBALL="$WORK/good.tar.gz" PREFIX="$run/prefix" PATH="$run/prefix/bin:$PATH"
check "exits 0" exited 0
check "the program is in PREFIX/bin" test -x "$run/prefix/bin/monitorctl"
check "nothing is written to the home folder" home_is_empty
check "says nothing about the PATH when the folder is on it" not_said "PATH"

begin "replaces an older install"
mkdir -p "$run/home/.local/bin"
echo 'old' >"$run/home/.local/bin/monitorctl"
run_install MONITORCTL_SOURCE_TARBALL="$WORK/good.tar.gz"
check "exits 0" exited 0
check "the new program runs" runs "$run/home/.local/bin/monitorctl"

begin "refuses a system that is not macOS"
stub uname Linux 0
run_install MONITORCTL_SOURCE_TARBALL="$WORK/good.tar.gz"
check "exits 1" exited 1
check "says it needs macOS" said "only on macOS"
check "installs nothing" home_is_empty

begin "refuses an Intel Mac"
printf '#!/bin/sh\nif [ "$1" = "-m" ]; then echo x86_64; else echo Darwin; fi\n' >"$run/stubs/uname"
chmod +x "$run/stubs/uname"
run_install MONITORCTL_SOURCE_TARBALL="$WORK/good.tar.gz"
check "exits 1" exited 1
check "says it needs Apple Silicon" said "Apple Silicon"
check "says what this Mac reported" said "x86_64"
check "installs nothing" home_is_empty

begin "explains missing developer tools"
stub xcode-select "xcode-select: error: unable to get active developer directory" 2
run_install MONITORCTL_SOURCE_TARBALL="$WORK/good.tar.gz"
check "exits 1" exited 1
check "gives the command that installs them" said "xcode-select --install"
check "installs nothing" home_is_empty

begin "stops when the download fails"
run_install MONITORCTL_SOURCE_URL="https://127.0.0.1:9/monitorctl.tar.gz"
check "exits 1" exited 1
check "says the download failed" said "could not be downloaded"
check "names the address" said "https://127.0.0.1:9/monitorctl.tar.gz"
check "installs nothing" home_is_empty
check "leaves no temporary files" temp_is_empty

begin "downloads over HTTPS only"
run_install MONITORCTL_SOURCE_URL="file://$WORK/good.tar.gz"
check "exits 1" exited 1
check "says the download failed" said "could not be downloaded"
check "installs nothing" home_is_empty

begin "refuses a download that is not monitorctl"
run_install MONITORCTL_SOURCE_TARBALL="$WORK/other.tar.gz"
check "exits 1" exited 1
check "says the download is not the source" said "is not the monitorctl source"
check "installs nothing" home_is_empty
check "leaves no temporary files" temp_is_empty

begin "refuses a tarball that is missing"
run_install MONITORCTL_SOURCE_TARBALL="$WORK/nowhere.tar.gz"
check "exits 1" exited 1
check "names the missing file" said "$WORK/nowhere.tar.gz"
check "installs nothing" home_is_empty

begin "keeps the old install when the build fails"
mkdir -p "$run/home/.local/bin"
echo 'old' >"$run/home/.local/bin/monitorctl"
run_install MONITORCTL_SOURCE_TARBALL="$WORK/broken.tar.gz"
check "exits 1" exited 1
check "says the build failed" said "could not be built"
check "shows what the compiler said" said "error"
check "the old program is untouched" test "$(cat "$run/home/.local/bin/monitorctl")" = "old"
check "leaves no temporary files" temp_is_empty

# The usual way to run the script is through a pipe, where the script is the shell's input.
begin "runs from a pipe"
env HOME="$run/home" TMPDIR="$run/tmp" MONITORCTL_SOURCE_TARBALL="$WORK/good.tar.gz" sh <"$SCRIPT" >"$run/out" 2>&1
echo $? >"$run/status"
check "exits 0" exited 0
check "the program runs" runs "$run/home/.local/bin/monitorctl"
check "prints the last step" said "monitorctl setup"

# A download can stop part way. The script must then do nothing at all, so its work happens in
# one function that the last line calls.
begin "a cut-off download runs nothing"
check "the script exists" test -f "$SCRIPT"
for lines in 20 60 100; do
	head -n "$lines" "$SCRIPT" >"$run/cut.sh" 2>/dev/null
	env HOME="$run/home" TMPDIR="$run/tmp" MONITORCTL_SOURCE_TARBALL="$WORK/good.tar.gz" sh "$run/cut.sh" >"$run/out" 2>&1
	check "the first $lines lines install nothing" home_is_empty
	check "the first $lines lines leave no temporary files" temp_is_empty
done
sed '$d' "$SCRIPT" >"$run/cut.sh" 2>/dev/null
env HOME="$run/home" TMPDIR="$run/tmp" MONITORCTL_SOURCE_TARBALL="$WORK/good.tar.gz" sh "$run/cut.sh" >"$run/out" 2>&1
check "everything but the last line installs nothing" home_is_empty

if [ "$failures" -eq 0 ]; then
	echo "All $checks install checks passed."
else
	echo "$failures of $checks install checks failed."
	exit 1
fi
