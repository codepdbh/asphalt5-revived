/* Android host runtime for the statically translated Asphalt 5 HD 3.4.1 engine.
 *
 * Guest memory map (32-bit offsets into the 4 GiB reservation in guest.c):
 *   0x00000000-0x00FFFFFF  ELF image and BSS
 *   0x10000000-0x10FFFFFF  guest stack (grows down from 0x11000000)
 *   0x11000000-0x3FFFFFFF  guest heap arena
 *   0x40000000-0x40FFFFFF  host scratch
 *   0x70000000-0x700FFFFF  host data: errno, ctype, __sF, JNI env, strings
 *   0x71000000-...         import veneers (code addresses only)
 *   0x72000000-...         JNI function stubs (code addresses only)
 */
#ifndef A5_RUNTIME_H
#define A5_RUNTIME_H

#include "guest.h"

#include <jni.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A5_STACK_TOP        0x11000000u
#define A5_SCRATCH          0x40000000u
#define A5_DATA_ERRNO       0x70000200u
#define A5_DATA_CTYPE       0x70000300u
#define A5_DATA_SF          0x70000400u   /* 3 x 84-byte bionic FILE */
#define A5_DATA_GUARD       0x70000500u
#define A5_DATA_DSO         0x70000504u
#define A5_DATA_STRINGS     0x70002000u   /* glGetString / inet_ntoa results */
#define A5_JNI_ENV          0x70001000u
#define A5_JNI_TABLE        0x70001100u
#define A5_JNI_VM           0x70001800u
#define A5_JNI_VM_TABLE     0x70001810u
#define A5_FILE_HANDLES     0x70010000u
#define A5_IMPORT_BASE      0x71000000u
#define A5_IMPORT_STRIDE    16u
#define A5_JNI_STUBS        0x72000000u
#define A5_JNI_VM_STUBS     0x72001000u

/* Logging (logcat tag "Asphalt5"). */
void a5_log(int priority, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define A5_INFO(...)  a5_log(4, __VA_ARGS__)
#define A5_WARN(...)  a5_log(5, __VA_ARGS__)
#define A5_ERROR(...) a5_log(6, __VA_ARGS__)

/* Guest memory validation: true when [addr, addr+len) is mapped read/write. */
bool a5_guest_range(uint32_t addr, uint32_t len);
/* Length-checked guest C string; aborts the guest on an unterminated or
 * unmapped string.  NULL maps to "(null)" only when allow_null is set. */
const char *a5_guest_cstr(CPU *cpu, uint32_t addr);
uint32_t a5_host_to_guest(const void *host);

/* Directories: game data (read-only originals) and saves (writable). */
void a5_set_paths(const char *data_dir, const char *save_dir);
const char *a5_data_dir(void);
const char *a5_save_dir(void);
/* Maps an Android path used by the original game to a host path.
 * Returns NULL for paths outside the two roots. */
const char *a5_map_path(const char *guest_path, bool writing, char *out, size_t cap);

/* Imports. */
void a5_register_imports(void);
void a5_set_errno(int host_errno);
GuestFn a5_net_handler(const char *name);
void a5_set_hostname(const char *name);

/* AAPCS (softfp) variadic argument reader. */
typedef struct {
    CPU *cpu;
    int reg;        /* next core register, 4 = exhausted */
    uint32_t sp;    /* next stacked argument */
} GuestVa;
GuestVa a5_va_regs(CPU *cpu, int first_reg);
GuestVa a5_va_list(uint32_t va_pointer);
uint32_t a5_va_u32(GuestVa *va);
uint64_t a5_va_u64(GuestVa *va);
double a5_va_double(GuestVa *va);
int a5_format(char *out, size_t cap, const char *fmt, GuestVa *va);

/* GLES1 bridge. */
bool a5_gles_dispatch(CPU *cpu, const char *name);
GuestFn a5_gles_handler(const char *name);
void a5_gles_set_surface(int width, int height, int logical_width, int logical_height);
void a5_gles_begin_frame(void);
void a5_gles_map_touch(float x, float y, int *gx, int *gy);
void a5_upload_compressed_texture(CPU *cpu);

/* Guest JNIEnv. */
void a5_jni_install(JNIEnv *env, jclass bridge);
void a5_jni_set_thread_env(JNIEnv *env);
void a5_jni_end_frame(void);
uint32_t a5_jni_env(void);
uint32_t a5_jni_class_token(void);
uint32_t a5_jni_new_string(const char *utf8);
bool a5_jni_take_movie_request(void);
void a5_jni_attach_thread(void);
void a5_jni_detach_thread(void);

/* Guest helpers. */
void a5_call(CPU *cpu, uint32_t address);
uint32_t a5_native_symbol(const char *suffix);

#ifdef __cplusplus
}
#endif

#endif
