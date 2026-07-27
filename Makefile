# Makefile — Build inject_jni.dylib and cheat_phase2.dylib (macOS arm64)
#
# Prerequisites:
#   - macOS arm64 (Apple Silicon)
#   - Xcode Command Line Tools (clang)
#   - JDK 17 with JNI headers (e.g. /opt/homebrew/opt/openjdk@17)
#
# Usage:
#   make            — build Phase 1 inject_jni.dylib
#   make phase2     — build Phase 2 cheat_phase2.dylib (+ OpenGL hook & ESP)
#   make all        — build both
#   make sign       — ad-hoc sign all dylibs
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

# ── Phase 1: basic JNI injection dylib ─────────────────────────────────────
.PHONY: phase1
phase1: inject_jni.dylib sign-inject

inject_jni.dylib: inject_jni.c
	@echo "==> Building Phase 1: $@ ..."
	$(CC) -dynamiclib -Wall -Wextra -O2 $(JNI_INCLUDE) -o $@ inject_jni.c
	@echo "==> $@ built successfully"

# ── Phase 2: JNI reflection + OpenGL hook + ESP ─────────────────────────────
.PHONY: phase2
phase2: cheat_phase2.dylib sign-phase2

cheat_phase2.dylib: cheat_phase2.c
	@echo "==> Building Phase 2: $@ ..."
	$(CC) -dynamiclib -Wall -Wextra -O2 $(JNI_INCLUDE) $(OPENGL_FW) -o $@ cheat_phase2.c
	@echo "==> $@ built successfully"

# ── Ad-hoc code-sign (required for DYLD_INSERT_LIBRARIES) ───────────────────
.PHONY: sign sign-inject sign-phase2
sign: sign-inject sign-phase2

sign-inject: inject_jni.dylib
	@echo "==> Ad-hoc signing inject_jni.dylib ..."
	codesign --force --sign - inject_jni.dylib
	@echo "==> Signed."

sign-phase2: cheat_phase2.dylib
	@echo "==> Ad-hoc signing cheat_phase2.dylib ..."
	codesign --force --sign - cheat_phase2.dylib
	@echo "==> Signed."

# ── Syntax checks (CI) ──────────────────────────────────────────────────────
.PHONY: test-syntax
test-syntax:
	@echo "==> Syntax check: inject_jni.c ..."
	$(CC) -c -Wall -Wextra $(JNI_INCLUDE) -o /dev/null inject_jni.c
	@echo "    OK"
	@echo "==> Syntax check: cheat_phase2.c ..."
	$(CC) -c -Wall -Wextra $(JNI_INCLUDE) -iframework /System/Library/Frameworks -include OpenGL/gl.h -include OpenGL/OpenGL.h -o /dev/null cheat_phase2.c 2>/dev/null || \
		(echo "    (skipped — no OpenGL on this platform)" && true)
	@echo "    OK/checked"

# ── Verify ───────────────────────────────────────────────────────────────────
.PHONY: check
check: inject_jni.dylib cheat_phase2.dylib
	@for dylib in inject_jni.dylib cheat_phase2.dylib; do \
		if [ -f "$$dylib" ]; then \
			echo "==> $$dylib architecture:"; \
			lipo -info "$$dylib" 2>/dev/null || file "$$dylib"; \
			echo ""; \
			echo "==> $$dylib dependencies:"; \
			otool -L "$$dylib" 2>/dev/null || echo "  (otool not available)"; \
			echo "---"; \
		fi; \
	done

# ── Clean ───────────────────────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -f inject_jni.dylib cheat_phase2.dylib
	rm -f *.o

# ── Help ────────────────────────────────────────────────────────────────────
.PHONY: help all
all: inject_jni.dylib cheat_phase2.dylib sign

help:
	@echo "Targets:"
	@echo "  make (or make all)     — build both dylibs + sign"
	@echo "  make phase1            — build Phase 1 (JNI injection test)"
	@echo "  make phase2            — build Phase 2 (ESP overlay)"
	@echo "  make sign              — ad-hoc sign all dylibs"
	@echo "  make check             — show architecture & dependencies"
	@echo "  make test-syntax       — compile-only syntax check (CI)"
	@echo "  make clean             — remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  JAVA_HOME              — JDK root (default: /opt/homebrew/opt/openjdk@17)"
