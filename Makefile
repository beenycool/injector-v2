# Makefile — Build inject_jni.dylib for Lunar Client injection (macOS arm64)
#
# Prerequisites:
#   - macOS arm64 (Apple Silicon)
#   - Xcode Command Line Tools (clang)
#   - JDK 17 with JNI headers (e.g. /opt/homebrew/opt/openjdk@17)
#
# Usage:
#   make          — build the dylib
#   make sign     — ad-hoc sign the dylib (required for DYLD_INSERT_LIBRARIES)
#   make clean    — remove build artifacts
#   make all      — build + sign (recommended)

DYLIBS       = inject_jni.dylib
SRC          = inject_jni.c

# ── JDK / JNI paths ─────────────────────────────────────────────────────────
# Adjust JAVA_HOME if your JDK is elsewhere.
JAVA_HOME   ?= /opt/homebrew/opt/openjdk@17
UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  JNI_OS_INC  = $(JAVA_HOME)/include/darwin
else
  JNI_OS_INC  = $(JAVA_HOME)/include/linux
endif
JNI_INCLUDE  = -I$(JAVA_HOME)/include -I$(JNI_OS_INC)

# ── Compiler flags ──────────────────────────────────────────────────────────
CC           = clang
CFLAGS       = -dynamiclib -Wall -Wextra -O2 $(JNI_INCLUDE)
# LDFLAGS: intentionally empty — JNI symbols resolved at runtime via dlopen()+dlsym()
LDFLAGS      =

# Default target
.PHONY: all
all: $(DYLIBS) sign

# Just check syntax without linking (useful for CI on non-macOS)
.PHONY: test-syntax
test-syntax: $(SRC)
	$(CC) -c -Wall -Wextra $(JNI_INCLUDE) -o /dev/null $(SRC)

# Build the dylib
$(DYLIBS): $(SRC)
	@echo "==> Building $@ ..."
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)
	@echo "==> $@ built successfully"

# Ad-hoc code-sign (required for DYLD_INSERT_LIBRARIES)
.PHONY: sign
sign: $(DYLIBS)
	@echo "==> Ad-hoc signing $< ..."
	codesign --force --sign - $<
	@echo "==> Signed. Ready for injection."

# Verify the dylib
.PHONY: check
check: $(DYLIBS)
	@echo "==> Architecture:"
	@lipo -info $<
	@echo ""
	@echo "==> Code signature:"
	@codesign -dv $< 2>&1 || true
	@echo ""
	@echo "==> Dependencies:"
	@otool -L $<

# Clean
.PHONY: clean
clean:
	rm -f $(DYLIBS)
	rm -f *.o

# Show help
.PHONY: help
help:
	@echo "Targets:"
	@echo "  make         — build inject_jni.dylib"
	@echo "  make sign    — ad-hoc sign the dylib"
	@echo "  make all     — build + sign (default)"
	@echo "  make check   — show architecture & signature info"
	@echo "  make test-syntax — compile-only syntax check (cross-platform CI)"
	@echo "  make clean   — remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  JAVA_HOME    — JDK root (default: /opt/homebrew/opt/openjdk@17)"
	@echo ""
	@echo "Usage:"
	@echo "  JAVA_HOME=/usr/local/opt/openjdk@17 make all"
