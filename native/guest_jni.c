/* Guest-side JNIEnv and JavaVM for the translated engine.
 *
 * The engine calls JNI through function tables in guest memory.  Each table
 * slot points at a synthetic guest address whose handler is jni_entry().
 *
 * Object references the engine can see:
 *   - one class token, used for every class and for `this` of instance
 *     natives; all Java callbacks resolve on the host GameBridge class;
 *   - strings and byte arrays, stored in the guest heap behind a 16-byte
 *     header so GetStringUTFChars/GetByteArrayElements return guest pointers.
 *
 * Local references follow JNI rules: they die when the outermost native call
 * returns (a5_jni_end_frame) or on DeleteLocalRef.  Freed objects go through
 * a quarantine first, because 3.4.1 reads some strings after deleting them.
 *
 * Resource reads (GLResLoader) run natively on the data directory.  Every
 * other method is forwarded to a static method of GameBridge with the same
 * name and signature.
 */
#include "runtime.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/stat.h>

#define CLASS_TOKEN   0x00001001u
#define METHOD_TAG    0x60000000u
#define TAG_STRING    0x4A535452u
#define TAG_ARRAY     0x4A415252u
#define HEADER        16u
#define STATE_LOCAL   1u
#define STATE_GLOBAL  2u          /* count in bits 1..31 (step 2) */

#define SLOT(name) (offsetof(struct JNINativeInterface, name) / sizeof(void *))
#define SLOT_COUNT (sizeof(struct JNINativeInterface) / sizeof(void *))

static jclass g_bridge;
static jclass g_string_class;
static jclass g_bytes_class;
static _Thread_local JNIEnv *t_env;
static JavaVM *g_vm;

/* ---- guest objects -------------------------------------------------- */

static bool is_object(uint32_t token, uint32_t tag)
{
    return token >= 0x11000000u && token + HEADER <= guest_heap_top() && rd32(token) == tag;
}

/* Per thread: only the GL thread ends frames; guest worker threads keep
 * their (rare) JNI objects alive. */
static _Thread_local uint32_t *local_list;
static _Thread_local size_t local_count, local_cap;

static uint32_t new_object(uint32_t tag, const void *data, uint32_t len)
{
    uint32_t extra = tag == TAG_STRING ? 1u : 0u;
    uint32_t token = guest_alloc((size_t)HEADER + len + extra);
    wr32(token, tag);
    wr32(token + 4, len);
    wr32(token + 8, STATE_LOCAL);
    wr32(token + 12, 0);
    if (data) memcpy(G(token + HEADER), data, len);
    else memset(G(token + HEADER), 0, len);
    if (extra) wr8(token + HEADER + len, 0);
    if (local_count == local_cap) {
        local_cap = local_cap ? local_cap * 2 : 256;
        local_list = realloc(local_list, local_cap * sizeof(*local_list));
        if (!local_list) abort();
    }
    local_list[local_count++] = token;
    return token;
}

uint32_t a5_jni_new_string(const char *utf8)
{
    return new_object(TAG_STRING, utf8, (uint32_t)strlen(utf8));
}

#define QUARANTINE 8192
static uint32_t g_quarantine[QUARANTINE];
static size_t g_q_head, g_q_count;
static uint64_t g_q_bytes;

static pthread_mutex_t g_quarantine_lock = PTHREAD_MUTEX_INITIALIZER;

static void release_object(uint32_t token)
{
    if (rd32(token + 8)) return;   /* still referenced */
    pthread_mutex_lock(&g_quarantine_lock);
    if (g_q_count == QUARANTINE || g_q_bytes > (64u << 20)) {
        uint32_t old = g_quarantine[g_q_head];
        g_q_head = (g_q_head + 1) % QUARANTINE;
        --g_q_count;
        if ((rd32(old) == TAG_STRING || rd32(old) == TAG_ARRAY) && !rd32(old + 8)) {
            g_q_bytes -= rd32(old + 4);
            wr32(old, 0);
            guest_free(old);
        }
    }
    g_quarantine[(g_q_head + g_q_count) % QUARANTINE] = token;
    ++g_q_count;
    g_q_bytes += rd32(token + 4);
    pthread_mutex_unlock(&g_quarantine_lock);
}

static void drop_local(uint32_t token)
{
    uint32_t state = rd32(token + 8);
    if (!(state & STATE_LOCAL)) return;
    wr32(token + 8, state & ~STATE_LOCAL);
    release_object(token);
}

void a5_jni_end_frame(void)
{
    for (size_t i = 0; i < local_count; ++i) {
        uint32_t token = local_list[i];
        if (is_object(token, TAG_STRING) || is_object(token, TAG_ARRAY)) drop_local(token);
    }
    local_count = 0;
}

static const char *string_of(CPU *cpu, uint32_t token)
{
    if (!is_object(token, TAG_STRING)) guest_abort(cpu, "JNI string token 0x%08x", token);
    return (const char *)G(token + HEADER);
}

/* ---- resources (GLResLoader) ---------------------------------------- */

static FILE *open_resource(const char *name, char *path, size_t cap)
{
    while (!strncmp(name, "./", 2)) name += name[2] == '/' ? 3 : 2;
    while (*name == ' ') ++name;
    if (!a5_map_path(name, false, path, cap)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) A5_WARN("resource missing: %s (%s)", name, path);
    return f;
}

static uint32_t resource_read(CPU *cpu, const char *name, int64_t offset, int64_t length)
{
    char path[1024];
    FILE *f = open_resource(name, path, sizeof(path));
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    int64_t size = ftell(f);
    if (length < 0) { offset = 0; length = size; }
    if (offset < 0 || offset > size) { fclose(f); return 0; }
    if (offset + length > size) length = size - offset;
    if (length > 0x10000000) guest_abort(cpu, "resource %s too large", name);
    uint32_t token = new_object(TAG_ARRAY, NULL, (uint32_t)length);
    fseek(f, (long)offset, SEEK_SET);
    size_t got = fread(G(token + HEADER), 1, (size_t)length, f);
    fclose(f);
    if (got != (size_t)length) A5_WARN("short resource read %s", name);
    return token;
}

static int32_t resource_length(const char *name)
{
    char path[1024];
    FILE *f = open_resource(name, path, sizeof(path));
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return (int32_t)size;
}

/* ---- methods -------------------------------------------------------- */

typedef enum { M_JAVA, M_MISSING, M_RES_FULL, M_RES_BYTES, M_RES_LENGTH, M_MOVIE } MethodKind;

/* Set when the engine asks Java to play a full-screen movie.  The host plays
 * nothing yet and releases GS_TrailerMovie after the frame, as the original
 * MyVideoView completion did. */
static bool g_movie_pending;

bool a5_jni_take_movie_request(void)
{
    bool pending = g_movie_pending;
    g_movie_pending = false;
    return pending;
}

typedef struct {
    char name[64];
    char sig[128];
    MethodKind kind;
    jmethodID id;
} Method;

#define MAX_METHODS 256
static Method g_methods[MAX_METHODS];
static uint32_t g_method_count;

static uint32_t find_method(CPU *cpu, uint32_t name_addr, uint32_t sig_addr)
{
    const char *name = a5_guest_cstr(cpu, name_addr);
    const char *sig = a5_guest_cstr(cpu, sig_addr);
    for (uint32_t i = 0; i < g_method_count; ++i) {
        if (!strcmp(g_methods[i].name, name) && !strcmp(g_methods[i].sig, sig)) return METHOD_TAG | i;
    }
    if (g_method_count == MAX_METHODS || strlen(name) >= 64 || strlen(sig) >= 128) {
        guest_abort(cpu, "JNI method table overflow at %s%s", name, sig);
    }
    Method *m = &g_methods[g_method_count];
    snprintf(m->name, sizeof(m->name), "%s", name);
    snprintf(m->sig, sizeof(m->sig), "%s", sig);
    if (!strcmp(name, "getResourceFull") && !strcmp(sig, "(Ljava/lang/String;)[B")) m->kind = M_RES_FULL;
    else if (!strcmp(name, "getResourceBytes") && !strcmp(sig, "(Ljava/lang/String;II)[B")) m->kind = M_RES_BYTES;
    else if (!strcmp(name, "getResourceLength") && !strcmp(sig, "(Ljava/lang/String;)I")) m->kind = M_RES_LENGTH;
    else if (!strcmp(name, "loadMovie") || !strcmp(name, "ResumeMovie")) m->kind = M_MOVIE;
    else {
        JNIEnv *env = t_env;
        m->id = (*env)->GetStaticMethodID(env, g_bridge, name, sig);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        m->kind = m->id ? M_JAVA : M_MISSING;
        if (!m->id) A5_WARN("GameBridge lacks %s%s; calls return 0", name, sig);
    }
    return METHOD_TAG | g_method_count++;
}

static Method *method_of(CPU *cpu, uint32_t token)
{
    uint32_t index = token & ~METHOD_TAG;
    if ((token & 0xF0000000u) != METHOD_TAG || index >= g_method_count) {
        guest_abort(cpu, "JNI method token 0x%08x", token);
    }
    return &g_methods[index];
}

typedef enum { ARGS_VARARGS, ARGS_VA_LIST, ARGS_ARRAY } ArgStyle;

typedef struct {
    ArgStyle style;
    GuestVa va;
    uint32_t array;
} ArgReader;

static uint32_t arg_u32(ArgReader *r)
{
    if (r->style == ARGS_ARRAY) { uint32_t v = rd32(r->array); r->array += 8; return v; }
    return a5_va_u32(&r->va);
}

static uint64_t arg_u64(ArgReader *r)
{
    if (r->style == ARGS_ARRAY) { uint64_t v = rd64(r->array); r->array += 8; return v; }
    return a5_va_u64(&r->va);
}

static float arg_float(ArgReader *r)
{
    if (r->style == ARGS_ARRAY) { uint32_t b = arg_u32(r); return f32_from_bits(b); }
    uint64_t bits = a5_va_u64(&r->va);   /* promoted to double */
    double d;
    memcpy(&d, &bits, 8);
    return (float)d;
}

static const char *skip_type(const char *p)
{
    while (*p == '[') ++p;
    if (*p == 'L') { p = strchr(p, ';'); return p ? p + 1 : ""; }
    return *p ? p + 1 : p;
}

static jobject host_object(CPU *cpu, JNIEnv *env, uint32_t token)
{
    if (!token) return NULL;
    if (token == CLASS_TOKEN) return (*env)->NewLocalRef(env, g_bridge);
    if (is_object(token, TAG_STRING)) return (*env)->NewStringUTF(env, string_of(cpu, token));
    if (is_object(token, TAG_ARRAY)) {
        jsize len = (jsize)rd32(token + 4);
        jbyteArray array = (*env)->NewByteArray(env, len);
        if (array) (*env)->SetByteArrayRegion(env, array, 0, len, (const jbyte *)G(token + HEADER));
        return array;
    }
    guest_abort(cpu, "JNI object token 0x%08x", token);
    return NULL;
}

static uint32_t guest_object(JNIEnv *env, jobject obj)
{
    if (!obj) return 0;
    uint32_t token = CLASS_TOKEN;
    if ((*env)->IsInstanceOf(env, obj, g_string_class)) {
        const char *utf = (*env)->GetStringUTFChars(env, obj, NULL);
        token = a5_jni_new_string(utf ? utf : "");
        if (utf) (*env)->ReleaseStringUTFChars(env, obj, utf);
    } else if ((*env)->IsInstanceOf(env, obj, g_bytes_class)) {
        jsize len = (*env)->GetArrayLength(env, obj);
        token = new_object(TAG_ARRAY, NULL, (uint32_t)len);
        (*env)->GetByteArrayRegion(env, obj, 0, len, (jbyte *)G(token + HEADER));
    }
    (*env)->DeleteLocalRef(env, obj);
    return token;
}

static void set_wide(CPU *cpu, uint64_t v)
{
    cpu->r[0] = (uint32_t)v;
    cpu->r[1] = (uint32_t)(v >> 32);
}

static void call_method(CPU *cpu, ArgStyle style)
{
    Method *m = method_of(cpu, cpu->r[2]);
    ArgReader r = {style, a5_va_regs(cpu, 3), cpu->r[3]};
    if (style == ARGS_VA_LIST) r.va = a5_va_list(cpu->r[3]);

    const char *ret = strchr(m->sig, ')');
    ret = ret ? ret + 1 : "V";
    if (m->kind == M_RES_FULL || m->kind == M_RES_BYTES || m->kind == M_RES_LENGTH) {
        const char *name = string_of(cpu, arg_u32(&r));
        if (m->kind == M_RES_LENGTH) { cpu->r[0] = (uint32_t)resource_length(name); return; }
        int64_t offset = -1, length = -1;
        if (m->kind == M_RES_BYTES) { offset = (int32_t)arg_u32(&r); length = (int32_t)arg_u32(&r); }
        cpu->r[0] = resource_read(cpu, name, offset, length);
        return;
    }
    if (m->kind == M_MISSING) { set_wide(cpu, 0); return; }
    if (m->kind == M_MOVIE) {
        uint32_t name = strchr(m->sig, 'L') ? arg_u32(&r) : 0;
        A5_INFO("movie %s skipped", name ? string_of(cpu, name) : "(resume)");
        g_movie_pending = true;
        return;
    }

    JNIEnv *env = t_env;
    jvalue args[16];
    int count = 0;
    for (const char *p = m->sig + 1; *p && *p != ')'; p = skip_type(p)) {
        if (count == 16) guest_abort(cpu, "JNI call %s has too many arguments", m->name);
        jvalue *a = &args[count++];
        switch (*p) {
        case 'Z': a->z = (jboolean)arg_u32(&r); break;
        case 'B': a->b = (jbyte)arg_u32(&r); break;
        case 'C': a->c = (jchar)arg_u32(&r); break;
        case 'S': a->s = (jshort)arg_u32(&r); break;
        case 'I': a->i = (jint)arg_u32(&r); break;
        case 'F': a->f = arg_float(&r); break;
        case 'J': a->j = (jlong)arg_u64(&r); break;
        case 'D': { uint64_t b = arg_u64(&r); memcpy(&a->d, &b, 8); break; }
        default: a->l = host_object(cpu, env, arg_u32(&r)); break;
        }
    }

    switch (*ret) {
    case 'V': (*env)->CallStaticVoidMethodA(env, g_bridge, m->id, args); break;
    case 'Z': cpu->r[0] = (*env)->CallStaticBooleanMethodA(env, g_bridge, m->id, args); break;
    case 'B': cpu->r[0] = (uint32_t)(int32_t)(*env)->CallStaticByteMethodA(env, g_bridge, m->id, args); break;
    case 'C': cpu->r[0] = (*env)->CallStaticCharMethodA(env, g_bridge, m->id, args); break;
    case 'S': cpu->r[0] = (uint32_t)(int32_t)(*env)->CallStaticShortMethodA(env, g_bridge, m->id, args); break;
    case 'I': cpu->r[0] = (uint32_t)(*env)->CallStaticIntMethodA(env, g_bridge, m->id, args); break;
    case 'J': set_wide(cpu, (uint64_t)(*env)->CallStaticLongMethodA(env, g_bridge, m->id, args)); break;
    case 'F': cpu->r[0] = f32_bits((*env)->CallStaticFloatMethodA(env, g_bridge, m->id, args)); break;
    case 'D': {
        double d = (*env)->CallStaticDoubleMethodA(env, g_bridge, m->id, args);
        uint64_t b;
        memcpy(&b, &d, 8);
        set_wide(cpu, b);
        break;
    }
    default: {
        jobject result = (*env)->CallStaticObjectMethodA(env, g_bridge, m->id, args);
        cpu->r[0] = (*env)->ExceptionCheck(env) ? 0 : guest_object(env, result);
        break;
    }
    }
    if ((*env)->ExceptionCheck(env)) {
        A5_ERROR("GameBridge.%s%s threw", m->name, m->sig);
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        set_wide(cpu, 0);
    }
    const char *p = m->sig + 1;
    for (int i = 0; i < count; ++i, p = skip_type(p)) {
        if ((*p == 'L' || *p == '[') && args[i].l) (*env)->DeleteLocalRef(env, args[i].l);
    }
}

/* ---- JNIEnv entry --------------------------------------------------- */

static uint32_t array_data(CPU *cpu, uint32_t token, uint32_t start, uint32_t len)
{
    if (!is_object(token, TAG_ARRAY)) guest_abort(cpu, "JNI byte array token 0x%08x", token);
    uint32_t size = rd32(token + 4);
    if (start > size || len > size - start) {
        guest_abort(cpu, "JNI array range %u+%u of %u", start, len, size);
    }
    return token + HEADER + start;
}

static void jni_entry(CPU *cpu)
{
    const uint32_t index = (cpu->pc - A5_JNI_STUBS) / 4u;
    const uint32_t a1 = cpu->r[1], a2 = cpu->r[2], a3 = cpu->r[3];
    const uint32_t s0 = rd32(cpu->r[13]);

    if (index == SLOT(GetVersion)) { cpu->r[0] = 0x00010004u; return; }
    if (index == SLOT(FindClass) || index == SLOT(GetObjectClass) ||
        index == SLOT(GetSuperclass)) { cpu->r[0] = CLASS_TOKEN; return; }
    if (index == SLOT(ExceptionOccurred) || index == SLOT(ExceptionCheck)) { cpu->r[0] = 0; return; }
    if (index == SLOT(ExceptionDescribe) || index == SLOT(ExceptionClear)) return;
    if (index == SLOT(FatalError)) guest_abort(cpu, "JNI FatalError: %s", a5_guest_cstr(cpu, a1));
    if (index == SLOT(PushLocalFrame) || index == SLOT(EnsureLocalCapacity)) { cpu->r[0] = 0; return; }
    if (index == SLOT(PopLocalFrame)) { cpu->r[0] = a1; return; }
    if (index == SLOT(NewGlobalRef)) {
        if (is_object(a1, TAG_STRING) || is_object(a1, TAG_ARRAY)) wr32(a1 + 8, rd32(a1 + 8) + STATE_GLOBAL);
        cpu->r[0] = a1;
        return;
    }
    if (index == SLOT(DeleteGlobalRef)) {
        if ((is_object(a1, TAG_STRING) || is_object(a1, TAG_ARRAY)) && rd32(a1 + 8) >= STATE_GLOBAL) {
            wr32(a1 + 8, rd32(a1 + 8) - STATE_GLOBAL);
            release_object(a1);
        }
        return;
    }
    if (index == SLOT(DeleteLocalRef)) {
        if (is_object(a1, TAG_STRING) || is_object(a1, TAG_ARRAY)) drop_local(a1);
        return;
    }
    if (index == SLOT(NewLocalRef)) { cpu->r[0] = a1; return; }
    if (index == SLOT(IsSameObject)) { cpu->r[0] = a1 == a2; return; }
    if (index == SLOT(IsInstanceOf)) { cpu->r[0] = 1; return; }
    if (index == SLOT(GetMethodID) || index == SLOT(GetStaticMethodID)) {
        cpu->r[0] = find_method(cpu, a2, a3);
        return;
    }
    /* Call<Type>Method{,V,A} and CallStatic<Type>Method{,V,A}: mid in r2. */
    if ((index >= SLOT(CallObjectMethod) && index <= SLOT(CallVoidMethodA)) ||
        (index >= SLOT(CallStaticObjectMethod) && index <= SLOT(CallStaticVoidMethodA))) {
        uint32_t first = index >= SLOT(CallStaticObjectMethod) ? SLOT(CallStaticObjectMethod)
                                                                : SLOT(CallObjectMethod);
        call_method(cpu, (ArgStyle)((index - first) % 3));
        return;
    }
    if (index == SLOT(NewStringUTF)) { cpu->r[0] = a5_jni_new_string(a5_guest_cstr(cpu, a1)); return; }
    if (index == SLOT(GetStringUTFLength)) { cpu->r[0] = (uint32_t)strlen(string_of(cpu, a1)); return; }
    if (index == SLOT(GetStringLength)) {
        uint32_t n = 0;
        for (const unsigned char *p = (const unsigned char *)string_of(cpu, a1); *p; ++p) n += (*p & 0xC0) != 0x80;
        cpu->r[0] = n;
        return;
    }
    if (index == SLOT(GetStringUTFChars) || index == SLOT(GetStringCritical)) {
        string_of(cpu, a1);
        if (a2) wr8(a2, 0);
        cpu->r[0] = a1 + HEADER;
        return;
    }
    if (index == SLOT(ReleaseStringUTFChars) || index == SLOT(ReleaseStringCritical) ||
        index == SLOT(ReleaseByteArrayElements) || index == SLOT(ReleasePrimitiveArrayCritical)) return;
    if (index == SLOT(GetStringUTFRegion)) {
        const char *s = string_of(cpu, a1);
        uint32_t len = (uint32_t)strlen(s);
        if (a2 > len || a3 > len - a2) guest_abort(cpu, "GetStringUTFRegion range");
        if (!a5_guest_range(s0, a3 + 1)) guest_abort(cpu, "GetStringUTFRegion target");
        memcpy(G(s0), s + a2, a3);
        wr8(s0 + a3, 0);
        return;
    }
    if (index == SLOT(GetArrayLength)) {
        if (!is_object(a1, TAG_ARRAY)) guest_abort(cpu, "GetArrayLength token 0x%08x", a1);
        cpu->r[0] = rd32(a1 + 4);
        return;
    }
    if (index == SLOT(NewByteArray)) { cpu->r[0] = new_object(TAG_ARRAY, NULL, a1); return; }
    if (index == SLOT(GetByteArrayElements) || index == SLOT(GetPrimitiveArrayCritical)) {
        if (a2) wr8(a2, 0);
        cpu->r[0] = array_data(cpu, a1, 0, 0);
        return;
    }
    if (index == SLOT(GetByteArrayRegion) || index == SLOT(SetByteArrayRegion)) {
        uint32_t data = array_data(cpu, a1, a2, a3);
        if (a3 && !a5_guest_range(s0, a3)) guest_abort(cpu, "JNI byte region buffer 0x%08x", s0);
        if (index == SLOT(GetByteArrayRegion)) memmove(G(s0), G(data), a3);
        else memmove(G(data), G(s0), a3);
        return;
    }
    if (index == SLOT(GetJavaVM)) { wr32(a1, A5_JNI_VM); cpu->r[0] = 0; return; }
    guest_abort(cpu, "unsupported JNIEnv function #%u", index);
}

static void vm_entry(CPU *cpu)
{
    const uint32_t index = (cpu->pc - A5_JNI_VM_STUBS) / 4u;
    switch (index) {
    case 4: case 7: /* AttachCurrentThread{,AsDaemon}(vm, JNIEnv **, args) */
    case 6:         /* GetEnv(vm, void **, version) */
        if (cpu->r[1]) wr32(cpu->r[1], A5_JNI_ENV);
        cpu->r[0] = 0;
        return;
    case 3: case 5: cpu->r[0] = 0; return;
    default: guest_abort(cpu, "unsupported JavaVM function #%u", index);
    }
}

/* ---- installation --------------------------------------------------- */

void a5_jni_set_thread_env(JNIEnv *env) { t_env = env; }

void a5_jni_attach_thread(void)
{
    JNIEnv *env = NULL;
    if (g_vm && (*g_vm)->AttachCurrentThread(g_vm, &env, NULL) == JNI_OK) t_env = env;
}

void a5_jni_detach_thread(void)
{
    if (g_vm && t_env) (*g_vm)->DetachCurrentThread(g_vm);
    t_env = NULL;
    free(local_list);
    local_list = NULL;
    local_count = local_cap = 0;
}
uint32_t a5_jni_env(void) { return A5_JNI_ENV; }
uint32_t a5_jni_class_token(void) { return CLASS_TOKEN; }

void a5_jni_install(JNIEnv *env, jclass bridge)
{
    t_env = env;
    (*env)->GetJavaVM(env, &g_vm);
    g_bridge = (*env)->NewGlobalRef(env, bridge);
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    jclass bytes_class = (*env)->FindClass(env, "[B");
    g_string_class = (*env)->NewGlobalRef(env, string_class);
    g_bytes_class = (*env)->NewGlobalRef(env, bytes_class);
    (*env)->DeleteLocalRef(env, string_class);
    (*env)->DeleteLocalRef(env, bytes_class);

    wr32(A5_JNI_ENV, A5_JNI_TABLE);
    for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
        uint32_t stub = A5_JNI_STUBS + i * 4u;
        wr32(A5_JNI_TABLE + i * 4u, i < 4 ? 0 : stub);
        if (i >= 4) guest_register_entry(stub, jni_entry, "JNIEnv");
    }
    wr32(A5_JNI_VM, A5_JNI_VM_TABLE);
    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t stub = A5_JNI_VM_STUBS + i * 4u;
        wr32(A5_JNI_VM_TABLE + i * 4u, i < 3 ? 0 : stub);
        if (i >= 3) guest_register_entry(stub, vm_entry, "JavaVM");
    }
}
