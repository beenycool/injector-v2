/*
 * cheat_phase2.c — Lunar Client 1.8.9 JNI Injection + ESP Overlay (macOS arm64)
 *
 * Phase 2: Runtime JNI reflection to discover obfuscated game internals,
 *           fishhook to intercept OpenGL frame flushes, and basic ESP rendering.
 *
 * Strategy:
 *   1. Inject via DYLD_INSERT_LIBRARIES (Phase 1 proven working)
 *   2. Wait for JVM, attach, find net.minecraft.client.Minecraft
 *   3. Use Java reflection (getDeclaredMethods/getDeclaredFields) to discover
 *      actual names of getMinecraft(), theWorld, thePlayer, posX/Y/Z, etc.
 *      This is robust against Lunar's obfuscation/remapping.
 *   4. Hook CGLFlushDrawable via fishhook to intercept every frame
 *   5. On each frame: save GL state → ortho overlay → project players → draw ESP → restore
 *
 * Build:
 *   clang -dynamiclib \
 *     -I/opt/homebrew/opt/openjdk@17/include \
 *     -I/opt/homebrew/opt/openjdk@17/include/darwin \
 *     -framework OpenGL \
 *     -o cheat_phase2.dylib cheat_phase2.c
 *   codesign --force --sign - cheat_phase2.dylib
 *
 * Launch:
 *   DYLD_INSERT_LIBRARIES=/path/to/cheat_phase2.dylib \
 *   DYLD_FORCE_FLAT_NAMESPACE=1 \
 *   ./launch_lunar.sh
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

/* OpenGL */
#include <OpenGL/gl.h>
#include <OpenGL/OpenGL.h>

/* JNI */
#include <jni.h>

/* ───────────────────────────────────────────────────────────────────────────
 * Logging — writes to file AND stderr so we always see output.
 * Uses fopen+fclose each call (no stale handles).
 * ─────────────────────────────────────────────────────────────────────────── */
#define LOG_PATH    "/tmp/phase2_step.txt"
#define RESULT_PATH "/tmp/phase2_result.txt"

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void step_log(const char *msg) {
    pthread_mutex_lock(&g_log_mutex);

    /* stderr — always works, visible in terminal */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    fprintf(stderr, "[%02d:%02d:%02d] %s\n",
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, msg);
    fflush(stderr);

    /* file */
    FILE *f = fopen(LOG_PATH, "a");
    if (f) {
        fprintf(f, "[%02d:%02d:%02d] %s\n",
                tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, msg);
        fflush(f);
        fclose(f);
    }

    pthread_mutex_unlock(&g_log_mutex);
}

static void step_clear(void) {
    FILE *f = fopen(LOG_PATH, "w");
    if (f) {
        fclose(f);
        fprintf(stderr, "[*] Log cleared: %s\n", LOG_PATH);
        fflush(stderr);
    }
}

/* Write a milestone marker using write(2) for crash-proof durability.
 * Even if the process dies immediately after, this is on disk. */
static void milestone(const char *tag) {
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s\n", tag);
    (void)!write(fd, buf, (size_t)n);
    fsync(fd);
    close(fd);
}

#define STEP(msg) do { step_log("STEP: " msg); } while(0)

/* ───────────────────────────────────────────────────────────────────────────
 * JNI Helpers
 * ─────────────────────────────────────────────────────────────────────────── */

static int check_exc(JNIEnv *env) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        return 1;
    }
    return 0;
}

/* Same but silent — used during polling where ClassNotFoundException is expected */
static int check_exc_quiet(JNIEnv *env) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 1;
    }
    return 0;
}

/* Call Java reflection: Class.getDeclaredMethods() → Method[] */
static jobjectArray get_declared_methods(JNIEnv *env, jclass cls) {
    jclass class_cls = (*env)->FindClass(env, "java/lang/Class");
    if (!class_cls) { check_exc(env); return NULL; }
    jmethodID mid = (*env)->GetMethodID(env, class_cls,
        "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    if (!mid) { check_exc(env); (*env)->DeleteLocalRef(env, class_cls); return NULL; }
    jobjectArray arr = (jobjectArray)(*env)->CallObjectMethod(env, cls, mid);
    if (check_exc(env)) arr = NULL;
    (*env)->DeleteLocalRef(env, class_cls);
    return arr;
}

/* Call Java reflection: Class.getDeclaredFields() → Field[] */
static jobjectArray get_declared_fields(JNIEnv *env, jclass cls) {
    jclass class_cls = (*env)->FindClass(env, "java/lang/Class");
    if (!class_cls) { check_exc(env); return NULL; }
    jmethodID mid = (*env)->GetMethodID(env, class_cls,
        "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    if (!mid) { check_exc(env); (*env)->DeleteLocalRef(env, class_cls); return NULL; }
    jobjectArray arr = (jobjectArray)(*env)->CallObjectMethod(env, cls, mid);
    if (check_exc(env)) arr = NULL;
    (*env)->DeleteLocalRef(env, class_cls);
    return arr;
}

/* Get the name of a Method or Field as C string. Caller must free(). */
static char *get_member_name(JNIEnv *env, jobject member) {
    jclass member_cls = (*env)->GetObjectClass(env, member);
    if (!member_cls) return NULL;
    jmethodID mid = (*env)->GetMethodID(env, member_cls, "getName",
                                        "()Ljava/lang/String;");
    if (!mid) { (*env)->DeleteLocalRef(env, member_cls); return NULL; }
    jstring jstr = (jstring)(*env)->CallObjectMethod(env, member, mid);
    if (!jstr) { (*env)->DeleteLocalRef(env, member_cls); return NULL; }
    const char *utf = (*env)->GetStringUTFChars(env, jstr, NULL);
    char *result = utf ? strdup(utf) : NULL;
    if (utf) (*env)->ReleaseStringUTFChars(env, jstr, utf);
    (*env)->DeleteLocalRef(env, jstr);
    (*env)->DeleteLocalRef(env, member_cls);
    return result;
}

/* Get the return type of a Method as a jclass */
static jclass method_return_type(JNIEnv *env, jobject method) {
    jclass method_cls = (*env)->GetObjectClass(env, method);
    if (!method_cls) return NULL;
    jmethodID mid = (*env)->GetMethodID(env, method_cls, "getReturnType",
                                        "()Ljava/lang/Class;");
    if (!mid) { (*env)->DeleteLocalRef(env, method_cls); return NULL; }
    jclass ret = (jclass)(*env)->CallObjectMethod(env, method, mid);
    if (check_exc(env)) ret = NULL;
    (*env)->DeleteLocalRef(env, method_cls);
    return ret;
}

/* Check if a Method is static via Modifier.isStatic(getModifiers()) */
static int method_is_static(JNIEnv *env, jobject method) {
    jclass method_cls = (*env)->GetObjectClass(env, method);
    if (!method_cls) return 0;

    /* Call method.getModifiers() → int */
    jmethodID mods_mid = (*env)->GetMethodID(env, method_cls, "getModifiers", "()I");
    if (!mods_mid) { (*env)->DeleteLocalRef(env, method_cls); return 0; }
    jint mods = (*env)->CallIntMethod(env, method, mods_mid);
    (*env)->DeleteLocalRef(env, method_cls);

    /* java.lang.reflect.Modifier.isStatic(mods) → boolean */
    jclass mod_cls = (*env)->FindClass(env, "java/lang/reflect/Modifier");
    if (!mod_cls) { check_exc_quiet(env); return 0; }
    jmethodID is_static_mid = (*env)->GetStaticMethodID(env, mod_cls,
        "isStatic", "(I)Z");
    if (!is_static_mid) { (*env)->DeleteLocalRef(env, mod_cls); return 0; }
    jboolean result = (*env)->CallStaticBooleanMethod(env, mod_cls, is_static_mid, mods);
    (*env)->DeleteLocalRef(env, mod_cls);
    return result;
}

/* Get parameter types of a Method → Class[] */
static jobjectArray method_param_types(JNIEnv *env, jobject method) {
    jclass method_cls = (*env)->GetObjectClass(env, method);
    if (!method_cls) return NULL;
    jmethodID mid = (*env)->GetMethodID(env, method_cls, "getParameterTypes",
                                        "()[Ljava/lang/Class;");
    if (!mid) { (*env)->DeleteLocalRef(env, method_cls); return NULL; }
    jobjectArray arr = (jobjectArray)(*env)->CallObjectMethod(env, method, mid);
    if (check_exc(env)) arr = NULL;
    (*env)->DeleteLocalRef(env, method_cls);
    return arr;
}

/* Get the type of a Field as a jclass */
static jclass field_type(JNIEnv *env, jobject field) {
    jclass field_cls = (*env)->GetObjectClass(env, field);
    if (!field_cls) return NULL;
    jmethodID mid = (*env)->GetMethodID(env, field_cls, "getType",
                                        "()Ljava/lang/Class;");
    if (!mid) { (*env)->DeleteLocalRef(env, field_cls); return NULL; }
    jclass t = (jclass)(*env)->CallObjectMethod(env, field, mid);
    if (check_exc(env)) t = NULL;
    (*env)->DeleteLocalRef(env, field_cls);
    return t;
}

/* Call Method.invoke(obj, args) */
static jobject method_invoke(JNIEnv *env, jobject method, jobject obj,
                              jobjectArray args) {
    jclass method_cls = (*env)->GetObjectClass(env, method);
    if (!method_cls) return NULL;
    jmethodID mid = (*env)->GetMethodID(env, method_cls, "invoke",
        "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    if (!mid) { (*env)->DeleteLocalRef(env, method_cls); return NULL; }
    jobject ret = (*env)->CallObjectMethod(env, method, mid, obj, args);
    if (check_exc(env)) ret = NULL;
    (*env)->DeleteLocalRef(env, method_cls);
    return ret;
}

/* Get Class.getName() */
static char *class_get_name(JNIEnv *env, jclass cls) {
    jclass class_cls = (*env)->FindClass(env, "java/lang/Class");
    if (!class_cls) return NULL;
    jmethodID mid = (*env)->GetMethodID(env, class_cls, "getName",
                                        "()Ljava/lang/String;");
    if (!mid) { (*env)->DeleteLocalRef(env, class_cls); return NULL; }
    jstring jstr = (jstring)(*env)->CallObjectMethod(env, cls, mid);
    if (!jstr) { (*env)->DeleteLocalRef(env, class_cls); return NULL; }
    const char *utf = (*env)->GetStringUTFChars(env, jstr, NULL);
    char *result = utf ? strdup(utf) : NULL;
    if (utf) (*env)->ReleaseStringUTFChars(env, jstr, utf);
    (*env)->DeleteLocalRef(env, jstr);
    (*env)->DeleteLocalRef(env, class_cls);
    return result;
}

/* Convert a dotted class name to JNI descriptor: "net.minecraft.world.World" → "Lnet/minecraft/world/World;" */
static char *class_to_descriptor(const char *dotted) {
    if (!dotted) return NULL;
    size_t len = strlen(dotted);
    char *desc = malloc(len + 3);
    if (!desc) return NULL;
    desc[0] = 'L';
    memcpy(desc + 1, dotted, len);
    desc[len + 1] = ';';
    desc[len + 2] = '\0';
    /* Convert dots to slashes */
    for (char *p = desc + 1; *p && *p != ';'; p++) {
        if (*p == '.') *p = '/';
    }
    return desc;
}

/* Attach current native thread to JVM */
static JNIEnv *attach_to_jvm(JavaVM *jvm) {
    JNIEnv *env = NULL;
    jint rs = (*jvm)->GetEnv(jvm, (void **)&env, JNI_VERSION_1_6);
    if (rs == JNI_EDETACHED) {
        rs = (*jvm)->AttachCurrentThread(jvm, (void **)&env, NULL);
        if (rs != JNI_OK) return NULL;
    } else if (rs != JNI_OK) {
        return NULL;
    }
    return env;
}

/* Find a context classloader that can load Minecraft.
 * Native pthreads have null context classloader, so we enumerate ALL
 * JVM threads and try each one's classloader.
 * IMPORTANT: We poll with a timeout because Minecraft classes may not be
 * loaded yet when our worker starts (Lunar loads them ~1s after JVM init). */
/* Class.forName(name, true, loader) — forward declaration */
static jclass find_class(JNIEnv *env, const char *name, jobject loader);
static jobject get_context_classloader(JNIEnv *env) {
    char lbuf[128];
    int max_attempts = 120; /* 60 seconds at 500ms */
    int attempt = 0;

    while (attempt < max_attempts) {
        if (attempt == 0)
            step_log("Enumerating threads for classloader (will retry if Minecraft not loaded yet)...");

        jclass thread_cls = (*env)->FindClass(env, "java/lang/Thread");
        if (!thread_cls) { check_exc(env); usleep(500000); attempt++; continue; }

        jmethodID all_mid = (*env)->GetStaticMethodID(env, thread_cls,
            "getAllStackTraces", "()Ljava/util/Map;");
        if (!all_mid) { check_exc(env); (*env)->DeleteLocalRef(env, thread_cls); usleep(500000); attempt++; continue; }

        jobject map = (*env)->CallStaticObjectMethod(env, thread_cls, all_mid);
        if (!map || check_exc(env)) { (*env)->DeleteLocalRef(env, thread_cls); usleep(500000); attempt++; continue; }

        jclass map_cls = (*env)->FindClass(env, "java/util/Map");
        jmethodID keyset_mid = (*env)->GetMethodID(env, map_cls, "keySet", "()Ljava/util/Set;");
        jobject key_set = (*env)->CallObjectMethod(env, map, keyset_mid);
        (*env)->DeleteLocalRef(env, map_cls);
        if (!key_set || check_exc(env)) { (*env)->DeleteLocalRef(env, map); (*env)->DeleteLocalRef(env, thread_cls); usleep(500000); attempt++; continue; }

        jclass set_cls = (*env)->FindClass(env, "java/util/Set");
        jmethodID toarray_mid = (*env)->GetMethodID(env, set_cls, "toArray", "()[Ljava/lang/Object;");
        jobjectArray threads = (jobjectArray)(*env)->CallObjectMethod(env, key_set, toarray_mid);
        (*env)->DeleteLocalRef(env, set_cls);
        (*env)->DeleteLocalRef(env, key_set);
        if (!threads || check_exc(env)) { (*env)->DeleteLocalRef(env, map); (*env)->DeleteLocalRef(env, thread_cls); usleep(500000); attempt++; continue; }

        jmethodID getloader_mid = (*env)->GetMethodID(env, thread_cls,
            "getContextClassLoader", "()Ljava/lang/ClassLoader;");
        if (!getloader_mid) { (*env)->DeleteLocalRef(env, thread_cls); usleep(500000); attempt++; continue; }

        jsize n = (*env)->GetArrayLength(env, threads);
        if (attempt == 0) {
            snprintf(lbuf, sizeof(lbuf), "  %d threads — polling for Minecraft class...", (int)n);
            step_log(lbuf);
        }

        for (jsize i = 0; i < n; i++) {
            jobject thread = (*env)->GetObjectArrayElement(env, threads, i);
            if (!thread) continue;
            jobject loader = (*env)->CallObjectMethod(env, thread, getloader_mid);
            check_exc_quiet(env); /* null or exception is expected for some threads */
            if (loader != NULL) {
                jclass test = find_class(env, "net.minecraft.client.Minecraft", loader);
                if (test != NULL) {
                    (*env)->DeleteLocalRef(env, test);
                    snprintf(lbuf, sizeof(lbuf), "Thread #%d classloader ready (attempt %d)", (int)i, attempt);
                    step_log(lbuf);
                    /* Clean up and return loader */
                    (*env)->DeleteLocalRef(env, thread);
                    (*env)->DeleteLocalRef(env, map);
                    (*env)->DeleteLocalRef(env, thread_cls);
                    (*env)->DeleteLocalRef(env, threads);
                    return loader;
                }
                (*env)->DeleteLocalRef(env, loader);
            }
            (*env)->DeleteLocalRef(env, thread);
        }

        (*env)->DeleteLocalRef(env, map);
        (*env)->DeleteLocalRef(env, thread_cls);
        (*env)->DeleteLocalRef(env, threads);

        attempt++;
        if (attempt > 0 && attempt % 10 == 0) {
            snprintf(lbuf, sizeof(lbuf), "  still polling (attempt %d/%d)...", attempt, max_attempts);
            step_log(lbuf);
        }
        usleep(500000); /* 500ms between retries */
    }

    step_log("Timed out waiting for Minecraft classloader (60s)");
    return NULL;
}

/* Class.forName(name, true, loader) */
static jclass find_class(JNIEnv *env, const char *name, jobject loader) {
    jclass class_cls = (*env)->FindClass(env, "java/lang/Class");
    if (!class_cls) { check_exc(env); return NULL; }
    jmethodID mid = (*env)->GetStaticMethodID(env, class_cls, "forName",
        "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    if (!mid) { (*env)->DeleteLocalRef(env, class_cls); return NULL; }
    jstring jname = (*env)->NewStringUTF(env, name);
    if (!jname) { (*env)->DeleteLocalRef(env, class_cls); return NULL; }
    jclass result = (jclass)(*env)->CallStaticObjectMethod(
        env, class_cls, mid, jname, JNI_TRUE, loader);
    check_exc_quiet(env); /* ClassNotFoundException expected during polling */
    (*env)->DeleteLocalRef(env, jname);
    (*env)->DeleteLocalRef(env, class_cls);
    return result;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Dynamic Discovery — method/field by return type / field type
 *
 * All discovered names and signatures are logged so you can inspect them.
 * ─────────────────────────────────────────────────────────────────────────── */

/* Cache: discovered JNI IDs (populated by discovery, used at frame time) */
static struct {
    jmethodID getMinecraft_mid;
    jfieldID  theWorld_fid;
    jfieldID  thePlayer_fid;
    jfieldID  playerEntities_fid;
    /* Entity field IDs (direct JNI access, no setAccessible needed) */
    jfieldID  posX_fid;
    jfieldID  posY_fid;
    jfieldID  posZ_fid;
    jfieldID  rotationYaw_fid;
    jfieldID  rotationPitch_fid;
    /* Classes */
    jclass    minecraft_class;   /* global ref */
    jclass    world_class;       /* global ref */
    jclass    entity_class;      /* global ref */
    jclass    entityplayer_class;/* global ref */
    /* Instances (refreshed each frame) */
    jobject   mc_instance;       /* global ref, refreshed */
    jobject   theWorld_instance; /* local ref each frame */
    jobject   thePlayer_instance;/* local ref each frame */
} g_disc;

/* Find a static method that returns type 'ret_cls' and takes 0 params */
static jobject discover_static_0arg_rettype(JNIEnv *env, jclass cls,
                                             jclass ret_cls,
                                             const char **out_name) {
    jobjectArray methods = get_declared_methods(env, cls);
    if (!methods) return NULL;
    jsize n = (*env)->GetArrayLength(env, methods);
    char lbuf[512];

    for (jsize i = 0; i < n; i++) {
        jobject method = (*env)->GetObjectArrayElement(env, methods, i);
        if (!method) continue;

        /* Check return type */
        jclass ret = method_return_type(env, method);
        if (!ret) {
            (*env)->DeleteLocalRef(env, method);
            continue;
        }
        int ret_match = (*env)->IsSameObject(env, ret, ret_cls);
        (*env)->DeleteLocalRef(env, ret);
        if (!ret_match) {
            (*env)->DeleteLocalRef(env, method);
            continue;
        }

        /* Check 0 parameters */
        jobjectArray params = method_param_types(env, method);
        if (!params) {
            (*env)->DeleteLocalRef(env, method);
            continue;
        }
        jsize pcount = (*env)->GetArrayLength(env, params);
        (*env)->DeleteLocalRef(env, params);
        if (pcount != 0) {
            (*env)->DeleteLocalRef(env, method);
            continue;
        }

        /* Found it! */
        char *name = get_member_name(env, method);
        snprintf(lbuf, sizeof(lbuf),
                 "  Discovered method: %s (returns same type, 0 params)", name ? name : "(null)");
        step_log(lbuf);
        if (out_name) *out_name = name;
        (*env)->DeleteLocalRef(env, methods);
        return method; /* caller must DeleteLocalRef */
    }

    (*env)->DeleteLocalRef(env, methods);
    return NULL;
}

/* Find a field whose type matches target_type_cls. Returns Field object. */
static jobject discover_field_by_type(JNIEnv *env, jclass cls,
                                       jclass target_type_cls,
                                       const char **out_name,
                                       char **out_type_name) {
    jobjectArray fields = get_declared_fields(env, cls);
    if (!fields) return NULL;
    jsize n = (*env)->GetArrayLength(env, fields);
    char lbuf[512];

    for (jsize i = 0; i < n; i++) {
        jobject field = (*env)->GetObjectArrayElement(env, fields, i);
        if (!field) continue;
        jclass ft = field_type(env, field);
        if (!ft) {
            (*env)->DeleteLocalRef(env, field);
            continue;
        }
        int match = (*env)->IsAssignableFrom(env, ft, target_type_cls);
        (*env)->DeleteLocalRef(env, ft);
        if (!match) {
            (*env)->DeleteLocalRef(env, field);
            continue;
        }
        char *name = get_member_name(env, field);
        /* Get the actual field type's class name for JNI descriptor */
        jclass ft2 = field_type(env, field);
        char *type_name = ft2 ? class_get_name(env, ft2) : NULL;
        if (ft2) (*env)->DeleteLocalRef(env, ft2);
        snprintf(lbuf, sizeof(lbuf),
                 "  Discovered field: %s (type: %s)", name ? name : "(null)", type_name ? type_name : "?");
        step_log(lbuf);
        if (out_name) *out_name = name;
        if (out_type_name) *out_type_name = type_name; else free(type_name);
        (*env)->DeleteLocalRef(env, fields);
        return field; /* caller must DeleteLocalRef */
    }

    (*env)->DeleteLocalRef(env, fields);
    return NULL;
}

/* Discover a List-type field → used for playerEntities */
static jobject discover_list_field(JNIEnv *env, jclass cls,
                                    const char **out_name) {
    jobjectArray fields = get_declared_fields(env, cls);
    if (!fields) return NULL;
    jsize n = (*env)->GetArrayLength(env, fields);
    char lbuf[512];

    for (jsize i = 0; i < n; i++) {
        jobject field = (*env)->GetObjectArrayElement(env, fields, i);
        if (!field) continue;
        jclass ft = field_type(env, field);
        if (!ft) {
            (*env)->DeleteLocalRef(env, field);
            continue;
        }
        char *tname = class_get_name(env, ft);
        (*env)->DeleteLocalRef(env, ft);
        /* Look for java.util.List */
        int is_list = tname && (
            strcmp(tname, "java.util.List") == 0 ||
            strstr(tname, "List") != NULL);
        if (!is_list) {
            free(tname);
            (*env)->DeleteLocalRef(env, field);
            continue;
        }
        char *name = get_member_name(env, field);
        snprintf(lbuf, sizeof(lbuf),
                 "  Discovered List field: %s (type: %s)", name ? name : "(null)", tname ? tname : "?");
        step_log(lbuf);
        free(tname);
        if (out_name) *out_name = name;
        (*env)->DeleteLocalRef(env, fields);
        return field;
    }

    (*env)->DeleteLocalRef(env, fields);
    return NULL;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Full runtime discovery pipeline
 * ─────────────────────────────────────────────────────────────────────────── */
static int run_discovery(JNIEnv *env) {
    jobject sys_loader = get_context_classloader(env);
    if (!sys_loader) { step_log("ERROR: get_context_classloader failed"); return -1; }
    step_log("Context classloader obtained successfully");

    /* --- 1. Find Minecraft class --- */
    STEP("Finding net.minecraft.client.Minecraft...");
    jclass mc_cls = find_class(env, "net.minecraft.client.Minecraft", sys_loader);
    if (!mc_cls) { step_log("ERROR: cannot find Minecraft class"); return -1; }
    g_disc.minecraft_class = (jclass)(*env)->NewGlobalRef(env, mc_cls);
    char *mc_name = class_get_name(env, mc_cls);
    step_log(mc_name ? mc_name : "Minecraft class found (name unknown)");
    free(mc_name);

    /* --- 2. Discover getMinecraft() static method ---
     * Minecraft has multiple static 0-arg methods returning Minecraft:
     *   newInstance() — deprecated, tries to call private constructor → crashes
     *   getMinecraft() — the real singleton accessor
     * We try ALL candidates, skip ones that throw, use the first that works. */
    STEP("Discovering getMinecraft() static method...");
    {
        jobjectArray methods = get_declared_methods(env, mc_cls);
        if (!methods) { step_log("ERROR: getDeclaredMethods() failed"); return -1; }
        jsize mcount = (*env)->GetArrayLength(env, methods);
        char *getmc_name = NULL;
        jobject getmc_method = NULL;
        jobject mc_instance = NULL;
        char tbuf[256];

        /* Max 2 arrays: throwing candidates (skip permanently) and pending (returned null) */
        #define MAX_PENDING 8
        struct { char *name; jobject method; } pending[MAX_PENDING];
        int npending = 0;

        for (jsize i = 0; i < mcount; i++) {
            jobject method = (*env)->GetObjectArrayElement(env, methods, i);
            if (!method) continue;

            /* Check static */
            int is_static = method_is_static(env, method);
            if (!is_static) { (*env)->DeleteLocalRef(env, method); continue; }

            /* Check return type matches Minecraft */
            jclass ret = method_return_type(env, method);
            if (!ret) { (*env)->DeleteLocalRef(env, method); continue; }
            int match = (*env)->IsSameObject(env, ret, mc_cls);
            (*env)->DeleteLocalRef(env, ret);
            if (!match) { (*env)->DeleteLocalRef(env, method); continue; }

            /* Check 0 parameters */
            jobjectArray params = method_param_types(env, method);
            if (!params) { (*env)->DeleteLocalRef(env, method); continue; }
            jsize pcount = (*env)->GetArrayLength(env, params);
            (*env)->DeleteLocalRef(env, params);
            if (pcount != 0) { (*env)->DeleteLocalRef(env, method); continue; }

            /* Candidate found — try invoking it */
            char *mname = get_member_name(env, method);
            snprintf(tbuf, sizeof(tbuf), "  Trying: %s()...", mname ? mname : "?");
            step_log(tbuf);

            mc_instance = method_invoke(env, method, NULL, NULL);
            if (check_exc_quiet(env)) {
                /* Threw (e.g. newInstance() with private ctor) — skip permanently */
                snprintf(tbuf, sizeof(tbuf), "  %s() threw, skipping permanently", mname ? mname : "?");
                step_log(tbuf);
                free(mname);
                (*env)->DeleteLocalRef(env, method);
                continue;
            }

            if (!mc_instance) {
                /* Returns null — save as pending (singleton not initialized yet) */
                if (npending < MAX_PENDING) {
                    pending[npending].name = mname;
                    pending[npending].method = (*env)->NewGlobalRef(env, method);
                    npending++;
                    snprintf(tbuf, sizeof(tbuf), "  %s() returned null — will retry (pending #%d)",
                             mname ? mname : "?", npending);
                    step_log(tbuf);
                } else {
                    free(mname);
                }
                (*env)->DeleteLocalRef(env, method);
                continue;
            }

            /* Immediate success! */
            snprintf(tbuf, sizeof(tbuf), "  %s() → %p SUCCESS", mname ? mname : "?", (void *)mc_instance);
            step_log(tbuf);
            getmc_name = mname;
            getmc_method = method;
            break;
        }

        /* If no immediate success, poll the pending candidates */
        if (!getmc_method && npending > 0) {
            step_log("No candidate returned immediately — polling pending methods for singleton initialization...");
            int poll_count = 0;
            while (poll_count < 60) {  /* ~30s max */
                usleep(500000);
                poll_count++;
                if (poll_count % 10 == 0) {
                    snprintf(tbuf, sizeof(tbuf), "  poll attempt %d/60...", poll_count);
                    step_log(tbuf);
                }

                for (int p = 0; p < npending; p++) {
                    jobject result = method_invoke(env, pending[p].method, NULL, NULL);
                    if (check_exc_quiet(env)) {
                        /* Now threw? Skip this candidate permanently */
                        snprintf(tbuf, sizeof(tbuf), "  pending[%d] %s() now threw — removing",
                                 p, pending[p].name ? pending[p].name : "?");
                        step_log(tbuf);
                        (*env)->DeleteLocalRef(env, pending[p].method);
                        pending[p].method = NULL;
                        free(pending[p].name);
                        pending[p].name = NULL;
                        continue;
                    }
                    if (result) {
                        /* Got it! */
                        snprintf(tbuf, sizeof(tbuf), "  pending[%d] %s() → %p SUCCESS (after ~%dms)",
                                 p, pending[p].name ? pending[p].name : "?",
                                 (void *)result, poll_count * 500);
                        step_log(tbuf);
                        getmc_name = pending[p].name; pending[p].name = NULL;
                        getmc_method = (*env)->NewLocalRef(env, pending[p].method);
                        mc_instance = result;

                        /* Clean up all pending */
                        for (int q = 0; q < npending; q++) {
                            if (pending[q].method) (*env)->DeleteLocalRef(env, pending[q].method);
                            free(pending[q].name);
                        }
                        (*env)->DeleteLocalRef(env, methods);
                        goto got_mc_method;
                    }
                    (*env)->DeleteLocalRef(env, result);
                }
            }

            /* Poll timeout — clean up */
            step_log("Timed out waiting for singleton");
            for (int p = 0; p < npending; p++) {
                if (pending[p].method) (*env)->DeleteLocalRef(env, pending[p].method);
                free(pending[p].name);
            }
        }

        (*env)->DeleteLocalRef(env, methods);

        if (!getmc_method) {
            step_log("ERROR: no working getMinecraft() candidate found");
            return -1;
        }
    got_mc_method: ;

        g_disc.mc_instance = (*env)->NewGlobalRef(env, mc_instance);
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "getMinecraft() = %p (method name: %s)",
                     (void *)mc_instance, getmc_name ? getmc_name : "?");
            step_log(buf);
        }

        /* Build the JNI method ID for future direct calls */
        jclass mc_inst_cls = (*env)->GetObjectClass(env, mc_instance);
        g_disc.getMinecraft_mid = (*env)->GetStaticMethodID(
            env, mc_inst_cls, getmc_name, "()Lnet/minecraft/client/Minecraft;");
        (*env)->DeleteLocalRef(env, mc_inst_cls);

        (*env)->DeleteLocalRef(env, getmc_method);
        (*env)->DeleteLocalRef(env, mc_instance);
        free(getmc_name);
    }

    /* --- 3. Find World class --- */
    STEP("Finding net.minecraft.world.World...");
    jclass world_cls = find_class(env, "net.minecraft.world.World", sys_loader);
    if (!world_cls) { step_log("ERROR: cannot find World class"); return -1; }
    g_disc.world_class = (jclass)(*env)->NewGlobalRef(env, world_cls);

    /* --- 4. Discover theWorld field --- */
    STEP("Discovering theWorld field (type World)...");
    const char *world_fname = NULL;
    char *world_type_name = NULL;
    jobject world_field = discover_field_by_type(env, mc_cls, world_cls, &world_fname, &world_type_name);
    if (!world_field) {
        step_log("ERROR: could not discover theWorld field");
        return -1;
    }
    /* Build JNI field ID using the ACTUAL field type, not the target type.
     * JNI GetFieldID bypasses Java access control — no setAccessible needed. */
    char *world_desc = world_type_name ? class_to_descriptor(world_type_name) : NULL;
    if (!world_desc) { step_log("ERROR: could not build theWorld descriptor"); free((void *)world_fname); free(world_type_name); (*env)->DeleteLocalRef(env, world_field); return -1; }
    g_disc.theWorld_fid = (*env)->GetFieldID(env, mc_cls, world_fname, world_desc);
    free(world_desc);
    if (!g_disc.theWorld_fid) { step_log("ERROR: GetFieldID for theWorld failed"); free((void *)world_fname); free(world_type_name); (*env)->DeleteLocalRef(env, world_field); return -1; }
    /* Verify: read the value via JNI (no setAccessible needed) */
    jobject theWorld = (*env)->GetObjectField(env, g_disc.mc_instance, g_disc.theWorld_fid);
    if (!theWorld) {
        step_log("ERROR: theWorld field is null");
        (*env)->DeleteLocalRef(env, world_field);
        return -1;
    }
    (*env)->DeleteLocalRef(env, theWorld); /* will re-get each frame */
    free((void *)world_fname);
    free(world_type_name);
    (*env)->DeleteLocalRef(env, world_field);
    step_log("theWorld discovered successfully");

    /* --- 5. Discover thePlayer field --- */
    STEP("Discovering thePlayer field (type EntityPlayer)...");
    jclass ep_cls = find_class(env, "net.minecraft.entity.player.EntityPlayer", sys_loader);
    if (!ep_cls) {
        step_log("WARNING: EntityPlayer class not found, skipping thePlayer discovery");
    } else {
        g_disc.entityplayer_class = (jclass)(*env)->NewGlobalRef(env, ep_cls);
        const char *player_fname = NULL;
        char *player_type_name = NULL;
        jobject player_field = discover_field_by_type(env, mc_cls, ep_cls, &player_fname, &player_type_name);
        if (player_field) {
            char *player_desc = player_type_name ? class_to_descriptor(player_type_name) : NULL;
            if (!player_desc) {
                step_log("WARNING: could not build thePlayer descriptor, using hardcoded sig");
                g_disc.thePlayer_fid = (*env)->GetFieldID(env, mc_cls, player_fname,
                    "Lnet/minecraft/entity/player/EntityPlayer;");
            } else {
                g_disc.thePlayer_fid = (*env)->GetFieldID(env, mc_cls, player_fname, player_desc);
                free(player_desc);
            }
            free((void *)player_fname);
            free(player_type_name);
            (*env)->DeleteLocalRef(env, player_field);
            step_log("thePlayer discovered successfully");
        } else {
            step_log("WARNING: could not discover thePlayer field");
        }
    }

    /* --- 6. Discover playerEntities field (List in World) --- */
    STEP("Discovering playerEntities field (List type in World)...");
    const char *pelist_name = NULL;
    jobject pelist_field = discover_list_field(env, world_cls, &pelist_name);
    if (pelist_field) {
        g_disc.playerEntities_fid = (*env)->GetFieldID(env, world_cls, pelist_name,
                                                        "Ljava/util/List;");
        free((void *)pelist_name);
        (*env)->DeleteLocalRef(env, pelist_field);
        step_log("playerEntities discovered successfully");
    } else {
        step_log("WARNING: could not discover playerEntities field");
    }

    /* --- 7. Find Entity class --- */
    STEP("Finding net.minecraft.entity.Entity...");
    jclass entity_cls = find_class(env, "net.minecraft.entity.Entity", sys_loader);
    if (!entity_cls) {
        step_log("ERROR: cannot find Entity class");
        return -1;
    }
    g_disc.entity_class = (jclass)(*env)->NewGlobalRef(env, entity_cls);

    /* --- 8. Discover posX/Y/Z fields (double) --- */
    STEP("Discovering posX/Y/Z fields (double)...");
    {
        /* Discover position/rotation fields via GetFieldID (direct JNI, no setAccessible needed) */
        jobjectArray efields = get_declared_fields(env, entity_cls);
        if (efields) {
            jsize n = (*env)->GetArrayLength(env, efields);
            for (jsize i = 0; i < n; i++) {
                jobject f = (*env)->GetObjectArrayElement(env, efields, i);
                if (!f) continue;
                jclass ft = field_type(env, f);
                char *tname = ft ? class_get_name(env, ft) : NULL;
                if (ft) (*env)->DeleteLocalRef(env, ft);

                int is_double = tname && strcmp(tname, "double") == 0;
                int is_float  = tname && strcmp(tname, "float") == 0;

                if (is_double || is_float) {
                    char *fname = get_member_name(env, f);
                    if (fname) {
                        const char *sig = is_double ? "D" : "F";
                        jfieldID fid = (*env)->GetFieldID(env, entity_cls, fname, sig);
                        if (fid) {
                            if (is_double) {
                                if (strstr(fname, "X") && !g_disc.posX_fid)
                                    { g_disc.posX_fid = fid; step_log("posX_fid ← found"); }
                                else if (strstr(fname, "Y") && !g_disc.posY_fid)
                                    { g_disc.posY_fid = fid; step_log("posY_fid ← found"); }
                                else if (strstr(fname, "Z") && !g_disc.posZ_fid)
                                    { g_disc.posZ_fid = fid; step_log("posZ_fid ← found"); }
                            } else {
                                if ((strstr(fname, "Yaw") || strstr(fname, "yaw"))
                                    && !g_disc.rotationYaw_fid)
                                    { g_disc.rotationYaw_fid = fid; step_log("rotationYaw_fid ← found"); }
                                else if ((strstr(fname, "Pitch") || strstr(fname, "pitch"))
                                         && !g_disc.rotationPitch_fid)
                                    { g_disc.rotationPitch_fid = fid; step_log("rotationPitch_fid ← found"); }
                            }
                        }
                        free(fname);
                    }
                }
                free(tname);
                (*env)->DeleteLocalRef(env, f);
            }
            (*env)->DeleteLocalRef(env, efields);
        }
    }

    if (!g_disc.posX_fid || !g_disc.posY_fid || !g_disc.posZ_fid) {
        step_log("WARNING: could not discover one or more position fields");
    }

    (*env)->DeleteLocalRef(env, sys_loader);
    (*env)->DeleteLocalRef(env, mc_cls);
    (*env)->DeleteLocalRef(env, world_cls);
    (*env)->DeleteLocalRef(env, entity_cls);

    STEP("Discovery complete");
    return 0;
}

/* ───────────────────────────────────────────────────────────────────────────
 * JNI Getters (fast, uses discovered JNI IDs)
 * ─────────────────────────────────────────────────────────────────────────── */

static jobject jni_get_world(JNIEnv *env) {
    if (!g_disc.mc_instance || !g_disc.theWorld_fid) return NULL;
    return (*env)->GetObjectField(env, g_disc.mc_instance, g_disc.theWorld_fid);
}

static jobject jni_get_player(JNIEnv *env) {
    if (!g_disc.mc_instance || !g_disc.thePlayer_fid) return NULL;
    return (*env)->GetObjectField(env, g_disc.mc_instance, g_disc.thePlayer_fid);
}

static jobject jni_get_player_list(JNIEnv *env, jobject world) {
    if (!world || !g_disc.playerEntities_fid) return NULL;
    return (*env)->GetObjectField(env, world, g_disc.playerEntities_fid);
}

/* Get entity position via JNI GetDoubleField (direct, no setAccessible needed) */
static int entity_get_pos(JNIEnv *env, jobject entity,
                           double *x, double *y, double *z) {
    if (!entity || !g_disc.posX_fid || !g_disc.posY_fid || !g_disc.posZ_fid)
        return -1;

    *x = (*env)->GetDoubleField(env, entity, g_disc.posX_fid);
    *y = (*env)->GetDoubleField(env, entity, g_disc.posY_fid);
    *z = (*env)->GetDoubleField(env, entity, g_disc.posZ_fid);
    return 0;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Fishhook — inline from facebook/fishhook (MIT license)
 * https://github.com/facebook/fishhook
 *
 * Compacted for our use case (arm64 only, macOS).
 * ─────────────────────────────────────────────────────────────────────────── */

#ifdef __LP64__
typedef struct mach_header_64     mach_header_t;
typedef struct segment_command_64 segment_command_t;
typedef struct section_64         section_t;
typedef struct nlist_64           nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT_64
#else
typedef struct mach_header        mach_header_t;
typedef struct segment_command    segment_command_t;
typedef struct section            section_t;
typedef struct nlist              nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT
#endif

#ifndef SEG_DATA_CONST
#define SEG_DATA_CONST  "__DATA_CONST"
#endif

struct rebinding {
    const char *name;
    void       *replacement;
    void      **replaced;
};

struct rebindings_entry {
    struct rebinding *rebindings;
    size_t            rebindings_nel;
    struct rebindings_entry *next;
};

static struct rebindings_entry *g_rebindings_head;

static int prepend_rebindings(struct rebindings_entry **head,
                               struct rebinding rebindings[], size_t nel) {
    struct rebindings_entry *entry = malloc(sizeof(struct rebindings_entry));
    if (!entry) return -1;
    entry->rebindings    = rebindings;
    entry->rebindings_nel = nel;
    entry->next          = *head;
    *head                = entry;
    return 0;
}

static void perform_rebinding_with_section(struct rebindings_entry *rebindings,
                                            section_t *section,
                                            intptr_t slide,
                                            nlist_t *symtab,
                                            char *strtab,
                                            uint32_t *indirect_symtab) {
    uint32_t *indirect_symbol_indices = indirect_symtab + section->reserved1;
    void **indirect_symbol_bindings =
        (void **)((uintptr_t)slide + section->addr);
    for (uint32_t i = 0; i < section->size / sizeof(void *); i++) {
        uint32_t symtab_index = indirect_symbol_indices[i];
        if (symtab_index == INDIRECT_SYMBOL_ABS
            || symtab_index == INDIRECT_SYMBOL_LOCAL) continue;
        uint32_t strtab_offset = symtab[symtab_index].n_un.n_strx;
        char *symbol_name = strtab + strtab_offset;
        /* Skip symbols whose name has changed since they were last bound
           (patch only if replacement function name differs from actual
           symbol name being looked for) — but we'll handle this properly */
        struct rebindings_entry *cur = rebindings;
        while (cur) {
            for (size_t j = 0; j < cur->rebindings_nel; j++) {
                if (strcmp(symbol_name, cur->rebindings[j].name) == 0) {
                    if (cur->rebindings[j].replaced != NULL
                        && indirect_symbol_bindings[i]
                           != cur->rebindings[j].replacement) {
                        *(cur->rebindings[j].replaced) =
                            indirect_symbol_bindings[i];
                    }
                    indirect_symbol_bindings[i] =
                        cur->rebindings[j].replacement;
                    goto symbol_loop;
                }
            }
            cur = cur->next;
        }
    symbol_loop:;
    }
}

static void rebind_symbols_for_image(struct rebindings_entry *rebindings,
                                      const mach_header_t *header,
                                      intptr_t slide) {
    Dl_info info;
    if (dladdr(header, &info) == 0) return;

    uintptr_t cur = (uintptr_t)header + sizeof(mach_header_t);
    segment_command_t *linkedit_segment = NULL;
    struct symtab_command *symtab_cmd   = NULL;
    struct dysymtab_command *dysymtab_cmd = NULL;

    for (uint32_t i = 0; i < header->ncmds;
         i++, cur += ((segment_command_t *)cur)->cmdsize) {
        segment_command_t *seg = (segment_command_t *)cur;
        if (seg->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
            if (strcmp(seg->segname, SEG_LINKEDIT) == 0) {
                linkedit_segment = seg;
            }
        } else if (seg->cmd == LC_SYMTAB) {
            symtab_cmd = (struct symtab_command *)seg;
        } else if (seg->cmd == LC_DYSYMTAB) {
            dysymtab_cmd = (struct dysymtab_command *)seg;
        }
    }

    if (!linkedit_segment || !symtab_cmd || !dysymtab_cmd) return;

    uintptr_t linkedit_base =
        (uintptr_t)slide + linkedit_segment->vmaddr - linkedit_segment->fileoff;
    nlist_t *symtab  = (nlist_t *)(linkedit_base + symtab_cmd->symoff);
    char *strtab     = (char *)(linkedit_base + symtab_cmd->stroff);
    uint32_t *indirect_symtab =
        (uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);

    cur = (uintptr_t)header + sizeof(mach_header_t);
    for (uint32_t i = 0; i < header->ncmds;
         i++, cur += ((segment_command_t *)cur)->cmdsize) {
        segment_command_t *seg = (segment_command_t *)cur;
        if (seg->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
            if (strcmp(seg->segname, SEG_DATA) != 0 &&
                strcmp(seg->segname, SEG_DATA_CONST) != 0) continue;
            for (uint32_t j = 0; j < seg->nsects; j++) {
                section_t *sect = (section_t *)((uintptr_t)seg
                    + sizeof(segment_command_t) + j * sizeof(section_t));
                if ((sect->flags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS
                    || (sect->flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS) {
                    perform_rebinding_with_section(rebindings, sect, slide,
                        symtab, strtab, indirect_symtab);
                }
            }
        }
    }
}

int rebind_symbols(struct rebinding rebindings[], size_t rebindings_nel) {
    int retval = prepend_rebindings(&g_rebindings_head, rebindings, rebindings_nel);
    if (retval < 0) return retval;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        rebind_symbols_for_image(g_rebindings_head,
            (const mach_header_t *)_dyld_get_image_header(i),
            _dyld_get_image_vmaddr_slide(i));
    }
    return retval;
}

/* ───────────────────────────────────────────────────────────────────────────
 * OpenGL Hook — intercept CGLFlushDrawable
 * ─────────────────────────────────────────────────────────────────────────── */

static CGLError (*orig_CGLFlushDrawable)(CGLContextObj) = NULL;
static int g_hook_installed = 0;
static int g_hook_first_call_logged = 0;

/* ESP state: updated every frame from JNI */
static double g_esp_players[128][4]; /* x, y, z, screen_x/y set at frame time */
static int    g_esp_count = 0;
static int    g_screen_w = 0, g_screen_h = 0;

/* --- 3D to 2D projection --- */
static void world_to_screen(double wx, double wy, double wz,
                             double *sx, double *sy) {
    GLfloat modelview[16], projection[16];
    GLint   viewport[4];

    glGetFloatv(GL_MODELVIEW_MATRIX, modelview);
    glGetFloatv(GL_PROJECTION_MATRIX, projection);
    glGetIntegerv(GL_VIEWPORT, viewport);

    /* Modelview transform */
    double mx = modelview[0]*wx  + modelview[4]*wy  + modelview[8]*wz  + modelview[12];
    double my = modelview[1]*wx  + modelview[5]*wy  + modelview[9]*wz  + modelview[13];
    double mz = modelview[2]*wx  + modelview[6]*wy  + modelview[10]*wz + modelview[14];
    double mw = modelview[3]*wx  + modelview[7]*wy  + modelview[11]*wz + modelview[15];

    /* Projection transform */
    double px = projection[0]*mx + projection[4]*my + projection[8]*mz  + projection[12]*mw;
    double py = projection[1]*mx + projection[5]*my + projection[9]*mz  + projection[13]*mw;
    double pz = projection[2]*mx + projection[6]*my + projection[10]*mz + projection[14]*mw;
    double pw = projection[3]*mx + projection[7]*my + projection[11]*mz + projection[15]*mw;

    if (pw <= 0.0) { *sx = -999; *sy = -999; return; }

    double ndx = px / pw;
    double ndy = py / pw;

    *sx = viewport[0] + viewport[2] * (ndx + 1.0) / 2.0;
    *sy = viewport[1] + viewport[3] * (1.0 - ndy) / 2.0;
}

/* --- Draw ESP overlay --- */
static void draw_esp_overlay(void) {
    if (g_esp_count == 0) return;

    /* Draw each player box */
    for (int p = 0; p < g_esp_count; p++) {
        double x = g_esp_players[p][0];
        double y = g_esp_players[p][1];
        double z = g_esp_players[p][2];

        double top_sx, top_sy, bot_sx, bot_sy;
        world_to_screen(x, y + 1.8, z, &top_sx, &top_sy);
        world_to_screen(x, y,       z, &bot_sx, &bot_sy);

        if (top_sx < 0 || top_sy < 0 || bot_sx < 0 || bot_sy < 0) continue;

        double h = fabs(bot_sy - top_sy);
        double w = h * 0.5;
        double left  = bot_sx - w / 2.0;
        double right = bot_sx + w / 2.0;

        glColor4f(0.0f, 1.0f, 0.0f, 1.0f);
        glLineWidth(1.5f);
        glBegin(GL_LINE_LOOP);
            glVertex2d(left,  top_sy);
            glVertex2d(right, top_sy);
            glVertex2d(right, bot_sy);
            glVertex2d(left,  bot_sy);
        glEnd();
    }
}

/* --- The hook --- */
static CGLError hook_CGLFlushDrawable(CGLContextObj ctx) {
    if (!g_hook_first_call_logged) {
        g_hook_first_call_logged = 1;
        step_log("CGLFlushDrawable hook FIRED — overlay active");
    }

    /* Save OpenGL state */
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();

    /* Set up ortho overlay */
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    g_screen_w = viewport[2];
    g_screen_h = viewport[3];

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, g_screen_w, g_screen_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Draw ESP */
    draw_esp_overlay();

    /* Restore OpenGL state */
    glPopAttrib();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    /* Call original */
    return orig_CGLFlushDrawable(ctx);
}

static int install_hook(void) {
    struct rebinding reb[] = {
        {"CGLFlushDrawable", (void *)hook_CGLFlushDrawable, (void **)&orig_CGLFlushDrawable},
    };
    if (rebind_symbols(reb, sizeof(reb) / sizeof(reb[0])) != 0) {
        step_log("ERROR: rebind_symbols(CGLFlushDrawable) failed");
        return -1;
    }
    g_hook_installed = 1;
    step_log("CGLFlushDrawable hook installed via fishhook");
    return 0;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Frame update — called from the worker thread loop, before frame renders
 * ─────────────────────────────────────────────────────────────────────────── */

static void update_frame_data(JNIEnv *env) {
    g_esp_count = 0;
    if (!g_disc.mc_instance) return;

    jobject world = jni_get_world(env);
    if (!world) return;
    jobject local_player = NULL;
    if (g_disc.thePlayer_fid)
        local_player = jni_get_player(env);

    jobject plist = jni_get_player_list(env, world);
    if (!plist) { (*env)->DeleteLocalRef(env, world); return; }

    /* Get list size */
    jclass list_cls = (*env)->FindClass(env, "java/util/List");
    jmethodID size_mid = list_cls
        ? (*env)->GetMethodID(env, list_cls, "size", "()I") : NULL;
    jmethodID get_mid  = list_cls
        ? (*env)->GetMethodID(env, list_cls, "get", "(I)Ljava/lang/Object;") : NULL;

    if (!size_mid || !get_mid) {
        check_exc(env);
        if (list_cls) (*env)->DeleteLocalRef(env, list_cls);
        (*env)->DeleteLocalRef(env, plist);
        (*env)->DeleteLocalRef(env, world);
        return;
    }

    jint count = (*env)->CallIntMethod(env, plist, size_mid);
    if (count > 128) count = 128;

    for (jint i = 0; i < count && g_esp_count < 128; i++) {
        jobject entity = (*env)->CallObjectMethod(env, plist, get_mid, i);
        if (!entity) continue;

        /* Skip local player */
        if (local_player && (*env)->IsSameObject(env, entity, local_player)) {
            (*env)->DeleteLocalRef(env, entity);
            continue;
        }

        double x, y, z;
        if (entity_get_pos(env, entity, &x, &y, &z) == 0) {
            int idx = g_esp_count++;
            g_esp_players[idx][0] = x;
            g_esp_players[idx][1] = y;
            g_esp_players[idx][2] = z;
            g_esp_players[idx][3] = 0; /* reserved */
        }
        (*env)->DeleteLocalRef(env, entity);
    }

    if (list_cls) (*env)->DeleteLocalRef(env, list_cls);
    (*env)->DeleteLocalRef(env, plist);
    if (local_player) (*env)->DeleteLocalRef(env, local_player);
    (*env)->DeleteLocalRef(env, world);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Worker Thread
 * ─────────────────────────────────────────────────────────────────────────── */

static void *phase2_worker(void *arg) {
    (void)arg;
    milestone("=== Phase 2 worker started ===");
    step_clear();
    step_log("=== Phase 2 worker started ===");

    /* --- Wait for JVM --- */
    typedef jint (*JVM_Finder_t)(JavaVM **, jsize, jsize *);
    JVM_Finder_t JVM_Finder = NULL;
    int attempts = 0;
    JavaVM *jvm = NULL;
    jsize n_vms = 0;

    STEP("Waiting for JVM...");
    milestone("STEP: Waiting for JVM...");
    /* Sleep briefly to let dyld constructor finish and release its lock.
     * Calling dlsym(RTLD_DEFAULT, ...) from a constructor-spawned thread
     * can deadlock on macOS if dyld still holds its internal lock. */
    usleep(200000);
    step_log("  entering dlsym poll loop...");
    while (!JVM_Finder && attempts < 120) {
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wpedantic"
        JVM_Finder = (JVM_Finder_t)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
        #pragma GCC diagnostic pop
        {
            char lbuf[192];
            snprintf(lbuf, sizeof(lbuf), "  dlsym returned %p (attempt %d/120)", (void *)JVM_Finder, attempts);
            step_log(lbuf);
        }
        if (!JVM_Finder) {
            attempts++;
            if (attempts == 1 || attempts % 20 == 0) {
                char lbuf[128];
                snprintf(lbuf, sizeof(lbuf), "  dlsym poll attempt %d/120...", attempts);
                step_log(lbuf);
            }
            usleep(500000);
        }
    }
    step_log("  exited dlsym poll loop");
    if (!JVM_Finder) { milestone("FAIL: dlsym JNI_GetCreatedJavaVMs"); step_log("ERROR: JVM symbol not found"); return NULL; }
    milestone("JVM dlsym OK");
    step_log("dlsym found JNI_GetCreatedJavaVMs, now polling for VMs...");

    attempts = 0;
    while (n_vms == 0 && attempts < 120) {
        JVM_Finder(&jvm, 1, &n_vms);
        if (n_vms == 0) {
            attempts++;
            if (attempts == 1 || attempts % 20 == 0)
                step_log("  still waiting for JVM creation...");
            usleep(500000);
        }
    }
    if (n_vms == 0) { milestone("FAIL: no JVM created"); step_log("ERROR: no JVM created after 60s polling"); return NULL; }
    milestone("JVM ready");
    step_log("JVM ready — attaching thread...");

    /* --- Attach and run discovery --- */
    JNIEnv *env = attach_to_jvm(jvm);
    if (!env) { step_log("ERROR: cannot attach to JVM"); return NULL; }

    STEP("Running runtime discovery...");
    if (run_discovery(env) != 0) {
        step_log("FATAL: discovery failed, check /tmp/phase2_step.txt");
        FILE *rf = fopen(RESULT_PATH, "w");
        if (rf) { fprintf(rf, "FAILURE: discovery failed\n"); fclose(rf); }
        return NULL;
    }

    /* --- Install OpenGL hook --- */
    STEP("Installing OpenGL hook...");
    if (install_hook() != 0) {
        step_log("FATAL: OpenGL hook install failed");
        FILE *rf = fopen(RESULT_PATH, "w");
        if (rf) { fprintf(rf, "FAILURE: hook install failed\n"); fclose(rf); }
        return NULL;
    }

    /* --- Result --- */
    FILE *rf = fopen(RESULT_PATH, "w");
    if (rf) {
        fprintf(rf, "SUCCESS: Phase 2 loaded\n");
        fprintf(rf, "  Minecraft class:    found\n");
        fprintf(rf, "  theWorld:           %s\n", g_disc.theWorld_fid ? "found" : "MISSING");
        fprintf(rf, "  thePlayer:          %s\n", g_disc.thePlayer_fid ? "found" : "MISSING");
        fprintf(rf, "  playerEntities:     %s\n", g_disc.playerEntities_fid ? "found" : "MISSING");
        fprintf(rf, "  posX/Y/Z fields:    %s\n",
                (g_disc.posX_fid && g_disc.posY_fid && g_disc.posZ_fid)
                ? "found" : "MISSING");
        fprintf(rf, "  CGLFlushDrawable hook: installed\n");
        fclose(rf);
    }
    step_log("=== Phase 2 worker: discovery done, hook active ===");

    /* --- Frame update loop (every ~16ms) --- */
    step_log("Starting frame update loop (60Hz-ish)...");
    while (1) {
        JNIEnv *fen = attach_to_jvm(jvm);
        if (fen) {
            update_frame_data(fen);
        }
        usleep(16000); /* ~60 fps update rate */
    }

    return NULL;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Constructor
 * ─────────────────────────────────────────────────────────────────────────── */

__attribute__((constructor))
static void on_load(void) {
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, phase2_worker, NULL);
    pthread_attr_destroy(&attr);
}
