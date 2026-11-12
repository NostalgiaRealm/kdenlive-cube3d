# SPDX-License-Identifier: GPL-2.0-or-later
#
# cube3d - 3D cube effect and transition for Kdenlive (frei0r plugins)
#
#   make            build the two plugins and the Kdenlive XML into build/
#   make check      run the renderer's self-test
#   make install    copy everything into your home directory (see install.sh)
#
# Needs a C compiler, make and python3 (which writes the Kdenlive XML); MLT and
# frei0r development packages are not required.
#
# Build on the oldest system you have at hand: the plugins must load into the
# C library inside Kdenlive's Flatpak, and a plugin built against a newer one
# is skipped by MLT without any message (install.sh checks for that).

CC      ?= gcc
CFLAGS  ?= -O3
WARN     = -Wall -Wextra -Wshadow
# -fno-math-errno: sqrt() and friends become plain instructions instead of calls
# into libm, whose symbol versions change between C library releases.
ALL_CFLAGS = -std=gnu11 -fPIC -fvisibility=hidden -fno-math-errno -pthread $(WARN) -Iinclude $(CFLAGS)
LDLIBS   = -lm -ldl -lpthread
LDFLAGS += -Wl,-z,defs -Wl,--as-needed

SRC  = src/cube3d.c src/c3d_render.c src/c3d_media.c
HDR  = src/c3d_render.h src/c3d_media.h include/frei0r.h

all: build/cube3d.so build/cube3d_transition.so build/kdenlive/.stamp

build:
	mkdir -p build

build/cube3d.so: $(SRC) $(HDR) | build
	$(CC) $(ALL_CFLAGS) -shared -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

build/cube3d_transition.so: $(SRC) $(HDR) | build
	$(CC) $(ALL_CFLAGS) -DCUBE3D_MIXER -shared -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

build/kdenlive/.stamp: tools/gen_kdenlive_xml.py | build
	python3 tools/gen_kdenlive_xml.py build/kdenlive
	touch $@

build/render_test: tests/render_test.c src/c3d_render.c src/c3d_render.h | build
	$(CC) -std=gnu11 -O2 -g -pthread $(WARN) -o $@ tests/render_test.c src/c3d_render.c -lm

check: build/render_test
	mkdir -p build/test-out
	build/render_test build/test-out

install: all
	sh ./install.sh

clean:
	rm -rf build

.PHONY: all check install clean
