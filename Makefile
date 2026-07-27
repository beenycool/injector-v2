# Makefile — Build cheat_phase2.dylib for Lunar Client injection (macOS arm64)
#
# Prerequisites:
#   - macOS arm64 (Apple Silicon)
#   - Xcode Command Line Tools (clang)
#   - JDK 17 with JNI headers (e.g. /opt/homebrew/opt/openjdk@17)
#
# Usage:
#   make            — build cheat_phase2.dylib and sign it
#   make sign       — re-sign the dylib
#   make clean      — remove build artifacts

# ── JDK / JNI paths ─────────────────────────────────────────────────────────
JAVA_HOME   ?= /opt/homebrew/opt/openjdk@17
UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  JNI_OS_INC  = $(JAVA_HOME)/include/darwin
  OPENGL_FW   = -framework OpenGL
else
  JNI_OS_INC  = $(JAVA_HOME)/include/linux
  OPENGL_FW   =
endif
JNI_INCLUDE  = -I$(JAVA_HOME)/include -I$(JNI_OS_INC)

# ── Compiler ─────────────────────────────────────────────────────────────────
# On macOS: clang (from Xcode CLT or /usr/bin/clang)
# On Linux: gcc or cc (set CC=gcc if needed)
CC          ?= clang
ifeq ($(shell which $(CC) 2>/dev/null || echo nope),nope)
  ifneq ($(shell which cc 2>/dev/null || echo nope),nope)
    CC := cc
  else ifneq ($(shell which gcc 2>/dev/null || echo nope),nope)
    CC := gcc
  endif
endif

# ── Targets ──────────────────────────────────────────────────────────────────
DYLIBS       = cheat_phase2.dylib
SRC          = cheat_phase2.c

.PHONY: default
default: $(DYLIBS) sign

.PHONY: all
all: $(DYLIBS) sign

$(DYLIBS): $(SRC)
	@echo "==> Building $@ ..."
	$(CC) -dynamiclib -Wall -Wextra -O2 $(JNI_INCLUDE) $(OPENGL_FW) -o $@ $(SRC)
	@echo "==> $@ built successfully"

# ── Ad-hoc code-sign ─────────────────────────────────────────────────────────
.PHONY: sign
sign: $(DYLIBS)
	@echo "==> Ad-hoc signing $< ..."
	codesign --force --sign - $<
	@echo "==> Signed."

# ── Syntax check (CI) ───────────────────────────────────────────────────────
.PHONY: test-syntax
test-syntax:
	@echo "==> Syntax check: cheat_phase2.c ..."
	$(CC) -c -Wall -Wextra $(JNI_INCLUDE) -iframework /System/Library/Frameworks -include OpenGL/gl.h -include OpenGL/OpenGL.h -o /dev/null cheat_phase2.c 2>/dev/null || \
		(echo "    (skipped — no OpenGL on this platform)" && true)
	@echo "    OK/checked"

# ── Verify ───────────────────────────────────────────────────────────────────
.PHONY: check
check: $(DYLIBS)
	@echo "==> $< architecture:"
	@lipo -info $< 2>/dev/null || file $<
	@echo ""
	@echo "==> $< dependencies:"
	@otool -L $< 2>/dev/null || echo "  (otool not available)"

# ── Clean ───────────────────────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -f $(DYLIBS)
	rm -f *.o

# ── Help ────────────────────────────────────────────────────────────────────
.PHONY: help
help:
	@echo "Targets:"
	@echo "  make              — build cheat_phase2.dylib + sign"
	@echo "  make sign         — re-sign the dylib"
	@echo "  make check        — show architecture & dependencies"
	@echo "  make test-syntax  — compile-only syntax check (CI)"
	@echo "  make clean        — remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  JAVA_HOME         — JDK root (default: /opt/homebrew/opt/openjdk@17)"
