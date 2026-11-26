# The linker that ships with Xcode can be older than the newest SDK on the machine, so the SDK is
# taken from the same toolchain as the compiler.
SDK := $(shell xcrun --sdk macosx --show-sdk-path)
CC := xcrun clang
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -isysroot $(SDK)
PREFIX ?= $(HOME)/.local

CORE := src/ddc.c src/controls.c src/cli.c src/profile.c src/run.c
HEADERS := $(wildcard src/*.h)

all: build/monitorctl

MAC := src/transport_mac.c src/info_mac.c src/database_curl.c src/ask_tty.c

build/monitorctl: $(CORE) $(MAC) src/main.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(CORE) $(MAC) src/main.c -framework IOKit -framework CoreFoundation -framework CoreGraphics

build/tests: $(CORE) tests/tests.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(CORE) tests/tests.c

test: build/tests
	./build/tests
	sh tests/install-test.sh

install: build/monitorctl
	install -d $(PREFIX)/bin
	install -m 755 build/monitorctl $(PREFIX)/bin/monitorctl

.PHONY: all test install
