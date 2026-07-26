#!/bin/bash
# launch_lunar.sh — Launch Lunar Client 1.8.9 with JNI dylib injection
#
# Usage:
#   ./launch_lunar.sh [--no-inject] [additional Lunar args...]
#
# Environment variables:
#   INJECT_DYLIB  — path to the dylib (default: ./inject_jni.dylib)
#
# Prerequisites:
#   - inject_jni.dylib must be built and ad-hoc signed (see Makefile)
#   - The java binary must have these entitlements:
#       com.apple.security.cs.allow-dyld-environment-variables = true
#       com.apple.security.cs.disable-library-validation   = true

set -euo pipefail

# ── Configuration ───────────────────────────────────────────────────────────

JAVA_BIN="/Users/soodies/.lunarclient/jre/5f250fb4cc79aea565a238c6cc374182dbce30ba/zulu17.64.17-ca-jre17.0.18-macosx_aarch64/zulu-17.jre/Contents/Home/bin/java"

# Path to the injection dylib (override with INJECT_DYLIB env var)
INJECT_DYLIB="${INJECT_DYLIB:-$(cd "$(dirname "$0")" && pwd)/inject_jni.dylib}"

# ── Argument parsing ────────────────────────────────────────────────────────

NO_INJECT=false
PASSTHROUGH_ARGS=()

for arg in "$@"; do
    case "$arg" in
        --no-inject)
            NO_INJECT=true
            ;;
        *)
            PASSTHROUGH_ARGS+=("$arg")
            ;;
    esac
done

# ── Validation ──────────────────────────────────────────────────────────────

if [ ! -f "$JAVA_BIN" ]; then
    echo "ERROR: Could not find Zulu JRE at $JAVA_BIN"
    exit 1
fi

if [ "$NO_INJECT" = false ]; then
    if [ ! -f "$INJECT_DYLIB" ]; then
        echo "ERROR: Injection dylib not found at $INJECT_DYLIB"
        echo "  Build it first: make -C $(dirname "$INJECT_DYLIB")"
        echo "  Or run with --no-inject to skip injection."
        exit 1
    fi
fi

# ── Clean up old logs ───────────────────────────────────────────────────────

rm -f /tmp/inject_log.txt /tmp/jni_class_found.txt

# ── Launch ──────────────────────────────────────────────────────────────────

if [ "$NO_INJECT" = false ]; then
    echo "[*] Launching Lunar Client with JNI injection..."
    echo "    Dylib: $INJECT_DYLIB"

    # DYLD_INSERT_LIBRARIES tells dyld to load our dylib into the process
    # at startup.  The dylib's __attribute__((constructor)) runs early, then
    # a worker thread waits for the JVM to start before doing JNI work.
    #
    # DYLD_FORCE_FLAT_NAMESPACE is not strictly required here (we use dlsym
    # on the handle from dlopen(RTLD_NOLOAD)), but it can help resolve
    # edge cases with two-level namespaces.  It's safe to include.

    export DYLD_INSERT_LIBRARIES="$INJECT_DYLIB"
    export DYLD_FORCE_FLAT_NAMESPACE=1

    "$JAVA_BIN" \
      --add-modules jdk.naming.dns \
      --add-exports jdk.naming.dns/com.sun.jndi.dns=java.naming \
      -Dlog4j2.formatMsgNoLookups=true \
      --add-opens java.base/java.io=ALL-UNNAMED \
      -XX:+UseStringDeduplication \
      -Dlunar.webosr.url=file:index.html \
      -Xmx3072m \
      -Dichor.fabric.localModPath=/Users/soodies/.lunarclient/profiles/1.8/mods \
      -Djava.library.path=/Users/soodies/.lunarclient/offline/multiver/natives \
      -Dlog4j.configurationFile=/Users/soodies/.lunarclient/profiles/1.8/logs/config.xml \
      -Dichor.logsFile=/Users/soodies/.lunarclient/profiles/1.8/logs/ichor-boot.log \
      -Dichor.usingIsolatedProfiles=true \
      -cp /Users/soodies/.lunarclient/offline/multiver/common-0.1.0-SNAPSHOT-all-nomappings.jar:/Users/soodies/.lunarclient/offline/multiver/lunar-platform-mappings-v1_8.jar:/Users/soodies/.lunarclient/offline/multiver/optifine-0.1.0-SNAPSHOT-all.jar:/Users/soodies/.lunarclient/offline/multiver/genesis-0.1.0-SNAPSHOT-all.jar:/Users/soodies/.lunarclient/offline/multiver/legacy-0.1.0-SNAPSHOT-all-nomappings.jar:/Users/soodies/.lunarclient/offline/multiver/lunar-lang.jar:/Users/soodies/.lunarclient/offline/multiver/lunar.jar \
      com.moonsworth.lunar.genesis.Genesis \
      --version 1.8.9 \
      --launcherVersion 3.7.12-ow \
      --assetIndex 1.8 \
      --gameDir "/Users/soodies/Library/Application Support/minecraft" \
      --assetsDir /Users/soodies/.lunarclient/shared/assets \
      --texturesDir /Users/soodies/.lunarclient/textures \
      --uiDir /Users/soodies/.lunarclient/ui/168cc47a710333ec8c400870db2cf13d688f5cff \
      --webosrDir /Users/soodies/.lunarclient/offline/multiver/natives \
      --workingDirectory . \
      --classpathDir /Users/soodies/.lunarclient/offline/multiver \
      --width 1280 --height 720 \
      "${PASSTHROUGH_ARGS[@]+"${PASSTHROUGH_ARGS[@]}"}"

    # After Lunar exits, show the injection results
    echo ""
    echo "[*] Lunar exited. Checking injection results..."
    if [ -f /tmp/jni_class_found.txt ]; then
        echo "--- jni_class_found.txt ---"
        cat /tmp/jni_class_found.txt
        echo "--- end ---"
    fi
    if [ -f /tmp/inject_log.txt ]; then
        echo ""
        echo "[*] Full injection log: /tmp/inject_log.txt"
        echo "    (tail shown below)"
        echo "--- inject_log.txt (last 20 lines) ---"
        tail -20 /tmp/inject_log.txt
        echo "--- end ---"
    fi

else
    echo "[*] Launching Lunar Client WITHOUT injection..."
    unset DYLD_INSERT_LIBRARIES
    unset DYLD_FORCE_FLAT_NAMESPACE

    "$JAVA_BIN" \
      --add-modules jdk.naming.dns \
      --add-exports jdk.naming.dns/com.sun.jndi.dns=java.naming \
      -Dlog4j2.formatMsgNoLookups=true \
      --add-opens java.base/java.io=ALL-UNNAMED \
      -XX:+UseStringDeduplication \
      -Dlunar.webosr.url=file:index.html \
      -Xmx3072m \
      -Dichor.fabric.localModPath=/Users/soodies/.lunarclient/profiles/1.8/mods \
      -Djava.library.path=/Users/soodies/.lunarclient/offline/multiver/natives \
      -Dlog4j.configurationFile=/Users/soodies/.lunarclient/profiles/1.8/logs/config.xml \
      -Dichor.logsFile=/Users/soodies/.lunarclient/profiles/1.8/logs/ichor-boot.log \
      -Dichor.usingIsolatedProfiles=true \
      -cp /Users/soodies/.lunarclient/offline/multiver/common-0.1.0-SNAPSHOT-all-nomappings.jar:/Users/soodies/.lunarclient/offline/multiver/lunar-platform-mappings-v1_8.jar:/Users/soodies/.lunarclient/offline/multiver/optifine-0.1.0-SNAPSHOT-all.jar:/Users/soodies/.lunarclient/offline/multiver/genesis-0.1.0-SNAPSHOT-all.jar:/Users/soodies/.lunarclient/offline/multiver/legacy-0.1.0-SNAPSHOT-all-nomappings.jar:/Users/soodies/.lunarclient/offline/multiver/lunar-lang.jar:/Users/soodies/.lunarclient/offline/multiver/lunar.jar \
      com.moonsworth.lunar.genesis.Genesis \
      --version 1.8.9 \
      --launcherVersion 3.7.12-ow \
      --assetIndex 1.8 \
      --gameDir "/Users/soodies/Library/Application Support/minecraft" \
      --assetsDir /Users/soodies/.lunarclient/shared/assets \
      --texturesDir /Users/soodies/.lunarclient/textures \
      --uiDir /Users/soodies/.lunarclient/ui/168cc47a710333ec8c400870db2cf13d688f5cff \
      --webosrDir /Users/soodies/.lunarclient/offline/multiver/natives \
      --workingDirectory . \
      --classpathDir /Users/soodies/.lunarclient/offline/multiver \
      --width 1280 --height 720 \
      "${PASSTHROUGH_ARGS[@]+"${PASSTHROUGH_ARGS[@]}"}"
fi
