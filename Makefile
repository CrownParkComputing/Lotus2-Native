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
HOST_H = src/host/amiga.h src/host/whdload.h src/host/rnc2.h src/host/cpu.h
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

# Defined here, not next to their rules: make expands a rule's
# prerequisites when it READS the rule, so a variable defined later in the
# file is empty in any earlier prerequisite list.
RECOMP_SRC = src/recomp/lotus2_recomp.c
DECODE_IMAGE = re/pipeline/decode.bin

ENGINE = src/engine/engine.c src/engine/compositor.c src/engine/road.c \
	src/engine/blitter.c src/engine/car.c src/engine/input.c \
	src/engine/scenery.c
ENGINE_H = src/engine/engine.h src/engine/guest.h src/engine/compositor.h \
	src/engine/blitter.h

# Hand-written native C standing in for the game's own instructions.
# Adding a routine here is what turns recompiled 68000 into a native
# implementation; make frame-gate decides whether it may stay.
NATIVE_OVR = src/host/native_overrides.c $(ENGINE)

all: build/lotus2 build/lotus2_native

# The ORACLE: Musashi behind cpu.h.  Its only job from here on is to
# produce the snapshot pairs and reference frames the native build is
# judged against.
build/lotus2: src/host/lotus2_run.c $(HOST) $(HOST_H) src/host/cpu_musashi.c $(MUSASHI)
	mkdir -p build
	$(CC) $(CFLAGS_NATIVE) -o $@ src/host/lotus2_run.c $(HOST) \
		src/host/cpu_musashi.c $(MUSASHI) -lm

# The NATIVE build: the same host and chipset, driven by recompiled C.
# Nothing from third_party/musashi is compiled or linked into it.
CFLAGS_RECOMP = -O2 -std=c11 -Wall -Wextra -include src/host/amiga.h \
	-Isrc/host -Isrc/recomp
# The generated file is one switch with tens of thousands of cases; -O2
# spends minutes on it for no measurable gain, so it is compiled apart at
# -O1 and linked in.
build/lotus2_recomp.o: $(RECOMP_SRC) src/recomp/m68krt.h
	mkdir -p build
	$(CC) -O1 -std=c11 -w -Isrc/recomp -c -o $@ $(RECOMP_SRC)

build/lotus2_recomp: src/host/lotus2_run.c $(HOST) $(HOST_H) \
		src/host/cpu_recomp.c src/recomp/m68krt.h $(NATIVE_OVR) \
		$(ENGINE_H) build/lotus2_recomp.o
	mkdir -p build
	$(CC) $(CFLAGS_RECOMP) -Isrc/engine -o $@ src/host/lotus2_run.c \
		$(HOST) src/host/cpu_recomp.c $(NATIVE_OVR) \
		build/lotus2_recomp.o -lm

# Proof that no emulator is linked in: the native binary must contain no
# Musashi symbols at all.
no-musashi: build/lotus2_recomp
	@if nm build/lotus2_recomp 2>/dev/null | grep -qiE ' (m68ki_|m68k_op_|m68k_execute|m68k_init)'; then \
		echo "FAIL: Musashi symbols present in build/lotus2_recomp"; \
		nm build/lotus2_recomp | grep -iE ' (m68ki_|m68k_op_)' | head; \
		exit 1; \
	else echo "no-musashi: build/lotus2_recomp contains no emulator"; fi

build/lotus2_native: src/engine/lotus2_native.c $(ENGINE) $(ENGINE_H)
	mkdir -p build
	$(CC) -O2 -std=c11 -Wall -Wextra -Isrc/engine -o $@ \
		src/engine/lotus2_native.c $(ENGINE)

# PLAY: the game itself, on recompiled C, with no emulator linked in.
# NOT the viewer -- that boots the game only to freeze it once the course
# data lands and defaults to a debug map page, so it was never a way to
# play.  This is the game screen, the pad and the sound, nothing else.
build/lotus2_game: src/host/lotus2_game.c $(HOST) $(HOST_H) \
		src/host/pad.c src/host/bezel.c src/host/bezel.h \
		src/host/cpu_recomp.c src/recomp/m68krt.h \
		$(NATIVE_OVR) $(ENGINE_H) build/lotus2_recomp.o
	mkdir -p build
	$(CC) $(CFLAGS_RECOMP) -Isrc/engine -o $@ src/host/lotus2_game.c \
		$(HOST) src/host/pad.c src/host/bezel.c src/host/cpu_recomp.c \
		$(NATIVE_OVR) build/lotus2_recomp.o $(RAYLIB_FLAGS)

RECORD ?=
# PLAY is the viewer with the bezel on: same front end, same sound, and
# the RE pages a button away instead of a different program.
play: build/lotus2_play
	./build/lotus2_play --live --bezel --dir $(INSTALL)

# The bare front end, without the RE pages.  Still the one the captures
# and the shot-based checks use.
play-min: build/lotus2_game
	./build/lotus2_game --dir $(INSTALL) \
		$(if $(RECORD),--record $(RECORD))

# The same window as `make play`, opened straight on the course preview.
# The guest only runs on the game page, so this does NOT start the game:
# you get the debug pages, and the game begins when you go to it.
debug: build/lotus2_play
	./build/lotus2_play --live --bezel --dir $(INSTALL) --page COURSE

# The debug viewer driven by recompiled C (map/track/geometry pages).
build/lotus2_play: src/viewer/lotus2_view.c $(HOST) $(HOST_H) \
		src/host/pad.c src/host/bezel.c src/host/bezel.h \
		src/host/cpu_recomp.c src/recomp/m68krt.h \
		$(NATIVE_OVR) $(ENGINE_H) build/lotus2_recomp.o
	mkdir -p build
	$(CC) $(CFLAGS_RECOMP) -Isrc/engine -o $@ \
		src/viewer/lotus2_view.c $(HOST) src/host/pad.c \
		src/host/bezel.c src/host/cpu_recomp.c $(NATIVE_OVR) \
		build/lotus2_recomp.o $(RAYLIB_FLAGS)

# Live viewer: runs the game through the oracle host in a window, with the
# TRACK / GEOMETRY / DISPLAY debug panels over the live guest state.
build/lotus2_view: src/viewer/lotus2_view.c $(HOST) $(HOST_H) \
		src/host/cpu_musashi.c $(MUSASHI) src/host/pad.c
	mkdir -p build
	$(CC) $(CFLAGS_NATIVE) -Isrc/engine -o $@ \
		src/viewer/lotus2_view.c $(HOST) src/host/pad.c \
		src/host/cpu_musashi.c $(MUSASHI) $(RAYLIB_FLAGS)

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
	./build/lotus2_view --live --dir $(INSTALL) \
		--fire-from 2100 --fire-period 100

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
# a playable FOREST course, however alarming the call graph looks.  It is
# also the recompiler's input -- only executed code is translated.
pcset: re/pipeline/pcset_race.txt

# ---- route A: static recompilation ----
# tools/m68k2c.py translates every executed instruction to C mechanically.
# Operand widths and register-write rules live in the generator and
# src/recomp/m68krt.h, so they are uniform instead of re-derived per port.
re/pipeline/pcset_race.txt: build/lotus2
	SWIV_PCSET=$@ ./build/lotus2 --dir $(INSTALL) --frames 9000 \
		--fire-from 2100 --fire-period 100 >/dev/null

# Chip RAM is reused: $0723b0-$072670 is loaded from Disk.1 at frame 1,
# executed, then overwritten by data.  In combined.bin (dumped much
# later) 700 of its 704 bytes are something else, and a disassembler
# renders that as 68020 addressing modes and chk.w (A6)+ -- which is what
# data looks like.  So the decode image is assembled PER REGION from a
# chip snapshot taken at the pc where that code actually runs.
re/pipeline/boot/chip_0723ba.bin: build/lotus2
	mkdir -p re/pipeline/boot
	SWIV_SNAP_PCS=723ba SWIV_SNAP_FROM=0 SWIV_SNAP_MAX=1 \
	SWIV_SNAP_PREFIX=re/pipeline/boot/snap_ \
	./build/lotus2 --dir $(INSTALL) --frames 3 >/dev/null 2>&1
	mv re/pipeline/boot/snap_0_0723ba_chip.bin $@
	rm -f re/pipeline/boot/snap_0_0723ba_fast.bin \
	      re/pipeline/boot/snap_0_0723ba.regs

# tools/image_coherence.py checks every traced region against a snapshot
# taken at that region's first executed pc, and lists the ones that need an
# overlay.  $204bf2-$2059fe was only 18.4% coherent -- ExpMem code loaded
# for the race, long after combined.bin was dumped.  A buffer of zeros
# disassembles as a run of `ori.b #$0,D0`, so the CPU walks through it
# instead of faulting and the failure surfaces thousands of frames later.
re/pipeline/boot/fast_204bf2.bin: build/lotus2
	mkdir -p re/pipeline/boot
	SWIV_SNAP_PCS=204bf2 SWIV_SNAP_FROM=0 SWIV_SNAP_MAX=1 \
	SWIV_SNAP_PREFIX=re/pipeline/boot/rc_ \
	./build/lotus2 --dir $(INSTALL) --frames 9000 \
		--fire-from 2100 --fire-period 100 >/dev/null 2>&1
	mv re/pipeline/boot/rc_0_204bf2_fast.bin $@
	rm -f re/pipeline/boot/rc_0_204bf2_chip.bin \
	      re/pipeline/boot/rc_0_204bf2.regs

$(DECODE_IMAGE): tools/decode_image.py re/pipeline/combined.bin \
		re/pipeline/boot/chip_0723ba.bin re/pipeline/boot/fast_204bf2.bin
	python3 tools/decode_image.py --base re/pipeline/combined.bin \
		--region 723b0-72690=re/pipeline/boot/chip_0723ba.bin \
		--region 204bf2-205a1e=re/pipeline/boot/fast_204bf2.bin --out $@

# Which routines may be replaced by native C.  Two conditions: entered
# only by BSR/JSR (this), and every register the original changes
# reproduced by the port (make render-gate says "[not override-eligible]"
# when not).  Both were learned by breaking the game.
override-check: build/dasm $(DECODE_IMAGE)
	python3 tools/override_check.py 21263c 211dd4 2169e0 2129f2 212680 \
		212662 212ba4 21270a 215b58 215adc 2160f2 215a7a 215a9c 215b24 \
		20d7e8

coherence: build/lotus2 $(DECODE_IMAGE)
	python3 tools/image_coherence.py

# Per-EDGE cycle costs measured from the oracle's own counter.  Keyed by
# (pc -> next pc) rather than by pc, because a conditional branch costs a
# different number of cycles taken than not taken and a per-pc mean rounds
# that away -- worth 0.6% drift per frame, which is a whole frame every
# few thousand.
# The offline disassembler used by the recompiler.  Musashi here is a
# BUILD TOOL, not part of the game: it decodes the image, it does not run it.
build/dasm: tools/dasm.c $(MUSASHI_DIR)/m68kdasm.c
	mkdir -p build
	$(CC) -O2 -std=c11 -I$(MUSASHI_DIR) -o $@ tools/dasm.c \
		$(MUSASHI_DIR)/m68kdasm.c

# Measured on BOTH courses we can reach.  A table measured only on FOREST
# leaves every night-course instruction with a guessed cost, and the small
# errors accumulate into a one-frame lag that shows up as the game taking
# a different path -- which is exactly how the night course first failed.
# Measured on EVERY course we can reach.  A table measured only on FOREST
# leaves the other courses' instructions with guessed costs, and the small
# errors accumulate into a one-frame lag that sends the game down a
# different path -- which is how night, fog and snow each first failed.
COURSES = night fog snow desert motorway marsh storm
re/pipeline/cycles.txt: build/lotus2 $(wildcard re/pipeline/courses/*.rec)
	SWIV_CYCLES=build/cyc_forest.txt ./build/lotus2 --dir $(INSTALL) \
		--frames 9000 --fire-from 2100 --fire-period 100 >/dev/null 2>&1
	@for c in $(COURSES); do \
		n=$$(stat -c%s re/pipeline/courses/$$c.rec); \
		SWIV_CYCLES=build/cyc_$$c.txt ./build/lotus2 --dir $(INSTALL) \
			--frames $$n --replay re/pipeline/courses/$$c.rec \
			--keys re/pipeline/courses/$$c.keys >/dev/null 2>&1; \
	done
	cat build/cyc_forest.txt $(addprefix build/cyc_,$(addsuffix .txt,$(COURSES))) \
		| sort -u -k1,2 > $@

# Every reachable course, oracle against native.
course-gate: build/lotus2 build/lotus2_recomp
	@for c in $(COURSES); do \
		n=$$(stat -c%s re/pipeline/courses/$$c.rec); \
		rm -rf build/fg_o build/fg_n; mkdir -p build/fg_o build/fg_n; \
		./build/lotus2 --dir $(INSTALL) --frames $$n \
			--replay re/pipeline/courses/$$c.rec \
			--keys re/pipeline/courses/$$c.keys \
			--ppm-seq build/fg_o/f --ppm-every 250 >/dev/null 2>&1; \
		./build/lotus2_recomp --dir $(INSTALL) --frames $$n \
			--replay re/pipeline/courses/$$c.rec \
			--keys re/pipeline/courses/$$c.keys \
			--ppm-seq build/fg_n/f --ppm-every 250 >/dev/null 2>&1; \
		printf "%-9s " $$c; \
		python3 tools/frame_gate.py build/fg_o build/fg_n | head -1; \
	done

$(RECOMP_SRC): tools/m68k2c.py re/pipeline/pcset_race.txt build/dasm \
		re/pipeline/cycles.txt $(DECODE_IMAGE)
	python3 tools/m68k2c.py --image $(DECODE_IMAGE) \
		--pcset re/pipeline/pcset_race.txt --range 200-390000 \
		--exhaustive \
		--descend --out $@

# Decode integrity: every pc the 68000 actually fetched from must be an
# instruction boundary in our disassembly.  A linear sweep silently walks
# jump tables and inline data as code and comes back into phase, which
# compiles fine and is wrong; this is what catches that.
decode-check: build/dasm re/pipeline/pcset_race.txt $(DECODE_IMAGE)
	python3 tools/decode_check.py

build/recomp_verify: src/recomp/recomp_verify.c build/lotus2_recomp.o \
		src/recomp/m68krt.h
	mkdir -p build
	$(CC) -O1 -std=c11 -w -Isrc/recomp -o $@ \
		src/recomp/recomp_verify.c build/lotus2_recomp.o

recomp: build/recomp_verify

# Same snapshot pairs the hand-ports are gated on, applied to generated C.
recomp-gate: build/recomp_verify
	./tools/recomp_gate.sh

# The whole-game gate: run oracle and native side by side and demand the
# frames be identical.  FRAMES/EVERY override the window.
FRAMES ?= 30000
EVERY  ?= 1000
INPUT  ?= --fire-from 2100 --fire-period 100
frame-gate: build/lotus2 build/lotus2_recomp
	@rm -rf build/fg_o build/fg_n && mkdir -p build/fg_o build/fg_n
	./build/lotus2 --dir $(INSTALL) --frames $(FRAMES) $(INPUT) \
		--ppm-seq build/fg_o/f --ppm-every $(EVERY) >/dev/null 2>&1
	./build/lotus2_recomp --dir $(INSTALL) --frames $(FRAMES) $(INPUT) \
		--ppm-seq build/fg_n/f --ppm-every $(EVERY) >/dev/null 2>&1
	@python3 tools/frame_gate.py build/fg_o build/fg_n

# Record a play session, then judge the native build against the oracle on
# exactly the input a person gave it.  Scripted fire cannot get past a
# password screen, so this is the only route to courses 2-8:
#
#   make play RECORD=my.rec          # play, reach the course, quit
#   make replay-gate REC=my.rec      # oracle vs native on that session
REC ?= build/session.rec
replay-gate: build/lotus2 build/lotus2_recomp
	@test -f $(REC) || { echo "no recording at $(REC); make play RECORD=..."; exit 2; }
	@$(MAKE) --no-print-directory frame-gate \
		FRAMES=$$(stat -c%s $(REC)) EVERY=$(EVERY) \
		INPUT="--replay $(REC)"

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

.PHONY: play-min course-snaps course-gate replay-gate override-check frame-gate debug coherence play no-musashi decode-check recomp recomp-gate statelog trace pcset drive all native boot-test selftest oracle test clean gate-capture road-capture render-gate view view-race track course

# Per-course race snapshots for the viewer's ROAD VIEW.  Each is a real
# race in that course, reached by replaying the password session that
# make course-gate uses, so the road chain has that course's own table,
# palette and copper list to work from.  Big (9 MB each) and derived, so
# they are not in the repository -- regenerate with this target.
COURSE_SNAPS = night fog snow desert motorway marsh storm
course-snaps: build/lotus2_recomp
	mkdir -p re/pipeline/courses
	SWIV_SNAP_PCS=211e78 SWIV_SNAP_FROM=6300 SWIV_SNAP_MAX=1 \
	SWIV_SNAP_PREFIX=re/pipeline/courses/forest_ \
	./build/lotus2_recomp --dir $(INSTALL) --fire-from 2100 \
		--fire-period 100 --frames 6310 >/dev/null 2>&1
	@for c in $(COURSE_SNAPS); do \
		SWIV_SNAP_PCS=211e78 SWIV_SNAP_FROM=6300 SWIV_SNAP_MAX=1 \
		SWIV_SNAP_PREFIX=re/pipeline/courses/$$c\_ \
		./build/lotus2_recomp --dir $(INSTALL) \
			--replay re/pipeline/courses/$$c.rec \
			--keys re/pipeline/courses/$$c.keys \
			--frames 6310 >/dev/null 2>&1; \
		echo "course-snaps: $$c"; \
	done
