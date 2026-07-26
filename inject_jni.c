/*
 * inject_jni.c — Lunar Client 1.8.9 JNI Injection Dylib (macOS arm64)
 *
 * Purpose:
 *   Injected via DYLD_INSERT_LIBRARIES into the Lunar Client process.
 *   Waits for the JVM to start, then uses the thread context classloader
 *   to locate net.minecraft.client.Minecraft via JNI.
 *
 * Build (macOS arm64):
 *   clang -dynamiclib \
 *     -I/opt/homebrew/opt/openjdk@17/include \
 *     -I/opt/homebrew/opt/openjdk@17/include/darwin \
 *     -o inject_jni.dylib inject_jni.c \
 *     -framework JavaVM
 *
 *   codesign --force --sign - inject_jni.dylib
 *
 * Inject:
 *   DYLD_INSERT_LIBRARIES=/path/to/inject_jni.dylib ./launch_lunar.sh
 *
 * Output:
 *   /tmp/inject_log.txt   — step-by-step debug log
 *   /tmp/jni_class_found.txt — final result
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include <jni.h>

/* ── logging ─────────────────────────────────────────────────────────────── */

#define LOG_PATH "/tmp/inject_log.txt"
#define RESULT_PATH "/tmp/jni_class_found.txt"

static FILE *g_log = NULL;

static void log_write(const char *msg) {
    if (!g_log) {
        g_log = fopen(LOG_PATH, "w");
        if (!g_log) return;
    }
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(g_log, "[%02d:%02d:%02d] %s\n",
            t->tm_hour, t->tm_min, t->tm_sec, msg);
    fflush(g_log);
}

static void log_close(void) {
    if (g_log) { fclose(g_log); g_log = NULL; }
}

/* ── JNI helpers ─────────────────────────────────────────────────────────── */

/*
 * Check for a pending Java exception, log it, and clear it.
 * Returns 1 if an exception was pending, 0 otherwise.
 */
static int check_and_clear_exception(JNIEnv *env) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        return 1;
    }
    return 0;
}

/*
 * Attach the current thread to the JVM and return a JNIEnv*.
 * Returns NULL on failure.
 */
static JNIEnv* attach_to_jvm(JavaVM *jvm) {
    JNIEnv *env = NULL;
    jint rs;

    // GetEnv returns JNI_EDETACHED if the thread is not attached
    rs = (*jvm)->GetEnv(jvm, (void **)&env, JNI_VERSION_1_6);
    if (rs == JNI_EDETACHED) {
        log_write("Thread not attached, calling AttachCurrentThread...");
        rs = (*jvm)->AttachCurrentThread(jvm, (void **)&env, NULL);
        if (rs != JNI_OK) {
            log_write("ERROR: AttachCurrentThread failed");
            return NULL;
        }
        log_write("AttachCurrentThread OK");
    } else if (rs != JNI_OK) {
        log_write("ERROR: GetEnv failed");
        return NULL;
    } else {
        log_write("Thread already attached to JVM");
    }

    return env;
}

/*
 * Find a class using the thread context classloader pattern.
 *
 * Pattern (Java equivalent):
 *   Thread.currentThread().getContextClassLoader()
 *       .loadClass("net.minecraft.client.Minecraft")
 *
 * Why: Minecraft classes are loaded by a custom classloader (LaunchClassLoader
 * or similar), NOT the bootstrap/system classloader.  FindClass() on the
 * default JNIEnv only searches the bootstrap classloader, so it will never
 * find application classes.  By obtaining the current thread's context
 * classloader and calling Class.forName(name, true, loader), we delegate to
 * the classloader that actually loaded the game.
 */
static jclass find_class_with_thread_loader(JNIEnv *env, const char *name) {
    char buf[512];

    // 1. java.lang.Thread class
    jclass thread_cls = (*env)->FindClass(env, "java/lang/Thread");
    if (!thread_cls || check_and_clear_exception(env)) {
        log_write("ERROR: FindClass(java/lang/Thread) failed");
        return NULL;
    }

    // 2. Thread.currentThread()
    jmethodID current_thread_mid = (*env)->GetStaticMethodID(
        env, thread_cls, "currentThread", "()Ljava/lang/Thread;");
    if (!current_thread_mid || check_and_clear_exception(env)) {
        log_write("ERROR: GetStaticMethodID(currentThread) failed");
        (*env)->DeleteLocalRef(env, thread_cls);
        return NULL;
    }

    jobject current_thread = (*env)->CallStaticObjectMethod(
        env, thread_cls, current_thread_mid);
    if (!current_thread || check_and_clear_exception(env)) {
        log_write("ERROR: CallStaticObjectMethod(currentThread) failed");
        (*env)->DeleteLocalRef(env, thread_cls);
        return NULL;
    }

    // 3. Thread.getContextClassLoader()
    jmethodID get_ccl_mid = (*env)->GetMethodID(
        env, thread_cls, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    if (!get_ccl_mid || check_and_clear_exception(env)) {
        log_write("ERROR: GetMethodID(getContextClassLoader) failed");
        (*env)->DeleteLocalRef(env, current_thread);
        (*env)->DeleteLocalRef(env, thread_cls);
        return NULL;
    }

    jobject class_loader = (*env)->CallObjectMethod(
        env, current_thread, get_ccl_mid);
    if (!class_loader || check_and_clear_exception(env)) {
        log_write("ERROR: CallObjectMethod(getContextClassLoader) failed");
        (*env)->DeleteLocalRef(env, current_thread);
        (*env)->DeleteLocalRef(env, thread_cls);
        return NULL;
    }
    log_write("Got thread context ClassLoader");

    // 4. java.lang.Class.forName(String, boolean, ClassLoader)
    jclass class_cls = (*env)->FindClass(env, "java/lang/Class");
    if (!class_cls || check_and_clear_exception(env)) {
        log_write("ERROR: FindClass(java/lang/Class) failed");
        (*env)->DeleteLocalRef(env, class_loader);
        (*env)->DeleteLocalRef(env, current_thread);
        (*env)->DeleteLocalRef(env, thread_cls);
        return NULL;
    }

    jmethodID for_name_mid = (*env)->GetStaticMethodID(
        env, class_cls,
        "forName",
        "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    if (!for_name_mid || check_and_clear_exception(env)) {
        log_write("ERROR: GetStaticMethodID(forName) failed");
        (*env)->DeleteLocalRef(env, class_cls);
        (*env)->DeleteLocalRef(env, class_loader);
        (*env)->DeleteLocalRef(env, current_thread);
        (*env)->DeleteLocalRef(env, thread_cls);
        return NULL;
    }

    jstring class_name_str = (*env)->NewStringUTF(env, name);
    if (!class_name_str || check_and_clear_exception(env)) {
        log_write("ERROR: NewStringUTF failed");
        (*env)->DeleteLocalRef(env, class_cls);
        (*env)->DeleteLocalRef(env, class_loader);
        (*env)->DeleteLocalRef(env, current_thread);
        (*env)->DeleteLocalRef(env, thread_cls);
        return NULL;
    }

    jclass result = (jclass)(*env)->CallStaticObjectMethod(
        env, class_cls, for_name_mid,
        class_name_str,
        JNI_TRUE,        // initialize
        class_loader);

    if (check_and_clear_exception(env)) {
        snprintf(buf, sizeof(buf),
                 "ERROR: Class.forName(\"%s\", true, classLoader) threw exception", name);
        log_write(buf);
        result = NULL;
    } else if (result != NULL) {
        snprintf(buf, sizeof(buf),
                 "SUCCESS: Found class %s via thread context classloader", name);
        log_write(buf);
    } else {
        snprintf(buf, sizeof(buf),
                 "ERROR: Class.forName(\"%s\", ...) returned NULL (no exception)", name);
        log_write(buf);
    }

    // Cleanup local refs (keep result)
    (*env)->DeleteLocalRef(env, class_name_str);
    (*env)->DeleteLocalRef(env, class_cls);
    (*env)->DeleteLocalRef(env, class_loader);
    (*env)->DeleteLocalRef(env, current_thread);
    (*env)->DeleteLocalRef(env, thread_cls);

    return result;
}

/* ── main injection logic ────────────────────────────────────────────────── */

/*
 * This function runs in a worker thread spawned by the constructor.
 * It polls until libjvm.dylib becomes available, then finds the JVM,
 * attaches, and looks up the Minecraft class.
 */
static void *injection_worker(void *arg) {
    (void)arg;

    log_write("=== inject_jni worker thread started ===");

    // ── Phase 1: locate libjvm.dylib ─────────────────────────────────────
    // The dylib constructor runs before main(), so libjvm may not be loaded
    // yet.  Poll with dlopen(RTLD_NOLOAD) until it appears.

    void *jvm_lib = NULL;
    int attempts = 0;
    const int max_attempts = 120;  // ~60 seconds with 500ms sleep

    log_write("Waiting for libjvm.dylib to be loaded...");

    while (attempts < max_attempts) {
        jvm_lib = dlopen("libjvm.dylib", RTLD_NOLOAD);
        if (jvm_lib != NULL) break;
        attempts++;
        usleep(500000);  // 500 ms
    }

    if (jvm_lib == NULL) {
        log_write("ERROR: libjvm.dylib not loaded after timeout");
        FILE *f = fopen(RESULT_PATH, "w");
        if (f) {
            fprintf(f, "ERROR: libjvm.dylib was never loaded\n");
            fclose(f);
        }
        log_close();
        return NULL;
    }

    log_write("Found libjvm.dylib (already loaded)");

    // ── Phase 2: get JNI_GetCreatedJavaVMs ───────────────────────────────

    typedef jint (*JNI_GetCreatedJavaVMs_t)(JavaVM **, jsize, jsize *);
    JNI_GetCreatedJavaVMs_t JNI_GetCreatedJavaVMs_ptr =
        (JNI_GetCreatedJavaVMs_t)dlsym(jvm_lib, "JNI_GetCreatedJavaVMs");

    if (!JNI_GetCreatedJavaVMs_ptr) {
        log_write("ERROR: dlsym(JNI_GetCreatedJavaVMs) failed");
        log_write(dlerror());
        FILE *f = fopen(RESULT_PATH, "w");
        if (f) {
            fprintf(f, "ERROR: JNI_GetCreatedJavaVMs symbol not found\n");
            fclose(f);
        }
        log_close();
        return NULL;
    }

    log_write("Found JNI_GetCreatedJavaVMs symbol");

    // ── Phase 3: wait for JVM to be created ──────────────────────────────

    JavaVM *jvm = NULL;
    jsize n_vms = 0;
    attempts = 0;

    log_write("Waiting for JVM to be created...");

    while (attempts < max_attempts) {
        jint rs = JNI_GetCreatedJavaVMs_ptr(&jvm, 1, &n_vms);
        if (rs == JNI_OK && n_vms > 0) break;
        attempts++;
        usleep(500000);
    }

    if (n_vms == 0) {
        log_write("ERROR: No JVM created after timeout");
        FILE *f = fopen(RESULT_PATH, "w");
        if (f) {
            fprintf(f, "ERROR: JVM was never created\n");
            fclose(f);
        }
        log_close();
        return NULL;
    }

    {
        char buf[128];
        snprintf(buf, sizeof(buf), "JVM found (n_vms=%d)", (int)n_vms);
        log_write(buf);
    }

    // ── Phase 4: attach current thread to JVM ────────────────────────────

    JNIEnv *env = attach_to_jvm(jvm);
    if (!env) {
        FILE *f = fopen(RESULT_PATH, "w");
        if (f) {
            fprintf(f, "ERROR: Could not attach to JVM\n");
            fclose(f);
        }
        log_close();
        return NULL;
    }

    // ── Phase 5: find Minecraft class via thread context classloader ─────

    log_write("Looking up net.minecraft.client.Minecraft via context classloader...");
    jclass mc_class = find_class_with_thread_loader(
        env, "net.minecraft.client.Minecraft");

    // ── Phase 6: write result ────────────────────────────────────────────

    FILE *f = fopen(RESULT_PATH, "w");
    if (f) {
        if (mc_class != NULL) {
            fprintf(f, "SUCCESS: Found net.minecraft.client.Minecraft\n");
            fprintf(f, "  Class pointer: %p\n", (void *)mc_class);
            fprintf(f, "  Classloader:   thread context classloader\n");
            fprintf(f, "  Method:        Class.forName(name, true, classLoader)\n");
            log_write("Result written: SUCCESS");
        } else {
            fprintf(f, "FAILURE: Could not find net.minecraft.client.Minecraft\n");
            fprintf(f, "  The class was not found via the thread context classloader.\n");
            fprintf(f, "  Possible causes:\n");
            fprintf(f, "    - Minecraft hasn't loaded yet (check timing)\n");
            fprintf(f, "    - The class name has changed (try other obfuscated names)\n");
            fprintf(f, "    - The context classloader is null or different\n");
            log_write("Result written: FAILURE");
        }
        fclose(f);
    } else {
        log_write("ERROR: Could not write result file");
    }

    log_write("=== inject_jni worker thread finished ===");
    log_close();
    return NULL;
}

/* ── constructor ─────────────────────────────────────────────────────────── */

/*
 * Runs automatically when the dylib is loaded via DYLD_INSERT_LIBRARIES.
 *
 * NOTE: On macOS, this runs very early — before main() and before the JVM
 * is created.  We spawn a worker thread that polls until libjvm.dylib and
 * the JVM become available, then performs the JNI work.
 */
__attribute__((constructor))
static void on_load(void) {
    // Use a quick fopen to announce the dylib loaded (before the log system)
    FILE *quick = fopen(LOG_PATH, "a");
    if (quick) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        fprintf(quick, "[%02d:%02d:%02d] inject_jni.dylib loaded (constructor)\n",
                t->tm_hour, t->tm_min, t->tm_sec);
        fclose(quick);
    }

    // Prevent the constructor from blocking the main thread (which would
    // deadlock the JVM startup).  Instead, spawn a detached worker thread.
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&thread, &attr, injection_worker, NULL);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        FILE *f = fopen(RESULT_PATH, "w");
        if (f) {
            fprintf(f, "ERROR: pthread_create failed (rc=%d)\n", rc);
            fclose(f);
        }
    }
}
