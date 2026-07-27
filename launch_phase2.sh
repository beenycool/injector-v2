#!/bin/bash
# launch_phase2.sh — Launch Lunar Client 1.8.9 with Phase 2 ESP dylib
#
# Usage:
#   ./launch_phase2.sh [--no-inject] [additional Lunar args...]
#
# Environment variable:
#   PHASE2_DYLIB  — path to cheat_phase2.dylib (default: ./cheat_phase2.dylib)

set -euo pipefail

JAVA_BIN="/Users/soodies/.lunarclient/jre/5f250fb4cc79aea565a238c6cc374182dbce30ba/zulu17.64.17-ca-jre17.0.18-macosx_aarch64/zulu-17.jre/Contents/Home/bin/java"
PHASE2_DYLIB="${PHASE2_DYLIB:-$(cd "$(dirname "$0")" && pwd)/cheat_phase2.dylib}"

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

if [ ! -f "$JAVA_BIN" ]; then
    echo "ERROR: Could not find Zulu JRE at $JAVA_BIN"
    exit 1
fi

if [ "$NO_INJECT" = false ]; then
    if [ ! -f "$PHASE2_DYLIB" ]; then
        echo "ERROR: Phase 2 dylib not found at $PHASE2_DYLIB"
        echo "  Build it first: make phase2"
        echo "  Or run with --no-inject to skip injection."
        exit 1
    fi
fi

# Clean up old logs
rm -f /tmp/phase2_step.txt /tmp/phase2_result.txt

if [ "$NO_INJECT" = false ]; then
    echo "[*] Launching Lunar Client with Phase 2 ESP injection..."
    echo "    Dylib: $PHASE2_DYLIB"

    export DYLD_INSERT_LIBRARIES="$PHASE2_DYLIB"
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

    echo ""
    echo "[*] Lunar exited. Checking Phase 2 results..."
    if [ -f /tmp/phase2_result.txt ]; then
        echo "--- phase2_result.txt ---"
        cat /tmp/phase2_result.txt
        echo "--- end ---"
    else
        echo "WARNING: /tmp/phase2_result.txt not found! Inject crashed early."
    fi
    if [ -f /tmp/phase2_step.txt ]; then
        echo ""
        echo "[*] Step log: /tmp/phase2_step.txt"
        echo "--- phase2_step.txt (last 30 lines) ---"
        tail -30 /tmp/phase2_step.txt
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
