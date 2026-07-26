# injector-v2 — Lunar Client JNI Injection (macOS arm64)

Injects a custom `.dylib` into Lunar Client 1.8.9 on macOS to access the JVM via JNI, without reverse-engineering or modifying any Lunar/Java binaries.

## How it works

Lunar Client's bundled Zulu JRE `java` binary has two key entitlements set to `true`:

- `com.apple.security.cs.allow-dyld-environment-variables`
- `com.apple.security.cs.disable-library-validation`

This means `DYLD_INSERT_LIBRARIES` works **without SIP modifications**. Our dylib loads early, spawns a worker thread, and waits for the JVM to start. Once the JVM is ready, it attaches and uses the **thread context classloader** to locate `net.minecraft.client.Minecraft` via `Class.forName(name, true, classLoader)`.

## Files

| File | Purpose |
|------|---------|
| `inject_jni.c` | The dylib source — JNI injection, class lookup |
| `Makefile` | Build + sign for macOS arm64 |
| `launch_lunar.sh` | Modified Lunar launch script with injection |
| `.github/workflows/build.yml` | CI build on GitHub Actions (macOS runner) |

## Build (on macOS arm64)

### Prerequisites

- macOS arm64 (Apple Silicon)
- Xcode Command Line Tools (`clang`, `codesign`)
- JDK 17 with JNI headers (e.g. `brew install openjdk@17`)

### Commands

```bash
# Build + sign (default JDK at /opt/homebrew/opt/openjdk@17)
make all

# Or specify a custom JDK path
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-17.jdk/Contents/Home make all

# Verify the dylib
make check
```

### Manual compilation

```bash
clang -dynamiclib \
  -I/opt/homebrew/opt/openjdk@17/include \
  -I/opt/homebrew/opt/openjdk@17/include/darwin \
  -o inject_jni.dylib inject_jni.c \
  -framework JavaVM

codesign --force --sign - inject_jni.dylib
```

## Inject and run

```bash
# Default: injects ./inject_jni.dylib
./launch_lunar.sh

# Specify a different dylib
INJECT_DYLIB=/path/to/inject_jni.dylib ./launch_lunar.sh

# Run without injection (vanilla Lunar)
./launch_lunar.sh --no-inject
```

## Verify injection

After Lunar exits, check:

```bash
cat /tmp/jni_class_found.txt    # SUCCESS or FAILURE with debug info
cat /tmp/inject_log.txt          # Step-by-step debug log
```

## Why the thread context classloader?

The default JNI `FindClass()` searches the **bootstrap classloader**, which only knows about JDK classes (java.lang.\*, java.util.\*, etc.). Minecraft's classes — including `net.minecraft.client.Minecraft` — are loaded by the application's custom classloader system (LaunchClassLoader / ModClassLoader).

By obtaining the **current thread's context classloader** and calling `Class.forName(name, true, classLoader)`, we delegate to the same classloader that loaded the game, which **can** resolve Minecraft classes.

### JNI implementation

```
Thread.currentThread()           → get the calling thread
  .getContextClassLoader()        → get its ClassLoader
Class.forName(
  "net.minecraft.client.Minecraft",
  true,                           → initialize the class
  classLoader                     → search via game's classloader
)
```

## Next steps

After the class is found:

- Cache the `jclass` and get method/field IDs
- Hook into the OpenGL rendering loop (`net.minecraft.client.renderer.EntityRenderer`, LWJGL)
- Inject custom rendering, ESP, tracers, UI overlays, etc.
- Add an ImGui overlay for a settings menu

## License

MIT
