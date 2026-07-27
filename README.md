# Injector v2 — macOS Lunar Client cheat dylib

A native macOS ARM64 dylib injected into Lunar Client (1.8.9) via
`DYLD_INSERT_LIBRARIES`. Uses the JNI Invocation API to attach to the hosting
JVM, discover obfuscated game internals via Java reflection, and hook OpenGL
for ESP overlay rendering.

## How it works

| File | Purpose |
|---|---|
| `cheat_phase2.c` | The dylib source — JNI injection, reflection discovery, fishhook OpenGL hook, ESP |
| `launch_lunar.sh` | Shell script to launch Lunar Client with `DYLD_INSERT_LIBRARIES` |
| `Makefile` | Build automation |

**Injection flow:**

1. `__attribute__((constructor))` runs at process start → spawns worker thread
2. Worker polls `dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs")` until JVM is ready
3. Attaches current thread to the JVM with `AttachCurrentThread`
4. Uses **Java reflection** (`getDeclaredMethods`/`getDeclaredFields`) to discover
   the actual names of `getMinecraft()`, `theWorld`, `thePlayer`,
   `playerEntities`, and position fields — matched by **type**, not name.
   This works regardless of Lunar's obfuscation / remapping (Ichor).
5. Installs a **fishhook** interpose on `CGLFlushDrawable` (the macOS buffer swap)
6. Every frame: save GL state → ortho projection → world-to-screen transform →
   green ESP boxes around players → restore GL state → call original
7. Worker thread updates player positions from JNI at ~60Hz

**Logging:**

| File | Contents |
|---|---|
| `/tmp/phase2_step.txt` | Timestamped step-by-step log (what was discovered, any errors) |
| `/tmp/phase2_result.txt` | Quick summary: SUCCESS/FAILURE + what was found |

## Prerequisites

- macOS (Apple Silicon / ARM64)
- Xcode Command Line Tools (`xcode-select --install`)
- JDK 17 with JNI headers (e.g. `brew install openjdk@17`)
- Lunar Client 1.8.9 installed (uses the Zulu 17 JRE bundled with Lunar)

The `java` binary **must** have these Hardened Runtime entitlements set:
- `com.apple.security.cs.allow-dyld-environment-variables`
- `com.apple.security.cs.disable-library-validation`

If you haven't done this yet:

```bash
codesign --force --options runtime \
  --entitlements entitlements.plist \
  --sign - \
  "/Users/soodies/.lunarclient/jre/5f250fb4cc79aea565a238c6cc374182dbce30ba/zulu17.64.17-ca-jre17.0.18-macosx_aarch64/zulu-17.jre/Contents/Home/bin/java"
```

## Build & Run

```bash
# Build
make

# Launch
./launch_lunar.sh
```

### Options

```bash
./launch_lunar.sh --no-inject           # Launch without the cheat dylib
CHEAT_DYLIB=/custom/path.dylib ./launch_lunar.sh  # Use a custom dylib path
```

## CI

GitHub Actions builds `cheat_phase2.dylib` on macOS, ad-hoc signs it, and uploads
the binary as an artifact. Note: the ad-hoc signature is machine-bound — you must
re-sign the dylib on your machine before it can be injected.

## Debugging

If Lunar crashes on launch, check `/tmp/phase2_step.txt` for the last completed
step. Common issues:

| Symptom | Likely cause |
|---|---|
| dylib rejected | Missing entitlements on the `java` binary |
| `/tmp/phase2_result.txt` not created | dylib failed to load (check console for dyld errors) |
| `FAILURE: discovery failed` | Class/field names changed; check `phase2_step.txt` |
| No ESP boxes showing | Hook installed but `CGLFlushDrawable` not called (LWJGL may use different path) |
| Crash on game exit | Usually `libWebOSR-Binding.dylib` (unrelated to injector) |
