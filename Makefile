# Lotus Turbo Challenge 2 (CD32, 1991) — native oracle.
#
# The host is a near-copy of the SWIV-Amiga OCS/AGA implementation: both
# titles use the same WHDLoad conventions, and Lotus 2 is an AGA title that
# runs on the same chipset frame.  MUSASHI_DIR can point anywhere a Musashi
# checkout lives; the third_party/musashi vendored copy is built by default.
#
# Lotus2CD32.slave asks for ~0.5 MB ChipMem ($80000) and ~8.5 MB ExpMem
# ($008800A0 — yes, it's odd, that value lives in the slave; we honor it
# rather than override).  CHIP_SIZE = 0x80000 (SWIV default).
#
# The retail binary lives in original/Lotus2CD32/ — never committed.
# Disk.1 is a RawDIC dump; the host's disk_load() reads it as a flat byte
# image the way WHDLoad's imager does.

MUSASHI_DIR ?= third_party/musashi
MUSASHI = $(MUSASHI_DIR)/m68kcpu.c $(MUSASHI_DIR)/m68kops.c \
	$(MUSASHI_DIR)/m68kdasm.c $(MUSASHI_DIR)/softfloat/softfloat.c
HOST = src/host/amiga.c src/host/whdload.c src/host/rnc2.c
HOST_H = src/host/amiga.h src/host/whdload.h src/host/rnc2.h
# RNC2 decoding is now pure C in src/host/rnc2.c (a faithful port of the
# method-2 unpack path from the public RNC ProPack decompilation); the
# assembled-ProPack detour documented in third_party/rnc/README.md is no
# longer part of the build.
CFLAGS_NATIVE = -DM68K_INSTRUCTION_HOOK=M68K_OPT_SPECIFY_HANDLER \
	-O2 -std=c11 -Wall -Wextra -include src/host/amiga.h \
	-I$(MUSASHI_DIR) -Isrc/host
RAYLIB_FLAGS = -I$(HOME)/.local/include $(HOME)/.local/lib/libraylib.a \
	-lm -lpthread -ldl -lGL -lX11

INSTALL ?= original/Lotus2CD32

ENGINE = src/engine/engine.c src/engine/compositor.c src/engine/road.c \
	src/engine/blitter.c src/engine/car.c src/engine/input.c \
	src/engine/scenery.c
ENGINE_H = src/engine/engine.h src/engine/guest.h src/engine/compositor.h \
	src/engine/blitter.h

all: build/lotus2 build/lotus2_native

build/lotus2: src/host/lotus2_run.c $(HOST) $(HOST_H) $(MUSASHI)
	mkdir -p build
	$(CC) $(CFLAGS_NATIVE) -o $@ src/host/lotus2_run.c $(HOST) $(MUSASHI) -lm

build/lotus2_native: src/engine/lotus2_native.c $(ENGINE) $(ENGINE_H)
	mkdir -p build
	$(CC) -O2 -std=c11 -Wall -Wextra -Isrc/engine -o $@ \
		src/engine/lotus2_native.c $(ENGINE)

# Live viewer: runs the game through the oracle host in a window, with the
# TRACK / GEOMETRY / DISPLAY debug panels over the live guest state.
build/lotus2_view: src/viewer/lotus2_view.c $(HOST) $(HOST_H) $(MUSASHI) \
		src/host/pad.c
	mkdir -p build
	$(CC) $(CFLAGS_NATIVE) -Isrc/engine -o $@ \
		src/viewer/lotus2_view.c $(HOST) src/host/pad.c $(MUSASHI) \
		$(RAYLIB_FLAGS)

# Drive the native engine: no emulator, only ported C, from a race
# snapshot.  Scenery is absent because $21508a is not ported yet.
build/lotus2_drive: src/engine/lotus2_drive.c $(ENGINE) $(ENGINE_H)
	mkdir -p build
	$(CC) -O2 -std=c11 -Wall -Wextra -Isrc/engine -o $@ \
		src/engine/lotus2_drive.c $(ENGINE) $(RAYLIB_FLAGS)

drive: build/lotus2_drive
	./build/lotus2_drive

# The debug viewer opens a course file directly -- no game boot, instant.
view: build/lotus2_view re/pipeline/course_forest.l2c
	./build/lotus2_view --course re/pipeline/course_forest.l2c

# Live session instead: boots the game, drives into a race, then freezes
# so the TRACK / GEOM / DISPLAY pages have real guest state to read.
view-race: build/lotus2_view
	./build/lotus2_view --live --dir $(INSTALL)

# Extract a standalone course from a road snapshot (needs make road-capture).
re/pipeline/course_forest.l2c: tools/course_extract.py \
		re/pipeline/road/st_2_212f16_fast.bin
	python3 tools/course_extract.py \
		re/pipeline/road/st_2_212f16_fast.bin $@ \
		--frame re/pipeline/gate_05600.ppm --name FOREST

course: re/pipeline/course_forest.l2c

# Decode the course table out of a road snapshot (needs make road-capture).
track: tools/track_dump.py
	python3 tools/track_dump.py re/pipeline/road/st_2_212f16_fast.bin \
		--csv re/pipeline/track_forest.csv --map re/pipeline/track_forest.svg

# TRANSLATE gates: capture a coherent oracle frame + RAM pair (title at
# frame 2000, mid-race at frame 5600), render the same frames natively
# (verbs + per-line copper compositor), and demand (a) word-exact verb
# outputs and (b) the native 320x200 frames to appear verbatim inside the
# oracle's raster.
gate-capture: build/lotus2
	./build/lotus2 --dir $(INSTALL) --frames 2000 \
		--ppm re/pipeline/gate_02000.ppm \
		--dump-file 0 0x80000 re/pipeline/gate_chip_02000.bin \
		--dump-file 0x208000 0x4000 re/pipeline/gate_base_02000.bin
	./build/lotus2 --dir $(INSTALL) --frames 5600 \
		--fire-from 2100 --fire-period 100 \
		--ppm re/pipeline/gate_05600.ppm \
		--dump-file 0 0x80000 re/pipeline/gate_chip_05600.bin \
		--dump-file 0x208000 0x4000 re/pipeline/gate_base_05600.bin

# Road-stage snapshots: entry RAM images for each BSR in the racing render
# chain at $212f12, so every ported stage is a proven input->output
# transform.  Needs a gameplay frame, so it drives the pad like the gates.
road-capture: build/lotus2
	mkdir -p re/pipeline/road
	SWIV_SNAP_PCS=212f12,212f16,212f1a,212f1e,212f22,212f26,212f2a,212f2e,212f32 \
	SWIV_SNAP_FROM=5599 SWIV_SNAP_MAX=9 \
	SWIV_SNAP_PREFIX=re/pipeline/road/st_ \
	./build/lotus2 --dir $(INSTALL) --frames 5601 \
		--fire-from 2100 --fire-period 100

render-gate: build/lotus2_native
	./build/lotus2_native --verify-road
	./build/lotus2_native --verify-verbs --render build/native_02000
	python3 tools/gate_compare.py re/pipeline/gate_02000.ppm \
		build/native_02000.$$(printf %06x 0x7f5f0).ppm || \
	python3 tools/gate_compare.py re/pipeline/gate_02000.ppm \
		build/native_02000.$$(printf %06x 0x7fedc).ppm
	./build/lotus2_native --chip re/pipeline/gate_chip_05600.bin \
		--base re/pipeline/gate_base_05600.bin \
		--coplist 0x7ed0c --render build/native_05600.ppm
	python3 tools/gate_compare.py re/pipeline/gate_05600.ppm \
		build/native_05600.ppm

# Per-instruction register trace of ONE call of a routine.  Porting from
# a disassembly guesses operand sizes; porting from this reads them off.
#   make statelog                 # ~11s, covers the frame-5600 race pass
#   make trace PC=21508a          # annotated listing of that routine
STATELOG ?= re/pipeline/statelog_race.bin
statelog: build/lotus2
	SWIV_STATELOG=$(STATELOG) SWIV_STATELOG_FROM=5600 \
	SWIV_STATELOG_MAX=400000 \
	./build/lotus2 --dir $(INSTALL) --frames 5602 \
		--fire-from 2100 --fire-period 100 >/dev/null

trace:
	@test -n "$(PC)" || { echo "usage: make trace PC=21508a"; exit 2; }
	python3 tools/trace_call.py $(STATELOG) $(PC) $(TRACEOPTS)

# Which PCs the game ever reaches on a full run into a race.  Triage: a
# routine in the call graph that never appears here is not on the path to
# a playable FOREST course, however alarming the call graph looks.
pcset: build/lotus2
	SWIV_PCSET=re/pipeline/pcset_race.txt \
	./build/lotus2 --dir $(INSTALL) --frames 9000 \
		--fire-from 2100 --fire-period 100 >/dev/null

native: build/lotus2

# Short boot: the slave has to at least start executing after install + patch.
boot-test: build/lotus2
	./build/lotus2 --dir $(INSTALL) --frames 300

# Chipset self tests: blitter minterms, display window, input, Paula.
selftest: build/lotus2
	./build/lotus2 --selftest

# Long oracle run with periodic screenshots + PC set.  Set
# SWIV_PCSET=re/pipeline/pcset.txt before calling to capture the executed PCs.
oracle: build/lotus2
	./build/lotus2 --dir $(INSTALL) --frames 600 \
		--ppm-seq re/pipeline/oracle_ --ppm-every 50 --trace

test: selftest boot-test

clean:
	rm -rf build

.PHONY: statelog trace pcset drive all native boot-test selftest oracle test clean gate-capture road-capture render-gate view view-race track course
