# Convenience wrapper so the usual commands work from the repo root.
# The real build is CMake, out-of-tree in build/.
#
#   make                 configure (first time) and build
#   sudo make install    install what was built to /usr/local
#   make install PREFIX=$HOME/.local     install for the current user
#
# `install` deliberately does not build, so running it under sudo never
# leaves root-owned files in build/.

PREFIX ?= /usr/local
BUILD  ?= build
JOBS   ?= $(shell nproc)

# libadwaitamm built from source usually lands in /usr/local.
export PKG_CONFIG_PATH := /usr/local/lib64/pkgconfig:/usr/local/lib/pkgconfig:$(PKG_CONFIG_PATH)

.PHONY: all install uninstall clean

all:
	cmake -S . -B $(BUILD)
	cmake --build $(BUILD) -j$(JOBS)

install:
	@test -x $(BUILD)/bin/dreamstation || { echo "Nothing built yet: run 'make' first (without sudo)."; exit 1; }
	cmake --install $(BUILD) --prefix $(PREFIX) --strip

uninstall:
	@test -f $(BUILD)/install_manifest.txt || { echo "No install record in $(BUILD)."; exit 1; }
	xargs rm -f < $(BUILD)/install_manifest.txt

clean:
	cmake --build $(BUILD) --target clean
