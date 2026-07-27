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

/* Get value of a Field on an object. Returns local-ref or NULL. */
static jobject field_get(JNIEnv *env, jobject field, jobject obj) {
    jclass field_cls = (*env)->GetObjectClass(env, field);
    if (!field_cls) return NULL;
    jmethodID mid = (*env)->GetMethodID(env, field_cls, "get",
                                        "(Ljava/lang/Object;)Ljava/lang/Object;");
    if (!mid) { (*env)->DeleteLocalRef(env, field_cls); return NULL; }
    jobject val = (*env)->CallObjectMethod(env, field, mid, obj);
    if (check_exc(env)) val = NULL;
    (*env)->DeleteLocalRef(env, field_cls);
    return val;
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

/* Make an AccessibleObject accessible (so we can read private members) */
static void set_accessible(JNIEnv *env, jobject ao) {
    jclass ao_cls = (*env)->GetObjectClass(env, ao);
    jmethodID mid = (*env)->GetMethodID(env, ao_cls, "setAccessible", "(Z)V");
    if (mid) (*env)->CallVoidMethod(env, ao, mid, JNI_TRUE);
    check_exc(env);
    (*env)->DeleteLocalRef(env, ao_cls);
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
    /* Entity fields: discovered as java.lang.reflect.Field objects,
       held as global refs; we use Field.getDouble() or Field.getFloat() */
    jobject   posX_field;
    jobject   posY_field;
    jobject   posZ_field;
    jobject   rotationYaw_field;
    jobject   rotationPitch_field;
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
        int match = (*env)->IsSameObject(env, ft, target_type_cls);
        (*env)->DeleteLocalRef(env, ft);
        if (!match) {
            (*env)->DeleteLocalRef(env, field);
            continue;
        }
        char *name = get_member_name(env, field);
        snprintf(lbuf, sizeof(lbuf),
                 "  Discovered field: %s (type match)", name ? name : "(null)");
        step_log(lbuf);
        if (out_name) *out_name = name;
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

    /* --- 2. Discover getMinecraft() static method --- */
    STEP("Discovering getMinecraft() static method...");
    const char *getmc_name = NULL;
    jobject getmc_method = discover_static_0arg_rettype(env, mc_cls, mc_cls, &getmc_name);
    if (!getmc_method) {
        step_log("ERROR: could not discover getMinecraft() method");
        return -1;
    }

    /* Invoke it */
    jobject mc_instance = method_invoke(env, getmc_method, NULL, NULL);
    if (!mc_instance) {
        step_log("ERROR: getMinecraft() invocation returned null");
        (*env)->DeleteLocalRef(env, getmc_method);
        return -1;
    }
    g_disc.mc_instance = (*env)->NewGlobalRef(env, mc_instance);
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "getMinecraft() = %p (method name: %s)",
                 (void *)mc_instance, getmc_name ? getmc_name : "?");
        step_log(buf);
    }
    step_log(getmc_name ? getmc_name : "getMinecraft name unknown");

    /* Build the JNI method ID for future direct calls */
    jclass mc_inst_cls = (*env)->GetObjectClass(env, mc_instance);
    g_disc.getMinecraft_mid = (*env)->GetStaticMethodID(
        env, mc_inst_cls, getmc_name, "()Lnet/minecraft/client/Minecraft;");
    (*env)->DeleteLocalRef(env, mc_inst_cls);
    free((void *)getmc_name);
    (*env)->DeleteLocalRef(env, getmc_method);

    /* --- 3. Find World class --- */
    STEP("Finding net.minecraft.world.World...");
    jclass world_cls = find_class(env, "net.minecraft.world.World", sys_loader);
    if (!world_cls) { step_log("ERROR: cannot find World class"); return -1; }
    g_disc.world_class = (jclass)(*env)->NewGlobalRef(env, world_cls);

    /* --- 4. Discover theWorld field --- */
    STEP("Discovering theWorld field (type World)...");
    const char *world_fname = NULL;
    jobject world_field = discover_field_by_type(env, mc_cls, world_cls, &world_fname);
    if (!world_field) {
        step_log("ERROR: could not discover theWorld field");
        return -1;
    }
    set_accessible(env, world_field);
    jobject theWorld = field_get(env, world_field, mc_instance);
    if (!theWorld) {
        step_log("ERROR: theWorld field is null");
        (*env)->DeleteLocalRef(env, world_field);
        return -1;
    }
    g_disc.theWorld_fid = (*env)->GetFieldID(env, mc_cls, world_fname,
                                              "Lnet/minecraft/world/World;");
    (*env)->DeleteLocalRef(env, theWorld); /* will re-get each frame */
    free((void *)world_fname);
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
        jobject player_field = discover_field_by_type(env, mc_cls, ep_cls, &player_fname);
        if (player_field) {
            set_accessible(env, player_field);
            g_disc.thePlayer_fid = (*env)->GetFieldID(env, mc_cls, player_fname,
                "Lnet/minecraft/entity/player/EntityPlayer;");
            free((void *)player_fname);
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
        set_accessible(env, pelist_field);
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

                if (is_double) {
                    char *fname = get_member_name(env, f);
                    if (fname) {
                        set_accessible(env, f);
                        if (strstr(fname, "X") && !g_disc.posX_field)
                            { g_disc.posX_field = (*env)->NewGlobalRef(env, f);
                              step_log("posX → global ref"); }
                        else if (strstr(fname, "Y") && !g_disc.posY_field)
                            { g_disc.posY_field = (*env)->NewGlobalRef(env, f);
                              step_log("posY → global ref"); }
                        else if (strstr(fname, "Z") && !g_disc.posZ_field)
                            { g_disc.posZ_field = (*env)->NewGlobalRef(env, f);
                              step_log("posZ → global ref"); }
                        free(fname);
                    }
                }
                if (is_float) {
                    char *fname = get_member_name(env, f);
                    if (fname) {
                        set_accessible(env, f);
                        if ((strstr(fname, "Yaw") || strstr(fname, "yaw"))
                            && !g_disc.rotationYaw_field)
                            { g_disc.rotationYaw_field = (*env)->NewGlobalRef(env, f);
                              step_log("rotationYaw → global ref"); }
                        else if ((strstr(fname, "Pitch") || strstr(fname, "pitch"))
                                 && !g_disc.rotationPitch_field)
                            { g_disc.rotationPitch_field = (*env)->NewGlobalRef(env, f);
                              step_log("rotationPitch → global ref"); }
                        free(fname);
                    }
                }
                free(tname);
                (*env)->DeleteLocalRef(env, f);
            }
            (*env)->DeleteLocalRef(env, efields);
        }
    }

    if (!g_disc.posX_field || !g_disc.posY_field || !g_disc.posZ_field) {
        step_log("ERROR: could not discover one or more position fields");
        step_log("  posX_field, posY_field, posZ_field may be NULL");
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

/* Get entity position via Field.getDouble() (reflection, not direct JNI) */
static int entity_get_pos(JNIEnv *env, jobject entity,
                           double *x, double *y, double *z) {
    if (!entity || !g_disc.posX_field || !g_disc.posY_field || !g_disc.posZ_field)
        return -1;

    jclass field_cls = (*env)->FindClass(env, "java/lang/reflect/Field");
    jmethodID get_double = (*env)->GetMethodID(env, field_cls, "getDouble",
                                                "(Ljava/lang/Object;)D");
    if (!get_double) { check_exc(env); (*env)->DeleteLocalRef(env, field_cls); return -1; }

    *x = (*env)->CallDoubleMethod(env, g_disc.posX_field, get_double, entity);
    *y = (*env)->CallDoubleMethod(env, g_disc.posY_field, get_double, entity);
    *z = (*env)->CallDoubleMethod(env, g_disc.posZ_field, get_double, entity);
    check_exc(env);
    (*env)->DeleteLocalRef(env, field_cls);
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
    step_clear();
    step_log("=== Phase 2 worker started ===");

    /* --- Wait for JVM --- */
    typedef jint (*JVM_Finder_t)(JavaVM **, jsize, jsize *);
    JVM_Finder_t JVM_Finder = NULL;
    int attempts = 0;
    JavaVM *jvm = NULL;
    jsize n_vms = 0;

    STEP("Waiting for JVM...");
    while (!JVM_Finder && attempts < 120) {
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wpedantic"
        JVM_Finder = (JVM_Finder_t)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
        #pragma GCC diagnostic pop
        if (!JVM_Finder) { attempts++; usleep(500000); }
    }
    if (!JVM_Finder) { step_log("ERROR: JVM symbol not found"); return NULL; }
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
    if (n_vms == 0) { step_log("ERROR: no JVM created after 60s polling"); return NULL; }
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
                (g_disc.posX_field && g_disc.posY_field && g_disc.posZ_field)
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
