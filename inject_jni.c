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
 *     -o inject_jni.dylib inject_jni.c
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
 * Log the details of a pending Java exception (message + stack trace)
 * by calling Exception.toString() via JNI. Returns 1 if exception was
 * pending and logged, 0 otherwise. Clears the exception.
 */
static int log_exception_details(JNIEnv *env, const char *prefix) {
    if (!(*env)->ExceptionCheck(env)) return 0;

    // Get the exception object
    jthrowable exc = (*env)->ExceptionOccurred(env);
    if (exc == NULL) {
        (*env)->ExceptionClear(env);
        return 1;
    }

    // Get exc.getClass().getName()
    jclass exc_cls = (*env)->GetObjectClass(env, exc);
    if (exc_cls) {
        jmethodID get_name_mid = (*env)->GetMethodID(
            env, exc_cls, "getName", "()Ljava/lang/String;");
        if (get_name_mid) {
            jstring name_str = (jstring)(*env)->CallObjectMethod(
                env, exc_cls, get_name_mid);
            if (name_str) {
                const char *name_utf = (*env)->GetStringUTFChars(env, name_str, NULL);
                if (name_utf) {
                    char buf[512];
                    snprintf(buf, sizeof(buf), "%s exception type: %s", prefix, name_utf);
                    // fprintf to the JVM stderr is already used by ExceptionDescribe,
                    // use our own log
                    FILE *tmp = fopen(LOG_PATH, "a");
                    if (tmp) {
                        time_t now = time(NULL);
                        struct tm *t = localtime(&now);
                        fprintf(tmp, "[%02d:%02d:%02d] %s\n",
                                t->tm_hour, t->tm_min, t->tm_sec, buf);
                        fclose(tmp);
                    }
                    (*env)->ReleaseStringUTFChars(env, name_str, name_utf);
                }
            }
        }
    }

    // Get exc.getMessage()
    jmethodID get_msg_mid = (*env)->GetMethodID(
        env, exc_cls, "getMessage", "()Ljava/lang/String;");
    if (get_msg_mid) {
        jstring msg_str = (jstring)(*env)->CallObjectMethod(
            env, exc, get_msg_mid);
        if (msg_str) {
            const char *msg_utf = (*env)->GetStringUTFChars(env, msg_str, NULL);
            if (msg_utf) {
                char buf[512];
                snprintf(buf, sizeof(buf), "%s exception message: %s", prefix, msg_utf);
                FILE *tmp = fopen(LOG_PATH, "a");
                if (tmp) {
                    time_t now = time(NULL);
                    struct tm *t = localtime(&now);
                    fprintf(tmp, "[%02d:%02d:%02d] %s\n",
                            t->tm_hour, t->tm_min, t->tm_sec, buf);
                    fclose(tmp);
                }
                (*env)->ReleaseStringUTFChars(env, msg_str, msg_utf);
            }
        }
    }

    // Get the stack trace as a string
    jmethodID to_string_mid = (*env)->GetMethodID(
        env, exc_cls, "toString", "()Ljava/lang/String;");
    if (to_string_mid) {
        jstring trace_str = (jstring)(*env)->CallObjectMethod(
            env, exc, to_string_mid);
        if (trace_str) {
            const char *trace_utf = (*env)->GetStringUTFChars(env, trace_str, NULL);
            if (trace_utf) {
                FILE *tmp = fopen(LOG_PATH, "a");
                if (tmp) {
                    time_t now = time(NULL);
                    struct tm *t = localtime(&now);
                    fprintf(tmp, "[%02d:%02d:%02d] %s exception: %s\n",
                            t->tm_hour, t->tm_min, t->tm_sec, prefix, trace_utf);
                    fclose(tmp);
                }
                (*env)->ReleaseStringUTFChars(env, trace_str, trace_utf);
            }
        }
    }

    (*env)->ExceptionClear(env);

    // Cleanup refs
    if (exc_cls) (*env)->DeleteLocalRef(env, exc_cls);
    (*env)->DeleteLocalRef(env, exc);

    return 1;
}

/*
 * Log the result of calling toString() on a Java object.
 */
static void log_obj_to_string(JNIEnv *env, jobject obj, const char *label) {
    if (!obj || !env) return;

    jclass obj_cls = (*env)->GetObjectClass(env, obj);
    if (!obj_cls) return;

    jmethodID to_string_mid = (*env)->GetMethodID(
        env, obj_cls, "toString", "()Ljava/lang/String;");
    if (!to_string_mid) {
        (*env)->DeleteLocalRef(env, obj_cls);
        return;
    }

    jstring str = (jstring)(*env)->CallObjectMethod(env, obj, to_string_mid);
    if (str) {
        const char *utf = (*env)->GetStringUTFChars(env, str, NULL);
        if (utf) {
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s = %s", label, utf);
            log_write(buf);
            (*env)->ReleaseStringUTFChars(env, str, utf);
        }
        (*env)->DeleteLocalRef(env, str);
    }
    (*env)->DeleteLocalRef(env, obj_cls);
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
 *
 * Returns: a local-ref jclass, or NULL on failure.
 */
static jclass find_class_with_loader(JNIEnv *env, const char *name,
                                      jobject class_loader) {
    char buf[512];

    // java.lang.Class.forName(String, boolean, ClassLoader)
    jclass class_cls = (*env)->FindClass(env, "java/lang/Class");
    if (!class_cls || check_and_clear_exception(env)) {
        log_write("ERROR: FindClass(java/lang/Class) failed");
        return NULL;
    }

    jmethodID for_name_mid = (*env)->GetStaticMethodID(
        env, class_cls,
        "forName",
        "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    if (!for_name_mid || check_and_clear_exception(env)) {
        log_write("ERROR: GetStaticMethodID(forName) failed");
        (*env)->DeleteLocalRef(env, class_cls);
        return NULL;
    }

    jstring class_name_str = (*env)->NewStringUTF(env, name);
    if (!class_name_str || check_and_clear_exception(env)) {
        log_write("ERROR: NewStringUTF failed");
        (*env)->DeleteLocalRef(env, class_cls);
        return NULL;
    }

    jclass result = (jclass)(*env)->CallStaticObjectMethod(
        env, class_cls, for_name_mid,
        class_name_str,
        JNI_TRUE,        // initialize
        class_loader);

    if (log_exception_details(env, "Class.forName")) {
        snprintf(buf, sizeof(buf),
                 "  -> Class.forName(\"%s\", true, classLoader) FAILED", name);
        log_write(buf);
        result = NULL;
    } else if (result != NULL) {
        snprintf(buf, sizeof(buf),
                 "SUCCESS: Found %s", name);
        log_write(buf);
    } else {
        snprintf(buf, sizeof(buf),
                 "  -> Class.forName(\"%s\", ...) returned NULL", name);
        log_write(buf);
    }

    (*env)->DeleteLocalRef(env, class_name_str);
    (*env)->DeleteLocalRef(env, class_cls);

    return result;
}

/*
 * Obtain the appropriate classloader for class lookup.
 *
 * Tries in order:
 *   1. Thread.currentThread().getContextClassLoader()
 *   2. ClassLoader.getSystemClassLoader()
 *   3. JNI FindClass (bootstrap classloader) — indicated by returning NULL
 *
 * Returns: a local-ref jobject (classloader), or NULL meaning "use bootstrap".
 * The caller must delete the local ref when done.
 */
static jobject get_class_loader(JNIEnv *env) {
    // 1. java.lang.Thread class
    jclass thread_cls = (*env)->FindClass(env, "java/lang/Thread");
    if (!thread_cls || check_and_clear_exception(env)) {
        log_write("ERROR: FindClass(java/lang/Thread) failed");
        return NULL;
    }

    // 1a. Thread.currentThread()
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

    // Log current thread name
    log_obj_to_string(env, current_thread, "Current thread");

    // 1b. Thread.getContextClassLoader()
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
    if (log_exception_details(env, "getContextClassLoader")) {
        log_write("WARNING: getContextClassLoader() threw exception");
        // Fall through — class_loader may still be non-null
    }

    // Check if context classloader is non-null
    if (class_loader != NULL) {
        log_write("Got thread context ClassLoader");
        log_obj_to_string(env, class_loader, "  -> context ClassLoader");
        (*env)->DeleteLocalRef(env, current_thread);
        (*env)->DeleteLocalRef(env, thread_cls);
        return class_loader;
    }

    // 2. getContextClassLoader() returned NULL.
    // This is normal for native threads attached via JNI.
    log_write("WARNING: Thread context classloader is NULL (native thread)");
    log_write("  -> Falling back to ClassLoader.getSystemClassLoader()");

    jclass system_cls = (*env)->FindClass(env, "java/lang/ClassLoader");
    if (!system_cls || check_and_clear_exception(env)) {
        log_write("ERROR: FindClass(java/lang/ClassLoader) failed");
        (*env)->DeleteLocalRef(env, current_thread);
        (*env)->DeleteLocalRef(env, thread_cls);
        return NULL;
    }

    jmethodID get_sys_mid = (*env)->GetStaticMethodID(
        env, system_cls, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    if (!get_sys_mid || check_and_clear_exception(env)) {
        log_write("ERROR: GetStaticMethodID(getSystemClassLoader) failed");
        (*env)->DeleteLocalRef(env, system_cls);
        (*env)->DeleteLocalRef(env, current_thread);
        (*env)->DeleteLocalRef(env, thread_cls);
        return NULL;
    }

    class_loader = (*env)->CallStaticObjectMethod(
        env, system_cls, get_sys_mid);
    if (log_exception_details(env, "getSystemClassLoader")) {
        log_write("ERROR: getSystemClassLoader() threw exception");
        (*env)->DeleteLocalRef(env, system_cls);
        (*env)->DeleteLocalRef(env, current_thread);
        (*env)->DeleteLocalRef(env, thread_cls);
        return NULL;
    }

    if (!class_loader) {
        log_write("ERROR: getSystemClassLoader() returned NULL");
        (*env)->DeleteLocalRef(env, system_cls);
        (*env)->DeleteLocalRef(env, current_thread);
        (*env)->DeleteLocalRef(env, thread_cls);
        return NULL;
    }

    log_write("Got system ClassLoader");
    log_obj_to_string(env, class_loader, "  -> system ClassLoader");

    // Log parent chain
    jmethodID get_parent_mid = (*env)->GetMethodID(
        env, system_cls, "getParent", "()Ljava/lang/ClassLoader;");
    if (get_parent_mid) {
        jobject parent = (*env)->CallObjectMethod(env, class_loader, get_parent_mid);
        if (parent) {
            log_obj_to_string(env, parent, "  -> system ClassLoader.parent");
            (*env)->DeleteLocalRef(env, parent);
        } else {
            log_write("  -> system ClassLoader.parent = NULL (bootstrap)");
        }
    }

    (*env)->DeleteLocalRef(env, system_cls);
    (*env)->DeleteLocalRef(env, current_thread);
    (*env)->DeleteLocalRef(env, thread_cls);

    return class_loader;
}

/* ── main injection logic ────────────────────────────────────────────────── */

/*
 * Try to get the LaunchClassLoader from net.minecraft.launchwrapper.Launch.
 *
 * The Launch class has a static field:
 *   public static LaunchClassLoader classLoader;
 *
 * This is the classloader that actually loads Minecraft game classes.
 * Returns a local-ref jobject (the LaunchClassLoader), or NULL if not
 * yet available.
 */
static jobject get_launch_classloader(JNIEnv *env) {
    // Use system classloader to find the Launch class
    jobject sys_loader = NULL;
    jclass launch_cls = NULL;

    // First try FindClass — works if the class is on the system classpath
    launch_cls = (*env)->FindClass(env, "net/minecraft/launchwrapper/Launch");
    if (!launch_cls) {
        check_and_clear_exception(env);

        // Fallback: use Class.forName with system classloader
        sys_loader = get_class_loader(env);
        if (!sys_loader) return NULL;

        jclass class_cls = (*env)->FindClass(env, "java/lang/Class");
        if (!class_cls || check_and_clear_exception(env)) {
            (*env)->DeleteLocalRef(env, sys_loader);
            return NULL;
        }
        jmethodID for_name_mid = (*env)->GetStaticMethodID(
            env, class_cls, "forName",
            "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
        if (!for_name_mid || check_and_clear_exception(env)) {
            (*env)->DeleteLocalRef(env, class_cls);
            (*env)->DeleteLocalRef(env, sys_loader);
            return NULL;
        }
        jstring name_str = (*env)->NewStringUTF(env, "net.minecraft.launchwrapper.Launch");
        if (!name_str || check_and_clear_exception(env)) {
            (*env)->DeleteLocalRef(env, class_cls);
            (*env)->DeleteLocalRef(env, sys_loader);
            return NULL;
        }
        launch_cls = (jclass)(*env)->CallStaticObjectMethod(
            env, class_cls, for_name_mid, name_str, JNI_TRUE, sys_loader);
        if (log_exception_details(env, "getLaunchClassLoader") || !launch_cls) {
            (*env)->DeleteLocalRef(env, name_str);
            (*env)->DeleteLocalRef(env, class_cls);
            (*env)->DeleteLocalRef(env, sys_loader);
            return NULL;
        }
        (*env)->DeleteLocalRef(env, name_str);
        (*env)->DeleteLocalRef(env, class_cls);
        (*env)->DeleteLocalRef(env, sys_loader);
    }

    log_write("Found net.minecraft.launchwrapper.Launch class, reading classLoader field...");

    // Get the static field "classLoader"
    jfieldID classloader_fid = (*env)->GetStaticFieldID(
        env, launch_cls, "classLoader",
        "Lnet/minecraft/launchwrapper/LaunchClassLoader;");
    if (!classloader_fid || check_and_clear_exception(env)) {
        log_write("get_launch_classloader: field 'classLoader' not found");
        (*env)->DeleteLocalRef(env, launch_cls);
        return NULL;
    }

    jobject launch_cl = (*env)->GetStaticObjectField(
        env, launch_cls, classloader_fid);
    if (!launch_cl || check_and_clear_exception(env)) {
        log_write("get_launch_classloader: classLoader field is NULL (not initialized yet)");
        (*env)->DeleteLocalRef(env, launch_cls);
        return NULL;
    }

    log_write("Got LaunchClassLoader instance!");
    log_obj_to_string(env, launch_cl, "  -> LaunchClassLoader");

    // Log its parent chain
    jclass loader_cls = (*env)->GetObjectClass(env, launch_cl);
    if (loader_cls) {
        jmethodID get_parent_mid = (*env)->GetMethodID(
            env, loader_cls, "getParent", "()Ljava/lang/ClassLoader;");
        if (get_parent_mid) {
            jobject parent = (*env)->CallObjectMethod(env, launch_cl, get_parent_mid);
            if (parent) {
                log_obj_to_string(env, parent, "  -> LaunchClassLoader.parent");
                (*env)->DeleteLocalRef(env, parent);
            }
        }
        (*env)->DeleteLocalRef(env, loader_cls);
    }

    (*env)->DeleteLocalRef(env, launch_cls);
    return launch_cl;
}

/*
 * Try to find a Minecraft class by chaining through known classloaders.
 *
 * Strategy:
 *   1. Get the system classloader
 *   2. Try to find class directly
 *   3. If that fails and Launch class is available, get LaunchClassLoader
 *      and try with that
 *
 * Returns a global-ref jclass, or NULL.
 */
static jclass find_minecraft_class(JNIEnv *env) {
    const char *target = "net.minecraft.client.Minecraft";
    const char *fallback_names[] = {
        "net.minecraft.client.main.Main",
        NULL
    };

    // ── Strategy 1: system classloader ──
    log_write("find_minecraft_class: trying system classloader...");
    jobject sys_loader = get_class_loader(env);
    if (sys_loader) {
        jclass result = find_class_with_loader(env, target, sys_loader);
        if (result) return result;
        (*env)->DeleteLocalRef(env, sys_loader);
    }

    // ── Strategy 2: LaunchClassLoader ──
    log_write("find_minecraft_class: trying LaunchClassLoader...");
    jobject launch_loader = get_launch_classloader(env);
    if (launch_loader) {
        jclass result = find_class_with_loader(env, target, launch_loader);
        if (result) {
            (*env)->DeleteLocalRef(env, launch_loader);
            return result;
        }
        // Also try fallback class names with LaunchClassLoader
        for (int c = 0; fallback_names[c] != NULL; c++) {
            result = find_class_with_loader(env, fallback_names[c], launch_loader);
            if (result) {
                (*env)->DeleteLocalRef(env, launch_loader);
                return result;
            }
        }
        (*env)->DeleteLocalRef(env, launch_loader);
    }

    // ── Strategy 3: FindClass direct ──
    log_write("find_minecraft_class: trying FindClass direct...");
    jclass fc = (*env)->FindClass(env, target);
    if (fc && !(*env)->ExceptionCheck(env)) {
        log_write("SUCCESS: FindClass(Minecraft) found the class!");
        return (jclass)(*env)->NewGlobalRef(env, fc);
    }
    check_and_clear_exception(env);

    return NULL;
}

/*
 * Enumerate all active Java threads and collect unique context classloaders.
 * Then try to find the Minecraft class with each one.
 *
 * This is the most reliable strategy: the Minecraft Client thread has
 * the correct context classloader that can see game classes, even though
 * our native-attached thread doesn't.  By calling Thread.getAllStackTraces(),
 * iterating all threads, and testing each thread's context classloader
 * via Class.forName(target, true, cl), we find the classloader that
 * actually loaded the game.
 *
 * Returns a global-ref jclass, or NULL.
 */
static jclass find_class_via_threads(JNIEnv *env, const char *target) {
    char buf[512];
    int thread_count = 0;

    // ── 1. Get Thread.getAllStackTraces() → Map<Thread, StackTraceElement[]> ──
    jclass thread_cls = (*env)->FindClass(env, "java/lang/Thread");
    if (!thread_cls || check_and_clear_exception(env)) {
        log_write("find_class_via_threads: FindClass(Thread) failed");
        return NULL;
    }

    jmethodID get_all_mid = (*env)->GetStaticMethodID(
        env, thread_cls, "getAllStackTraces", "()Ljava/util/Map;");
    if (!get_all_mid || check_and_clear_exception(env)) {
        log_write("find_class_via_threads: getMethod(getAllStackTraces) failed");
        (*env)->DeleteLocalRef(env, thread_cls);
        return NULL;
    }

    jobject traces_map = (*env)->CallStaticObjectMethod(env, thread_cls, get_all_mid);
    if (!traces_map || check_and_clear_exception(env)) {
        log_write("find_class_via_threads: getAllStackTraces() returned NULL");
        (*env)->DeleteLocalRef(env, thread_cls);
        return NULL;
    }

    // ── 2. Map.keySet() → Set<Thread> ──
    jclass map_cls = (*env)->GetObjectClass(env, traces_map);
    jmethodID key_set_mid = (*env)->GetMethodID(env, map_cls, "keySet", "()Ljava/util/Set;");
    if (!key_set_mid || check_and_clear_exception(env)) {
        (*env)->DeleteLocalRef(env, map_cls);
        (*env)->DeleteLocalRef(env, thread_cls);
        (*env)->DeleteLocalRef(env, traces_map);
        return NULL;
    }

    jobject key_set = (*env)->CallObjectMethod(env, traces_map, key_set_mid);
    if (!key_set || check_and_clear_exception(env)) {
        (*env)->DeleteLocalRef(env, map_cls);
        (*env)->DeleteLocalRef(env, thread_cls);
        (*env)->DeleteLocalRef(env, traces_map);
        return NULL;
    }

    // ── 3. Set.iterator() → Iterator<Thread> ──
    jclass set_cls = (*env)->GetObjectClass(env, key_set);
    jmethodID iterator_mid = (*env)->GetMethodID(env, set_cls, "iterator", "()Ljava/util/Iterator;");
    if (!iterator_mid || check_and_clear_exception(env)) {
        (*env)->DeleteLocalRef(env, set_cls);
        (*env)->DeleteLocalRef(env, key_set);
        (*env)->DeleteLocalRef(env, map_cls);
        (*env)->DeleteLocalRef(env, thread_cls);
        (*env)->DeleteLocalRef(env, traces_map);
        return NULL;
    }

    jobject iterator = (*env)->CallObjectMethod(env, key_set, iterator_mid);
    if (!iterator || check_and_clear_exception(env)) {
        (*env)->DeleteLocalRef(env, set_cls);
        (*env)->DeleteLocalRef(env, key_set);
        (*env)->DeleteLocalRef(env, map_cls);
        (*env)->DeleteLocalRef(env, thread_cls);
        (*env)->DeleteLocalRef(env, traces_map);
        return NULL;
    }

    jclass iter_cls = (*env)->GetObjectClass(env, iterator);
    jmethodID has_next_mid = (*env)->GetMethodID(env, iter_cls, "hasNext", "()Z");
    jmethodID next_mid = (*env)->GetMethodID(env, iter_cls, "next", "()Ljava/lang/Object;");

    // Thread.getContextClassLoader() + getName()
    jmethodID get_ccl_mid = (*env)->GetMethodID(
        env, thread_cls, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    jmethodID get_name_mid = (*env)->GetMethodID(
        env, thread_cls, "getName", "()Ljava/lang/String;");

    // Class.forName(String, boolean, ClassLoader)
    jclass class_cls_pre = (*env)->FindClass(env, "java/lang/Class");
    jmethodID for_name_mid_pre = NULL;
    if (class_cls_pre) {
        for_name_mid_pre = (*env)->GetStaticMethodID(
            env, class_cls_pre, "forName",
            "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    }

    // ── 4. Iterate threads, test each context classloader ──
    jclass result = NULL;
    int found_count = 0;

    while ((*env)->CallBooleanMethod(env, iterator, has_next_mid) && result == NULL) {
        jobject thread = (*env)->CallObjectMethod(env, iterator, next_mid);
        if (!thread) continue;
        thread_count++;

        if (get_ccl_mid && get_name_mid && for_name_mid_pre) {
            // Get thread name for logging
            jstring tname = (jstring)(*env)->CallObjectMethod(env, thread, get_name_mid);
            const char *name_utf = NULL;
            if (tname) {
                name_utf = (*env)->GetStringUTFChars(env, tname, NULL);
            }

            // Get context classloader
            jobject cl = (*env)->CallObjectMethod(env, thread, get_ccl_mid);

            if (cl && !(*env)->ExceptionCheck(env)) {
                // Try Class.forName(target) with this classloader
                jstring target_str = (*env)->NewStringUTF(env, target);
                if (target_str) {
                    jclass candidate = (jclass)(*env)->CallStaticObjectMethod(
                        env, class_cls_pre, for_name_mid_pre,
                        target_str, JNI_TRUE, cl);

                    if (candidate && !(*env)->ExceptionCheck(env)) {
                        snprintf(buf, sizeof(buf),
                                 "SUCCESS: Thread '%s' classloader can find %s!",
                                 name_utf ? name_utf : "?", target);
                        log_write(buf);
                        log_obj_to_string(env, cl, "  -> Minecraft classloader");

                        found_count++;
                        result = (jclass)(*env)->NewGlobalRef(env, candidate);
                        (*env)->DeleteLocalRef(env, candidate);
                    } else {
                        check_and_clear_exception(env);
                    }
                    (*env)->DeleteLocalRef(env, target_str);
                }
            } else {
                check_and_clear_exception(env);
            }

            if (tname) {
                (*env)->ReleaseStringUTFChars(env, tname, name_utf);
                (*env)->DeleteLocalRef(env, tname);
            }
            if (cl) (*env)->DeleteLocalRef(env, cl);
        }
        (*env)->DeleteLocalRef(env, thread);
    }

    snprintf(buf, sizeof(buf),
             "find_class_via_threads: scanned %d threads, found %d with class %s",
             thread_count, found_count, target);
    log_write(buf);

    // Cleanup
    if (class_cls_pre) (*env)->DeleteLocalRef(env, class_cls_pre);
    (*env)->DeleteLocalRef(env, iter_cls);
    (*env)->DeleteLocalRef(env, set_cls);
    (*env)->DeleteLocalRef(env, iterator);
    (*env)->DeleteLocalRef(env, key_set);
    (*env)->DeleteLocalRef(env, map_cls);
    (*env)->DeleteLocalRef(env, thread_cls);
    (*env)->DeleteLocalRef(env, traces_map);

    return result;
}

static void *injection_worker(void *arg) {
    (void)arg;

    log_write("=== inject_jni worker thread started ===");

    struct timespec t_start, t_phase;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    t_phase = t_start;

    // ── Phase 1: locate JNI_GetCreatedJavaVMs symbol ────────────────────
    //
    // We can NOT dlopen("libjvm.dylib", RTLD_NOLOAD) — on macOS the Zulu JRE
    // loads libjvm from a bundled path inside ~/.lunarclient/jre/..., and
    // dlopen with just a leaf name can't find it even with RTLD_NOLOAD.
    //
    // Instead, poll dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs").  RTLD_DEFAULT
    // searches every already-loaded image in the process (including libjvm),
    // so this works regardless of where libjvm.dylib lives on disk.
    //
    // The dylib constructor runs before main(), so libjvm may not be loaded
    // yet.  Poll until the symbol appears.

    typedef jint (*JNI_GetCreatedJavaVMs_t)(JavaVM **, jsize, jsize *);
    JNI_GetCreatedJavaVMs_t JNI_GetCreatedJavaVMs_ptr = NULL;
    int attempts = 0;
    const int max_attempts = 120;  // ~60 seconds with 500ms sleep

    log_write("Phase 1: Waiting for JNI_GetCreatedJavaVMs (JVM startup)...");

    while (attempts < max_attempts) {
        JNI_GetCreatedJavaVMs_ptr =
            (JNI_GetCreatedJavaVMs_t)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
        if (JNI_GetCreatedJavaVMs_ptr != NULL) break;
        attempts++;
        usleep(500000);
    }

    if (!JNI_GetCreatedJavaVMs_ptr) {
        log_write("ERROR: JNI_GetCreatedJavaVMs not found after timeout");
        log_write(dlerror());
        FILE *f = fopen(RESULT_PATH, "w");
        if (f) {
            fprintf(f, "ERROR: JNI_GetCreatedJavaVMs symbol never appeared\n");
            fclose(f);
        }
        log_close();
        return NULL;
    }

    {
        struct timespec t_now;
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        double elapsed = (t_now.tv_sec - t_phase.tv_sec)
            + (t_now.tv_nsec - t_phase.tv_nsec) / 1e9;
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "Found JNI_GetCreatedJavaVMs (attempt %d, %.1fs since start)",
                 attempts, elapsed);
        log_write(buf);
        t_phase = t_now;
    }

    // ── Phase 2: wait for JVM to be created ──────────────────────────────

    JavaVM *jvm = NULL;
    jsize n_vms = 0;
    attempts = 0;

    log_write("Phase 2: Waiting for JVM to be created...");

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
        struct timespec t_now;
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        double elapsed = (t_now.tv_sec - t_phase.tv_sec)
            + (t_now.tv_nsec - t_phase.tv_nsec) / 1e9;
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "JVM found (n_vms=%d, attempt %d, %.1fs for JVM startup)",
                 (int)n_vms, attempts, elapsed);
        log_write(buf);
        t_phase = t_now;
    }

    // ── Phase 3: attach current thread to JVM ────────────────────────────

    log_write("Phase 3: Attaching to JVM...");
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

    {
        struct timespec t_now;
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        double elapsed = (t_now.tv_sec - t_phase.tv_sec)
            + (t_now.tv_nsec - t_phase.tv_nsec) / 1e9;
        char buf[128];
        snprintf(buf, sizeof(buf), "Attached to JVM (%.3fs)", elapsed);
        log_write(buf);
        t_phase = t_now;
    }

    // ── Phase 4: find Minecraft class ────────────────────────────────────
    //
    // The JVM starts before Minecraft's classes are loaded.  Poll until the
    // class appears (or timeout).

    jclass found_class = NULL;
    attempts = 0;

    log_write("Phase 4: Looking up Minecraft class (thread-enumeration strategy)...");

    while (attempts < max_attempts && found_class == NULL) {
        JNIEnv *env = attach_to_jvm(jvm);
        if (!env) {
            attempts++;
            usleep(500000);
            continue;
        }

        // Strategy: enumerate ALL Java threads, test each context classloader
        // This catches the "Client thread" which has the correct classloader
        found_class = find_class_via_threads(env, "net.minecraft.client.Minecraft");

        if (found_class == NULL) {
            // Fallback: try known bridge classes' own getClassLoader()
            const char *fallbacks[] = {
                "net.minecraft.launchwrapper.LaunchClassLoader",
                "net.minecraft.launchwrapper.Launch",
                "com.moonsworth.lunar.genesis.Genesis",
                "com.moonsworth.lunar.LunarClient",
                "com.moonsworth.lunar.legacy.IIOOIIHRIOCCIHHCIRICHIIHIHOHII",
                NULL
            };

            jobject loader = get_class_loader(env);
            if (loader) {
                for (int c = 0; fallbacks[c] != NULL && found_class == NULL; c++) {
                    jclass bridge = find_class_with_loader(env, fallbacks[c], loader);
                    if (bridge) {
                        // Try the bridge's own getClassLoader()
                        jclass bridge_meta = (*env)->GetObjectClass(env, bridge);
                        if (bridge_meta) {
                            jmethodID get_cl_mid = (*env)->GetMethodID(
                                env, bridge_meta, "getClassLoader",
                                "()Ljava/lang/ClassLoader;");
                            if (get_cl_mid) {
                                jobject bridge_load = (*env)->CallObjectMethod(
                                    env, bridge, get_cl_mid);
                                if (bridge_load && !check_and_clear_exception(env)) {
                                    found_class = find_class_with_loader(
                                        env, "net.minecraft.client.Minecraft", bridge_load);
                                    if (found_class) {
                                        found_class = (jclass)(*env)->NewGlobalRef(env, found_class);
                                    }
                                    (*env)->DeleteLocalRef(env, bridge_load);
                                } else {
                                    check_and_clear_exception(env);
                                }
                            }
                            (*env)->DeleteLocalRef(env, bridge_meta);
                        }

                        (*env)->DeleteLocalRef(env, bridge);
                    }
                }
                (*env)->DeleteLocalRef(env, loader);
            }
        } else {
            found_class = (jclass)(*env)->NewGlobalRef(env, found_class);
        }

        attempts++;
        if (found_class == NULL) {
            usleep(500000);
        }
    }

    {
        struct timespec t_now;
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        double elapsed_total = (t_now.tv_sec - t_start.tv_sec)
            + (t_now.tv_nsec - t_start.tv_nsec) / 1e9;
        double elapsed_phase = (t_now.tv_sec - t_phase.tv_sec)
            + (t_now.tv_nsec - t_phase.tv_nsec) / 1e9;
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Lookup finished: attempt=%d lookup=%.1fs total=%.1fs",
                 attempts, elapsed_phase, elapsed_total);
        log_write(buf);
        t_phase = t_now;
    }

    // ── Phase 5: write result ────────────────────────────────────────────

    log_write("Phase 5: Writing result...");
    FILE *f = fopen(RESULT_PATH, "w");
    if (f) {
        if (found_class != NULL) {
            fprintf(f, "SUCCESS: Found Minecraft class\n");
            fprintf(f, "  Class:         net.minecraft.client.Minecraft\n");
            fprintf(f, "  Class pointer: %p\n", (void *)found_class);
            fprintf(f, "  Method:        Thread enumeration → context classloader\n");
            log_write("Result written: SUCCESS");
        } else {
            fprintf(f, "FAILURE: Could not find net.minecraft.client.Minecraft\n");
            fprintf(f, "  Strategies attempted:\n");
            fprintf(f, "    1. Enumerate ALL Java threads' context classloaders\n");
            fprintf(f, "    2. System classloader → Launch/Genesis bridge classes\n");
            fprintf(f, "    3. FindClass direct (bootstrap classloader)\n");
            fprintf(f, "  Check /tmp/inject_log.txt for detailed debug info.\n");
            log_write("Result written: FAILURE");
        }
        fclose(f);
    } else {
        log_write("ERROR: Could not write result file");
    }

    // ── Done ──
    {
        struct timespec t_now;
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        double elapsed = (t_now.tv_sec - t_start.tv_sec)
            + (t_now.tv_nsec - t_start.tv_nsec) / 1e9;
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "=== inject_jni worker thread finished (total %.1fs) ===", elapsed);
        log_write(buf);
    }
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
