/* Engine host: boots the translated engine and replays the lifecycle that the
 * original Java classes (Asphalt5, Asphalt5Renderer, GLMediaPlayer,
 * GLResLoader) drove.  Every entry point runs on the GL thread; the guest CPU
 * state is single-threaded.
 */
#include "runtime.h"

#include <jni.h>
#include <stdlib.h>
#include <sys/random.h>

/* Logical canvas reported to the engine as its screen size.  854x480 is the
 * classic layout; the app widens it to the display's aspect ratio. */
static int g_logical_w = 854;
static int g_logical_h = 480;
#define NATIVE_PREFIX "Java_com_gameloft_android_GAND_GloftA5HD_"

static CPU g_cpu;
static bool g_booted;
static bool g_failed;

uint32_t a5_native_symbol(const char *suffix)
{
    char name[256];
    snprintf(name, sizeof(name), NATIVE_PREFIX "%s", suffix);
    return guest_symbol(name);
}

/* Calls a guest JNI native: r0 = JNIEnv, r1 = class or `this`, r2/r3 and the
 * stack carry the Java arguments. */
static void call_native(const char *suffix, uint32_t r2, uint32_t r3,
                        const uint32_t *stack, int stack_words)
{
    uint32_t address = a5_native_symbol(suffix);
    if (!address) guest_abort(&g_cpu, "missing native %s", suffix);
    CPU *cpu = &g_cpu;
    uint32_t saved_sp = cpu->r[13];
    if (stack_words) {
        cpu->r[13] = (cpu->r[13] - 4u * (uint32_t)stack_words) & ~7u;
        for (int i = 0; i < stack_words; ++i) wr32(cpu->r[13] + 4u * (uint32_t)i, stack[i]);
    }
    cpu->r[0] = a5_jni_env();
    cpu->r[1] = a5_jni_class_token();
    cpu->r[2] = r2;
    cpu->r[3] = r3;
    a5_call(cpu, address);
    cpu->r[13] = saved_sp;
    a5_jni_end_frame();
}

/* ---- local Wi-Fi multiplayer ------------------------------------------
 * The 3.4.1 GAND build ships GS_MultiplayerMenu with one button, ONLINE
 * (action 0x40d, Gameloft's lobby servers, offline for years).  The local
 * Wi-Fi flow is still complete in the engine: GS_MultiplayerMenu::Update
 * handles action 0x40c by pushing GS_MPLocalChooseCommType or
 * GS_MultiplayerMenuLocal.  The button is pointed at that action and its
 * label at the matching string 0x40c.  Addresses are for the audited
 * libasphalt5.so (SHA-256 2966c986...).
 */
#define MP_MENU_APPLY_SETTINGS 0x0019817cu   /* GS_MultiplayerMenu::ApplyMenuSettings, vtable slot 33 */
#define MP_MENU_ITEM_ACTION_A  0x001f82f4u   /* item table (800-wide layout), entry 0 action */
#define MP_MENU_ITEM_ACTION_B  0x001f830cu   /* item table (other layouts), entry 0 action */
#define MP_ACTION_LOCAL        0x40cu
#define MP_ACTION_ONLINE       0x40du

void fn_0019817c(CPU *cpu);

static void multiplayer_menu_settings(CPU *cpu)
{
    uint32_t self = cpu->r[0];
    fn_0019817c(cpu);
    wr32(self + 0x958, MP_ACTION_LOCAL);   /* button label string */
}

static void enable_local_multiplayer(void)
{
    const uint32_t actions[] = {MP_MENU_ITEM_ACTION_A, MP_MENU_ITEM_ACTION_B};
    for (size_t i = 0; i < 2; ++i) {
        uint32_t address = g_load_bias + actions[i];
        if (rd32(address) != MP_ACTION_ONLINE) {
            A5_WARN("multiplayer menu table differs; local Wi-Fi not enabled");
            return;
        }
    }
    for (size_t i = 0; i < 2; ++i) wr32(g_load_bias + actions[i], MP_ACTION_LOCAL);
    guest_register_entry(g_load_bias + MP_MENU_APPLY_SETTINGS, multiplayer_menu_settings,
                         "GS_MultiplayerMenu::ApplyMenuSettings (local Wi-Fi)");
}

int a5_boot_engine(const char *engine_path, CPU *cpu)
{
    if (guest_mem_init() != 0) {
        A5_ERROR("cannot reserve the 4 GiB guest address space");
        return -1;
    }
    uint32_t guard;
    if (getrandom(&guard, sizeof(guard), 0) != sizeof(guard)) guard = 0xA5C39E17u;
    wr32(A5_DATA_GUARD, guard);
    guest_register_generated();
    a5_register_imports();
    if (guest_load_elf(engine_path) != 0) {
        A5_ERROR("cannot load %s", engine_path);
        return -1;
    }
    memset(cpu, 0, sizeof(*cpu));
    cpu->r[13] = A5_STACK_TOP - 16u;
    cpu->cpsr = 0x10;
    cpu->tid = 1;
    guest_run_init_array(cpu);

    /* Self-check: the engine's own CRC32 over a known vector. */
    memcpy(G(A5_SCRATCH), "123456789", 9);
    cpu->r[0] = 0;
    cpu->r[1] = A5_SCRATCH;
    cpu->r[2] = 9;
    a5_call(cpu, guest_symbol("crc32"));
    if (cpu->r[0] != 0xcbf43926u) {
        A5_ERROR("engine crc32 self-check returned 0x%08x", cpu->r[0]);
        return -1;
    }
    enable_local_multiplayer();
    A5_INFO("engine loaded, constructors done, crc32 self-check passed");
    return 0;
}

#define GAME_NATIVE(name) Java_com_androidevolved_asphalt5_GameNative_##name

JNIEXPORT jboolean JNICALL
GAME_NATIVE(nativeBoot)(JNIEnv *env, jclass cls, jstring engine, jstring data_dir,
                        jstring save_dir, jclass bridge, jint language,
                        jint logical_width, jint logical_height)
{
    (void)cls;
    if (g_booted || g_failed) return g_booted;
    if (logical_width >= 320 && logical_width <= 2048 && logical_height >= 240 && logical_height <= 1536) {
        g_logical_w = logical_width;
        g_logical_h = logical_height;
    }
    A5_INFO("logical canvas %dx%d", g_logical_w, g_logical_h);
    const char *engine_path = (*env)->GetStringUTFChars(env, engine, NULL);
    const char *data = (*env)->GetStringUTFChars(env, data_dir, NULL);
    const char *save = (*env)->GetStringUTFChars(env, save_dir, NULL);
    a5_set_paths(data, save);
    int status = a5_boot_engine(engine_path, &g_cpu);
    if (status == 0) a5_jni_install(env, bridge);
    (*env)->ReleaseStringUTFChars(env, engine, engine_path);
    (*env)->ReleaseStringUTFChars(env, data_dir, data);
    (*env)->ReleaseStringUTFChars(env, save_dir, save);
    if (status != 0) {
        g_failed = true;
        return JNI_FALSE;
    }

    uint32_t m_env = guest_symbol("mEnv");
    if (m_env) wr32(m_env, a5_jni_env());

    /* Asphalt5Renderer.onSurfaceCreated */
    call_native("Asphalt5Renderer_nativeGetJNIEnv", 0, 0, NULL, 0);
    call_native("GLResLoader_nativeInit", 0, 0, NULL, 0);
    call_native("GLMediaPlayer_nativeInit", 0, 0, NULL, 0);
    call_native("GLMediaPlayer_nativeGetTotalSounds", 0, 0, NULL, 0);
    jint total = (jint)g_cpu.r[0];
    call_native("GLMediaPlayer_nativeGetTotalSoundsOfSameInstance", 0, 0, NULL, 0);
    jint same = (jint)g_cpu.r[0];
    jmethodID init_audio = (*env)->GetStaticMethodID(env, bridge, "initAudio", "(II)V");
    if (init_audio) (*env)->CallStaticVoidMethod(env, bridge, init_audio, total, same);
    A5_INFO("audio: %d sounds, %d instances each", total, same);
    /* Second argument selects the PVRTC data set (non-HTC, non-X10 path). */
    call_native("Asphalt5_nativeInit", 0, 1, NULL, 0);
    /* nativeInit(enableKeyboard, 1, width, height, language) */
    const uint32_t args[] = {(uint32_t)g_logical_w, (uint32_t)g_logical_h, (uint32_t)language};
    call_native("Asphalt5Renderer_nativeInit", 0, 1, args, 3);
    g_booted = true;
    A5_INFO("engine initialized");
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
GAME_NATIVE(nativeSurfaceChanged)(JNIEnv *env, jclass cls, jint width, jint height)
{
    (void)cls;
    a5_jni_set_thread_env(env);
    a5_gles_set_surface(width, height, g_logical_w, g_logical_h);
    if (!g_booted) return;
    call_native("Asphalt5Renderer_nativeGetJNIEnv", 0, 0, NULL, 0);
    call_native("Asphalt5Renderer_nativeResize", (uint32_t)g_logical_w, (uint32_t)g_logical_h, NULL, 0);
}

JNIEXPORT void JNICALL
GAME_NATIVE(nativeRender)(JNIEnv *env, jclass cls)
{
    (void)cls;
    if (!g_booted) return;
    a5_jni_set_thread_env(env);
    a5_gles_begin_frame();
    call_native("Asphalt5Renderer_nativeRender", 0, 0, NULL, 0);
    if (a5_jni_take_movie_request()) {
        /* MyVideoView's completion cleared Game+0x1d72, which releases
         * GS_TrailerMovie::Update (same offset as the reference port). */
        uint32_t game_ptr = guest_symbol("g_pMainGameClass");
        uint32_t game = game_ptr ? rd32(game_ptr) : 0;
        if (game) wr8(game + 0x1d72, 0);
    }
}

JNIEXPORT void JNICALL
GAME_NATIVE(nativeMusicCompleted)(JNIEnv *env, jclass cls, jint sound)
{
    (void)cls;
    if (!g_booted) return;
    a5_jni_set_thread_env(env);
    call_native("GLMediaPlayer_nativeSetStopOnMusic", (uint32_t)sound, 0, NULL, 0);
}

JNIEXPORT void JNICALL
GAME_NATIVE(nativeTouch)(JNIEnv *env, jclass cls, jint action, jfloat x, jfloat y, jint pointer)
{
    (void)cls;
    static const char *const names[] = {
        "Asphalt5_nativeTouchPressed", "Asphalt5_nativeTouchMoved", "Asphalt5_nativeTouchReleased",
    };
    if (!g_booted || action < 0 || action > 2) return;
    a5_jni_set_thread_env(env);
    int gx, gy;
    a5_gles_map_touch(x, y, &gx, &gy);
    const uint32_t stack[] = {(uint32_t)pointer};
    call_native(names[action], (uint32_t)gx, (uint32_t)gy, stack, 1);
}

JNIEXPORT void JNICALL
GAME_NATIVE(nativeKey)(JNIEnv *env, jclass cls, jboolean down, jint keycode)
{
    (void)cls;
    if (!g_booted) return;
    a5_jni_set_thread_env(env);
    call_native(down ? "Asphalt5_nativeSetOnKeyDown" : "Asphalt5_nativeSetOnKeyUp",
                (uint32_t)keycode, 0, NULL, 0);
}

JNIEXPORT void JNICALL
GAME_NATIVE(nativeAccelerometer)(JNIEnv *env, jclass cls, jfloat x, jfloat y, jfloat z)
{
    (void)cls;
    if (!g_booted) return;
    a5_jni_set_thread_env(env);
    const uint32_t stack[] = {f32_bits(z)};
    call_native("Asphalt5_nativeAccelerometer", f32_bits(x), f32_bits(y), stack, 1);
}

JNIEXPORT void JNICALL
GAME_NATIVE(nativePause)(JNIEnv *env, jclass cls, jboolean paused)
{
    (void)cls;
    if (!g_booted) return;
    a5_jni_set_thread_env(env);
    call_native(paused ? "Asphalt5GLSurfaceView_nativePause" : "Asphalt5GLSurfaceView_nativeResume",
                0, 0, NULL, 0);
}

JNIEXPORT jint JNICALL
GAME_NATIVE(nativeCanInterrupt)(JNIEnv *env, jclass cls)
{
    (void)cls;
    if (!g_booted) return 1;
    a5_jni_set_thread_env(env);
    call_native("Asphalt5_nativeCanInterrupt", 0, 0, NULL, 0);
    return (jint)g_cpu.r[0];
}

JNIEXPORT void JNICALL
GAME_NATIVE(nativeCheckWifi)(JNIEnv *env, jclass cls, jboolean active)
{
    (void)cls;
    if (!g_booted) return;
    a5_jni_set_thread_env(env);
    /* Asphalt5.nativeCheckwifi(Z): the original activity reported Wi-Fi state
     * on create/start/resume; local multiplayer stays disabled without it. */
    call_native("Asphalt5_nativeCheckwifi", active ? 1u : 0u, 0, NULL, 0);
}

JNIEXPORT void JNICALL
GAME_NATIVE(nativeSetPlayerName)(JNIEnv *env, jclass cls, jstring name)
{
    (void)cls;
    const char *utf = (*env)->GetStringUTFChars(env, name, NULL);
    if (utf) {
        a5_set_hostname(utf);
        (*env)->ReleaseStringUTFChars(env, name, utf);
    }
}
