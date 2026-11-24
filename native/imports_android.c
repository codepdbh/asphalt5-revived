/* Android implementation of every import of libasphalt5.so 3.4.1.
 *
 * Each import name gets a direct handler that is registered at its veneer
 * and PLT addresses, so calls from translated code reach the host function
 * without a string lookup.  Guest pointers are 32-bit offsets into g_mem and
 * are validated before the host touches them.
 */
#include "runtime.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define A0 (cpu->r[0])
#define A1 (cpu->r[1])
#define A2 (cpu->r[2])
#define A3 (cpu->r[3])
#define SP (cpu->r[13])
#define RET(v) (cpu->r[0] = (uint32_t)(v))

/* ---- logging -------------------------------------------------------- */

void a5_log(int priority, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(priority, "Asphalt5", fmt, ap);
    va_end(ap);
}

void platform_crash_log(const char *message)
{
    __android_log_write(ANDROID_LOG_FATAL, "Asphalt5", message);
}

/* ---- guest memory --------------------------------------------------- */

bool a5_guest_range(uint32_t addr, uint32_t len)
{
    uint64_t end = (uint64_t)addr + len;
    if (!addr) return false;
    if (end <= 0x01000000u) return true;
    if (addr >= 0x10000000u && end <= 0x41000000u) return true;
    if (addr >= 0x70000000u && end <= 0x70100000u) return true;
    return false;
}

static uint32_t region_end(uint32_t addr)
{
    if (addr && addr < 0x01000000u) return 0x01000000u;
    if (addr >= 0x10000000u && addr < 0x41000000u) return 0x41000000u;
    if (addr >= 0x70000000u && addr < 0x70100000u) return 0x70100000u;
    return 0;
}

const char *a5_guest_cstr(CPU *cpu, uint32_t addr)
{
    uint32_t end = region_end(addr);
    if (end && memchr(G(addr), 0, end - addr)) return (const char *)G(addr);
    guest_abort(cpu, "invalid guest string at 0x%08x", addr);
    return "";
}

static void *buffer(CPU *cpu, uint32_t addr, uint32_t len)
{
    if (!len) return G(addr);
    if (!a5_guest_range(addr, len)) {
        guest_abort(cpu, "invalid guest buffer 0x%08x+0x%x", addr, len);
    }
    return G(addr);
}

uint32_t a5_host_to_guest(const void *host)
{
    return host ? (uint32_t)((const uint8_t *)host - g_mem) : 0;
}

void a5_call(CPU *cpu, uint32_t address)
{
    cpu->r[14] = 0;
    guest_call(cpu, address);
}

/* ---- paths ---------------------------------------------------------- */

static char g_data_dir[512];
static char g_save_dir[512];

void a5_set_paths(const char *data_dir, const char *save_dir)
{
    snprintf(g_data_dir, sizeof(g_data_dir), "%s", data_dir);
    snprintf(g_save_dir, sizeof(g_save_dir), "%s", save_dir);
}

const char *a5_data_dir(void) { return g_data_dir; }
const char *a5_save_dir(void) { return g_save_dir; }

static bool safe_relative(const char *path)
{
    if (!*path) return false;
    for (const char *p = path; *p; ) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if ((n == 2 && p[0] == '.' && p[1] == '.')) return false;
        if (!slash) break;
        p = slash + 1;
    }
    return true;
}

const char *a5_map_path(const char *in, bool writing, char *out, size_t cap)
{
    static const char *const data_prefixes[] = {
        "/sdcard/gameloft/games/asphalt5/", "/sdcard/gameloft/games/GloftA5HD/",
        "/mnt/sdcard/gameloft/games/asphalt5/",
    };
    static const char save_prefix[] = "/data/data/com.gameloft.android.GAND.GloftA5HD";
    const char *rest = NULL;
    const char *root = g_data_dir;
    for (size_t i = 0; i < sizeof(data_prefixes) / sizeof(data_prefixes[0]); ++i) {
        size_t n = strlen(data_prefixes[i]);
        if (!strncmp(in, data_prefixes[i], n)) { rest = in + n; break; }
    }
    if (!rest && !strncmp(in, save_prefix, sizeof(save_prefix) - 1)) {
        rest = in + sizeof(save_prefix) - 1;
        root = g_save_dir;
    }
    if (!rest && in[0] != '/') rest = in;  /* "./x", ".//x", "x" */
    if (!rest) return NULL;
    while (*rest == '/' || (rest[0] == '.' && rest[1] == '/')) rest += rest[0] == '.' ? 2 : 1;
    if (!safe_relative(rest)) return NULL;
    /* New files always go to the save directory; existing originals are read
     * from the data directory unless a newer save copy exists. */
    if (root == g_data_dir) {
        snprintf(out, cap, "%s/%s", g_save_dir, rest);
        struct stat st;
        if (writing || stat(out, &st) == 0) return out;
    }
    snprintf(out, cap, "%s/%s", root, rest);
    return out;
}

/* ---- data imports --------------------------------------------------- */

static void init_data_imports(void)
{
    enum { U = 0x01, L = 0x02, N = 0x04, S = 0x08, P = 0x10, C = 0x20, X = 0x40, B = 0x80 };
    const uint32_t table = A5_DATA_CTYPE + 16;
    wr32(A5_DATA_ERRNO, 0);
    wr32(A5_DATA_DSO, 0);
    /* bionic (pre-L) exports `const char *_ctype_` pointing at a 1 + 256
     * entry table of BSD class bits; macros index (_ctype_ + 1)[c]. */
    wr32(A5_DATA_CTYPE, table);
    wr8(table, 0);
    for (int c = 0; c < 256; ++c) {
        uint8_t v = 0;
        if (c < 128) {
            if (isupper(c)) v |= U;
            if (islower(c)) v |= L;
            if (isdigit(c)) v |= N;
            if (isspace(c)) v |= S;
            if (ispunct(c)) v |= P;
            if (iscntrl(c)) v |= C;
            if (isxdigit(c) && !isdigit(c)) v |= X;
            if (c == ' ') v |= B;
        }
        wr8(table + 1 + (uint32_t)c, v);
    }
    memset(G(A5_DATA_SF), 0, 3 * 84);
}

uint32_t guest_import_addr(const char *name)
{
    if (!strcmp(name, "_ctype_")) return A5_DATA_CTYPE;
    if (!strcmp(name, "__sF")) return A5_DATA_SF;
    if (!strcmp(name, "__stack_chk_guard")) return A5_DATA_GUARD;
    if (!strcmp(name, "__dso_handle")) return A5_DATA_DSO;
    for (size_t i = 0; i < g_import_def_count; ++i) {
        if (!strcmp(name, g_import_defs[i].name)) return A5_IMPORT_BASE + (uint32_t)i * A5_IMPORT_STRIDE;
    }
    return 0;
}

void a5_set_errno(int host_errno)
{
    wr32(A5_DATA_ERRNO, (uint32_t)host_errno);  /* Linux and bionic share values */
}

/* ---- variadic arguments (AAPCS softfp) ------------------------------ */

GuestVa a5_va_regs(CPU *cpu, int first_reg)
{
    GuestVa va = {cpu, first_reg, cpu->r[13]};
    return va;
}

GuestVa a5_va_list(uint32_t va_pointer)
{
    GuestVa va = {NULL, 4, va_pointer};
    return va;
}

uint32_t a5_va_u32(GuestVa *va)
{
    if (va->reg < 4) return va->cpu->r[va->reg++];
    uint32_t v = rd32(va->sp);
    va->sp += 4;
    return v;
}

uint64_t a5_va_u64(GuestVa *va)
{
    if (va->reg < 4) {
        va->reg += va->reg & 1;            /* doubleword in an even pair */
        if (va->reg < 4) {
            uint64_t v = va->cpu->r[va->reg] | ((uint64_t)va->cpu->r[va->reg + 1] << 32);
            va->reg += 2;
            return v;
        }
    }
    va->reg = 4;
    va->sp = (va->sp + 7u) & ~7u;
    uint64_t v = rd64(va->sp);
    va->sp += 8;
    return v;
}

double a5_va_double(GuestVa *va)
{
    uint64_t bits = a5_va_u64(va);
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static void put_char(char *out, size_t cap, size_t *o, char c)
{
    if (*o + 1 < cap) out[*o] = c;
    ++*o;
}

int a5_format(char *out, size_t cap, const char *fmt, GuestVa *va)
{
    size_t o = 0;
    for (const char *p = fmt; *p; ) {
        if (*p != '%') { put_char(out, cap, &o, *p++); continue; }
        ++p;
        if (*p == '%') { put_char(out, cap, &o, '%'); ++p; continue; }
        char spec[48];
        size_t s = 0;
        spec[s++] = '%';
        while (*p && strchr("-+ #0'", *p) && s < 16) spec[s++] = *p++;
        if (*p == '*') { s += (size_t)snprintf(spec + s, sizeof(spec) - s, "%d", (int32_t)a5_va_u32(va)); ++p; }
        else while (isdigit((unsigned char)*p) && s < 28) spec[s++] = *p++;
        if (*p == '.') {
            spec[s++] = *p++;
            if (*p == '*') { s += (size_t)snprintf(spec + s, sizeof(spec) - s, "%d", (int32_t)a5_va_u32(va)); ++p; }
            else while (isdigit((unsigned char)*p) && s < 40) spec[s++] = *p++;
        }
        int longs = 0, half = 0;
        for (;; ++p) {
            if (*p == 'l') ++longs;
            else if (*p == 'h') ++half;
            else if (*p == 'q' || *p == 'L' || *p == 'j') longs = 2;
            else if (*p == 'z' || *p == 't') ;
            else break;
        }
        char conv = *p ? *p++ : 0;
        char tmp[1024];
        int n = 0;
        switch (conv) {
        case 'd': case 'i': {
            int64_t v = longs >= 2 ? (int64_t)a5_va_u64(va) : (int64_t)(int32_t)a5_va_u32(va);
            if (half == 1) v = (int16_t)v; else if (half >= 2) v = (int8_t)v;
            memcpy(spec + s, "lld", 4);
            n = snprintf(tmp, sizeof(tmp), spec, (long long)v);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = longs >= 2 ? a5_va_u64(va) : a5_va_u32(va);
            if (half == 1) v = (uint16_t)v; else if (half >= 2) v = (uint8_t)v;
            spec[s] = 'l'; spec[s + 1] = 'l'; spec[s + 2] = conv; spec[s + 3] = 0;
            n = snprintf(tmp, sizeof(tmp), spec, (unsigned long long)v);
            break;
        }
        case 'c': {
            spec[s] = 'c'; spec[s + 1] = 0;
            n = snprintf(tmp, sizeof(tmp), spec, (int)a5_va_u32(va));
            break;
        }
        case 's': case 'S': {
            uint32_t ptr = a5_va_u32(va);
            char wide[512];
            const char *str = "(null)";
            if (ptr && region_end(ptr)) {
                if (longs || conv == 'S') {
                    size_t i = 0;
                    for (uint32_t a = ptr; i + 1 < sizeof(wide) && a5_guest_range(a, 4) && rd32(a); a += 4) {
                        uint32_t ch = rd32(a);
                        wide[i++] = ch < 128 ? (char)ch : '?';
                    }
                    wide[i] = 0;
                    str = wide;
                } else if (memchr(G(ptr), 0, region_end(ptr) - ptr)) {
                    str = (const char *)G(ptr);
                }
            }
            spec[s] = 's'; spec[s + 1] = 0;
            n = snprintf(tmp, sizeof(tmp), spec, str);
            break;
        }
        case 'p': {
            n = snprintf(tmp, sizeof(tmp), "0x%x", a5_va_u32(va));
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d = a5_va_double(va);
            spec[s] = conv; spec[s + 1] = 0;
            n = snprintf(tmp, sizeof(tmp), spec, d);
            break;
        }
        case 'n': {
            uint32_t ptr = a5_va_u32(va);
            if (a5_guest_range(ptr, 4)) wr32(ptr, (uint32_t)o);
            break;
        }
        default:
            put_char(out, cap, &o, '%');
            if (conv) put_char(out, cap, &o, conv);
            continue;
        }
        if (n < 0) n = 0;
        if ((size_t)n >= sizeof(tmp)) n = (int)sizeof(tmp) - 1;
        for (int i = 0; i < n; ++i) put_char(out, cap, &o, tmp[i]);
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return (int)o;
}

/* ---- stdio ---------------------------------------------------------- */

#define MAX_FILES 64
static FILE *g_files[MAX_FILES];

static uint32_t file_handle(FILE *f)
{
    for (int i = 0; i < MAX_FILES; ++i) {
        if (!g_files[i]) { g_files[i] = f; return A5_FILE_HANDLES + (uint32_t)i * 16u; }
    }
    fclose(f);
    return 0;
}

enum { STREAM_NONE, STREAM_FILE, STREAM_OUT, STREAM_ERR, STREAM_IN };

static int stream_kind(uint32_t h, FILE **f)
{
    *f = NULL;
    if (h >= A5_FILE_HANDLES && h < A5_FILE_HANDLES + MAX_FILES * 16u && !(h & 15u)) {
        *f = g_files[(h - A5_FILE_HANDLES) / 16u];
        return *f ? STREAM_FILE : STREAM_NONE;
    }
    if (h == A5_DATA_SF) return STREAM_IN;
    if (h == A5_DATA_SF + 84) return STREAM_OUT;
    if (h == A5_DATA_SF + 168) return STREAM_ERR;
    return STREAM_NONE;
}

static void log_text(int priority, const char *text)
{
    if (*text) __android_log_print(priority, "A5Guest", "%s", text);
}

static void write_stream(CPU *cpu, uint32_t h, const char *text, size_t len)
{
    FILE *f;
    switch (stream_kind(h, &f)) {
    case STREAM_FILE: fwrite(text, 1, len, f); break;
    case STREAM_OUT: log_text(ANDROID_LOG_INFO, text); break;
    case STREAM_ERR: log_text(ANDROID_LOG_WARN, text); break;
    default: guest_abort(cpu, "write to invalid FILE 0x%08x", h);
    }
}

static void imp_fopen(CPU *cpu)
{
    const char *path = a5_guest_cstr(cpu, A0);
    const char *mode = a5_guest_cstr(cpu, A1);
    bool writing = strpbrk(mode, "wa+") != NULL;
    char host[1024];
    const char *mapped = a5_map_path(path, writing, host, sizeof(host));
    FILE *f = mapped ? fopen(mapped, mode) : NULL;
    if (!f) {
        a5_set_errno(mapped ? errno : EACCES);
        if (writing) A5_WARN("fopen(%s, %s) failed: %s", path, mode, mapped ? strerror(errno) : "outside roots");
        RET(0);
        return;
    }
    RET(file_handle(f));
}

static void imp_fclose(CPU *cpu)
{
    FILE *f;
    if (stream_kind(A0, &f) == STREAM_FILE) {
        g_files[(A0 - A5_FILE_HANDLES) / 16u] = NULL;
        RET(fclose(f));
        return;
    }
    RET(0);
}

static void imp_fread(CPU *cpu)
{
    FILE *f;
    uint64_t total = (uint64_t)A1 * A2;
    if (stream_kind(A3, &f) != STREAM_FILE || total > 0x40000000u) { RET(0); return; }
    RET(fread(buffer(cpu, A0, (uint32_t)total), 1, (size_t)total, f) / (A1 ? A1 : 1));
}

static void imp_fwrite(CPU *cpu)
{
    FILE *f;
    uint64_t total = (uint64_t)A1 * A2;
    if (total > 0x40000000u) { RET(0); return; }
    int kind = stream_kind(A3, &f);
    void *data = buffer(cpu, A0, (uint32_t)total);
    if (kind == STREAM_FILE) { RET(fwrite(data, 1, (size_t)total, f) / (A1 ? A1 : 1)); return; }
    if (kind == STREAM_OUT || kind == STREAM_ERR) {
        char text[1024];
        size_t n = total < sizeof(text) - 1 ? (size_t)total : sizeof(text) - 1;
        memcpy(text, data, n);
        text[n] = 0;
        write_stream(cpu, A3, text, n);
        RET(A2);
        return;
    }
    RET(0);
}

static void imp_fseek(CPU *cpu)
{
    FILE *f;
    RET(stream_kind(A0, &f) == STREAM_FILE ? fseek(f, (long)(int32_t)A1, (int)A2) : -1);
}

static void imp_ftell(CPU *cpu)
{
    FILE *f;
    RET(stream_kind(A0, &f) == STREAM_FILE ? (int32_t)ftell(f) : -1);
}

static void imp_fflush(CPU *cpu)
{
    FILE *f;
    if (stream_kind(A0, &f) == STREAM_FILE) fflush(f);
    RET(0);
}

static void imp_fileno(CPU *cpu)
{
    FILE *f;
    int kind = stream_kind(A0, &f);
    RET(kind == STREAM_FILE ? fileno(f) : kind == STREAM_IN ? 0 : kind == STREAM_OUT ? 1 :
        kind == STREAM_ERR ? 2 : -1);
}

static void imp_fputc(CPU *cpu)
{
    char c[2] = {(char)A0, 0};
    write_stream(cpu, A1, c, 1);
    RET(A0 & 0xffu);
}

static void imp_puts(CPU *cpu)
{
    log_text(ANDROID_LOG_INFO, a5_guest_cstr(cpu, A0));
    RET(1);
}

static void imp_printf(CPU *cpu)
{
    char text[4096];
    GuestVa va = a5_va_regs(cpu, 1);
    int n = a5_format(text, sizeof(text), a5_guest_cstr(cpu, A0), &va);
    log_text(ANDROID_LOG_INFO, text);
    RET(n);
}

static void imp_fprintf(CPU *cpu)
{
    char text[8192];
    GuestVa va = a5_va_regs(cpu, 2);
    int n = a5_format(text, sizeof(text), a5_guest_cstr(cpu, A1), &va);
    write_stream(cpu, A0, text, strlen(text));
    RET(n);
}

static void imp_vfprintf(CPU *cpu)
{
    char text[8192];
    GuestVa va = a5_va_list(A2);
    int n = a5_format(text, sizeof(text), a5_guest_cstr(cpu, A1), &va);
    write_stream(cpu, A0, text, strlen(text));
    RET(n);
}

static void store_formatted(CPU *cpu, uint32_t dst, const char *fmt, GuestVa *va)
{
    char text[16384];
    int n = a5_format(text, sizeof(text), fmt, va);
    if ((size_t)n >= sizeof(text)) guest_abort(cpu, "sprintf output too long (%d)", n);
    memcpy(buffer(cpu, dst, (uint32_t)n + 1), text, (size_t)n + 1);
    RET(n);
}

static void imp_sprintf(CPU *cpu)
{
    GuestVa va = a5_va_regs(cpu, 2);
    store_formatted(cpu, A0, a5_guest_cstr(cpu, A1), &va);
}

static void imp_vsprintf(CPU *cpu)
{
    GuestVa va = a5_va_list(A2);
    store_formatted(cpu, A0, a5_guest_cstr(cpu, A1), &va);
}

static void imp_swprintf(CPU *cpu)
{
    /* wchar_t is 32-bit on Android. */
    uint32_t dst = A0, cap = A1, wfmt = A2;
    char fmt[1024];
    size_t i = 0;
    for (uint32_t a = wfmt; i + 1 < sizeof(fmt); a += 4) {
        if (!a5_guest_range(a, 4)) guest_abort(cpu, "invalid swprintf format");
        uint32_t ch = rd32(a);
        if (!ch) break;
        fmt[i++] = ch < 128 ? (char)ch : '?';
    }
    fmt[i] = 0;
    char text[4096];
    GuestVa va = a5_va_regs(cpu, 3);
    int n = a5_format(text, sizeof(text), fmt, &va);
    if (!cap) { RET(-1); return; }
    uint32_t count = (uint32_t)n < cap ? (uint32_t)n : cap - 1;
    buffer(cpu, dst, (count + 1) * 4);
    for (uint32_t k = 0; k < count; ++k) wr32(dst + k * 4, (uint8_t)text[k]);
    wr32(dst + count * 4, 0);
    RET((uint32_t)n < cap ? n : -1);
}

static void imp_fscanf(CPU *cpu)
{
    FILE *f;
    if (stream_kind(A0, &f) != STREAM_FILE) { RET(-1); return; }
    const char *fmt = a5_guest_cstr(cpu, A1);
    GuestVa va = a5_va_regs(cpu, 2);
    int assigned = 0;
    const char *p = fmt;
    while (*p) {
        /* One piece = literal text up to and including one conversion. */
        char piece[128];
        size_t n = 0;
        bool conversion = false, suppress = false, wide = false;
        char conv = 0;
        while (*p && n < sizeof(piece) - 4) {
            if (*p != '%') { piece[n++] = *p++; continue; }
            if (p[1] == '%') { piece[n++] = *p++; piece[n++] = *p++; continue; }
            piece[n++] = *p++;
            if (*p == '*') { suppress = true; piece[n++] = *p++; }
            while (isdigit((unsigned char)*p) && n < sizeof(piece) - 4) piece[n++] = *p++;
            while (*p == 'h' || *p == 'l' || *p == 'L') {
                if (*p == 'l' || *p == 'L') wide = true;
                if (*p == 'h') piece[n++] = *p;
                ++p;
            }
            if (*p == '[') {
                do piece[n++] = *p; while (*p && *p++ != ']' && n < sizeof(piece) - 4);
                conv = '[';
            } else {
                conv = *p ? *p++ : 0;
                if (conv && strchr("eEfFgGaA", conv) && wide) piece[n++] = 'l';
                piece[n++] = conv;
            }
            conversion = true;
            break;
        }
        piece[n] = 0;
        int got;
        if (!conversion || suppress || conv == 'n') {
            got = fscanf(f, piece);
            if (got == EOF) { RET(assigned ? assigned : -1); return; }
            continue;
        }
        uint32_t dst = a5_va_u32(&va);
        size_t size = strchr("eEfFgGaA", conv) ? (wide ? 8 : 4) : (conv == 'c' ? 1 : 4);
        if (conv == 's' || conv == '[') {
            uint32_t end = region_end(dst);
            if (!end) guest_abort(cpu, "fscanf string target 0x%08x", dst);
            got = fscanf(f, piece, (char *)G(dst));
        } else {
            got = fscanf(f, piece, (void *)buffer(cpu, dst, (uint32_t)size));
        }
        if (got == EOF) { RET(assigned ? assigned : -1); return; }
        if (got != 1) break;
        ++assigned;
    }
    RET(assigned);
}

/* ---- memory and strings -------------------------------------------- */

static void imp_memcpy(CPU *cpu)
{
    if (A2) memmove(buffer(cpu, A0, A2), buffer(cpu, A1, A2), A2);
}

static void imp_memset(CPU *cpu)
{
    if (A2) memset(buffer(cpu, A0, A2), (int)A1, A2);
}

static void imp_memcmp(CPU *cpu)
{
    RET(A2 ? memcmp(buffer(cpu, A0, A2), buffer(cpu, A1, A2), A2) : 0);
}

static void imp_memchr(CPU *cpu)
{
    RET(A2 ? a5_host_to_guest(memchr(buffer(cpu, A0, A2), (int)A1, A2)) : 0);
}

static void imp_strlen(CPU *cpu) { RET(strlen(a5_guest_cstr(cpu, A0))); }

static void imp_strcpy(CPU *cpu)
{
    const char *src = a5_guest_cstr(cpu, A1);
    size_t n = strlen(src) + 1;
    memmove(buffer(cpu, A0, (uint32_t)n), src, n);
}

static void imp_strncpy(CPU *cpu)
{
    if (!A2) return;
    char *dst = buffer(cpu, A0, A2);
    uint32_t end = region_end(A1);
    const char *src = (const char *)G(A1);
    if (!end) guest_abort(cpu, "strncpy source 0x%08x", A1);
    size_t n = 0;
    while (n < A2 && A1 + n < end && src[n]) ++n;
    memmove(dst, src, n);
    memset(dst + n, 0, A2 - n);
}

static void imp_strlcpy(CPU *cpu)
{
    const char *src = a5_guest_cstr(cpu, A1);
    size_t len = strlen(src);
    if (A2) {
        size_t n = len < A2 - 1 ? len : A2 - 1;
        char *dst = buffer(cpu, A0, (uint32_t)n + 1);
        memmove(dst, src, n);
        dst[n] = 0;
    }
    RET(len);
}

static void imp_strcat(CPU *cpu)
{
    const char *dst = a5_guest_cstr(cpu, A0);
    const char *src = a5_guest_cstr(cpu, A1);
    size_t d = strlen(dst), s = strlen(src) + 1;
    memmove(buffer(cpu, A0 + (uint32_t)d, (uint32_t)s), src, s);
}

static void imp_strcmp(CPU *cpu) { RET(strcmp(a5_guest_cstr(cpu, A0), a5_guest_cstr(cpu, A1))); }

static void imp_strncmp(CPU *cpu)
{
    uint32_t e0 = region_end(A0), e1 = region_end(A1);
    if (!A2) { RET(0); return; }
    if (!e0 || !e1) guest_abort(cpu, "strncmp 0x%08x 0x%08x", A0, A1);
    size_t n = A2;
    if (n > e0 - A0) n = e0 - A0;
    if (n > e1 - A1) n = e1 - A1;
    RET(strncmp((const char *)G(A0), (const char *)G(A1), n));
}

static void imp_strcasecmp(CPU *cpu) { RET(strcasecmp(a5_guest_cstr(cpu, A0), a5_guest_cstr(cpu, A1))); }
static void imp_strstr(CPU *cpu) { RET(a5_host_to_guest(strstr(a5_guest_cstr(cpu, A0), a5_guest_cstr(cpu, A1)))); }
static void imp_strchr(CPU *cpu) { RET(a5_host_to_guest(strchr(a5_guest_cstr(cpu, A0), (int)A1))); }
static void imp_strrchr(CPU *cpu) { RET(a5_host_to_guest(strrchr(a5_guest_cstr(cpu, A0), (int)A1))); }
static void imp_atoi(CPU *cpu) { RET(atoi(a5_guest_cstr(cpu, A0))); }

static void imp_strtol(CPU *cpu)
{
    const char *s = a5_guest_cstr(cpu, A0);
    char *end;
    long long v = strtoll(s, &end, (int)A2);
    if (v > INT32_MAX) { v = INT32_MAX; a5_set_errno(ERANGE); }
    if (v < INT32_MIN) { v = INT32_MIN; a5_set_errno(ERANGE); }
    if (A1) { buffer(cpu, A1, 4); wr32(A1, a5_host_to_guest(end)); }
    RET((int32_t)v);
}

static void imp_strtod(CPU *cpu)
{
    const char *s = a5_guest_cstr(cpu, A0);
    char *end;
    double v = strtod(s, &end);
    if (A1) { buffer(cpu, A1, 4); wr32(A1, a5_host_to_guest(end)); }
    uint64_t bits;
    memcpy(&bits, &v, 8);
    cpu->r[0] = (uint32_t)bits;
    cpu->r[1] = (uint32_t)(bits >> 32);
}

/* ---- heap ----------------------------------------------------------- */

static void imp_malloc(CPU *cpu) { RET(guest_alloc(A0)); }
static void imp_calloc(CPU *cpu)
{
    uint64_t n = (uint64_t)A0 * A1;
    RET(n > 0x20000000u ? 0 : guest_alloc_zero((size_t)n));
}
static void imp_realloc(CPU *cpu) { RET(guest_realloc(A0, A1)); }
static void imp_free(CPU *cpu) { guest_free(A0); }

static void sort_merge(CPU *cpu, uint32_t cmp, size_t n, size_t size)
{
    /* Stable bottom-up merge sort calling the guest comparator.  The guest
     * array is sorted in host scratch and copied back, so the comparator sees
     * guest pointers into a scratch area at A5_SCRATCH. */
    uint32_t a_base = A5_SCRATCH, b_base = A5_SCRATCH + (uint32_t)(n * size);
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * width) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                CPU c = *cpu;
                c.r[0] = a_base + (uint32_t)(i * size);
                c.r[1] = a_base + (uint32_t)(j * size);
                c.r[14] = 0;
                guest_call(&c, cmp);
                if ((int32_t)c.r[0] <= 0) { memcpy(G(b_base) + k * size, G(a_base) + i * size, size); ++i; }
                else { memcpy(G(b_base) + k * size, G(a_base) + j * size, size); ++j; }
                ++k;
            }
            memcpy(G(b_base) + k * size, G(a_base) + i * size, (mid - i) * size);
            k += mid - i;
            memcpy(G(b_base) + k * size, G(a_base) + j * size, (hi - j) * size);
        }
        memcpy(G(a_base), G(b_base), n * size);
    }
}

static _Thread_local bool g_sorting;

static void imp_qsort(CPU *cpu)
{
    uint32_t base = A0, n = A1, size = A2, cmp = A3;
    if (n < 2 || !size) return;
    uint64_t bytes = (uint64_t)n * size;
    if (bytes * 2 > 0x01000000u || g_sorting) {
        /* Large or nested sorts: in-place insertion sort. */
        uint8_t tmp[256];
        if (size > sizeof(tmp)) guest_abort(cpu, "qsort element too large (%u)", size);
        buffer(cpu, base, (uint32_t)bytes);
        for (uint32_t i = 1; i < n; ++i) {
            for (uint32_t j = i; j > 0; --j) {
                CPU c = *cpu;
                c.r[0] = base + (j - 1) * size;
                c.r[1] = base + j * size;
                c.r[14] = 0;
                guest_call(&c, cmp);
                if ((int32_t)c.r[0] <= 0) break;
                memcpy(tmp, G(base + j * size), size);
                memcpy(G(base + j * size), G(base + (j - 1) * size), size);
                memcpy(G(base + (j - 1) * size), tmp, size);
            }
        }
        return;
    }
    g_sorting = true;
    memcpy(G(A5_SCRATCH), buffer(cpu, base, (uint32_t)bytes), (size_t)bytes);
    sort_merge(cpu, cmp, n, size);
    memcpy(G(base), G(A5_SCRATCH), (size_t)bytes);
    g_sorting = false;
}

/* ---- process, time, threads ---------------------------------------- */

static void imp_abort(CPU *cpu) { guest_abort(cpu, "guest abort()"); }
static void imp_exit(CPU *cpu)
{
    A5_INFO("guest exit(%d)", (int)A0);
    _Exit((int)A0);
}
static void imp_pure_virtual(CPU *cpu) { guest_abort(cpu, "pure virtual call"); }
static void imp_stack_chk_fail(CPU *cpu) { guest_abort(cpu, "stack protector check failed"); }
static void imp_errno(CPU *cpu) { RET(A5_DATA_ERRNO); }

static void imp_time(CPU *cpu)
{
    time_t t = time(NULL);
    if (A0) { buffer(cpu, A0, 4); wr32(A0, (uint32_t)t); }
    RET(t);
}

static void imp_gettimeofday(CPU *cpu)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (A0) { buffer(cpu, A0, 8); wr32(A0, (uint32_t)tv.tv_sec); wr32(A0 + 4, (uint32_t)tv.tv_usec); }
    if (A1) { buffer(cpu, A1, 8); wr32(A1, 0); wr32(A1 + 4, 0); }
    RET(0);
}

static void imp_srand48(CPU *cpu) { srand48((long)(int32_t)A0); }
static void imp_lrand48(CPU *cpu) { RET((uint32_t)lrand48() & 0x7fffffffu); }

#define MAX_DESTRUCTORS 512
static uint32_t g_dtor_obj[MAX_DESTRUCTORS], g_dtor_fn[MAX_DESTRUCTORS];
static size_t g_dtor_count;

static void imp_aeabi_atexit(CPU *cpu)
{
    if (g_dtor_count < MAX_DESTRUCTORS) {
        g_dtor_obj[g_dtor_count] = A0;
        g_dtor_fn[g_dtor_count++] = A1;
    }
    RET(0);
}

void a5_run_guest_destructors(CPU *cpu)
{
    while (g_dtor_count) {
        --g_dtor_count;
        cpu->r[0] = g_dtor_obj[g_dtor_count];
        a5_call(cpu, g_dtor_fn[g_dtor_count]);
    }
}

static void imp_guard_acquire(CPU *cpu)
{
    /* ARM C++ ABI: bit 0 of the guard word marks completed initialization. */
    buffer(cpu, A0, 4);
    RET(!(rd32(A0) & 1u));
}

static void imp_guard_release(CPU *cpu) { buffer(cpu, A0, 4); wr32(A0, 1); }
static void imp_unwind(CPU *cpu) { guest_abort(cpu, "C++ unwinding requested"); }

static void imp_log_print(CPU *cpu)
{
    char text[2048];
    GuestVa va = a5_va_regs(cpu, 3);
    a5_format(text, sizeof(text), a5_guest_cstr(cpu, A2), &va);
    const char *tag = A1 ? a5_guest_cstr(cpu, A1) : "?";
    int priority = (int)A0;
    if (priority < ANDROID_LOG_VERBOSE || priority > ANDROID_LOG_FATAL) priority = ANDROID_LOG_INFO;
    __android_log_print(priority, "A5Guest", "[%s] %s", tag, text);
    RET(0);
}

/* ---- network helpers (sockets live in net_android.c) ----------------- */

static void imp_inet_addr(CPU *cpu) { RET(inet_addr(a5_guest_cstr(cpu, A0))); }

static void imp_inet_ntoa(CPU *cpu)
{
    struct in_addr in = {.s_addr = A0};
    const uint32_t out = A5_DATA_STRINGS + 0x1800;
    snprintf((char *)G(out), 32, "%s", inet_ntoa(in));
    RET(out);
}

static void imp_inet_ntop(CPU *cpu)
{
    int family = (int)A0 == 10 ? AF_INET6 : (int)A0 == 2 ? AF_INET : -1;
    if (family < 0 || !A3) { a5_set_errno(EAFNOSUPPORT); RET(0); return; }
    const void *src = buffer(cpu, A1, family == AF_INET ? 4 : 16);
    char *dst = buffer(cpu, A2, A3);
    RET(inet_ntop(family, src, dst, A3) ? A2 : 0);
}

static void imp_getcwd(CPU *cpu)
{
    if (!A0 || A1 < 2) { a5_set_errno(ERANGE); RET(0); return; }
    memcpy(buffer(cpu, A0, 2), "/", 2);
    RET(A0);
}

/* ---- ARM EABI helpers ---------------------------------------------- */

static void imp_idiv(CPU *cpu)
{
    int32_t x = (int32_t)A0, y = (int32_t)A1;
    RET(!y ? 0 : (y == -1 ? (uint32_t)0 - (uint32_t)x : (uint32_t)(x / y)));
}

static void imp_idivmod(CPU *cpu)
{
    int32_t x = (int32_t)A0, y = (int32_t)A1;
    if (!y) { cpu->r[0] = 0; cpu->r[1] = (uint32_t)x; return; }
    if (y == -1) { cpu->r[0] = (uint32_t)0 - (uint32_t)x; cpu->r[1] = 0; return; }
    cpu->r[0] = (uint32_t)(x / y);
    cpu->r[1] = (uint32_t)(x % y);
}

static void imp_uidiv(CPU *cpu) { RET(A1 ? A0 / A1 : 0); }

static void imp_uidivmod(CPU *cpu)
{
    uint32_t x = A0, y = A1;
    cpu->r[0] = y ? x / y : 0;
    cpu->r[1] = y ? x % y : x;
}

/* ---- libm (softfp: doubles in r0:r1/r2:r3, floats in r0/r1) ---------- */

static double d_arg(CPU *cpu, int reg)
{
    uint64_t b = cpu->r[reg] | ((uint64_t)cpu->r[reg + 1] << 32);
    double d;
    memcpy(&d, &b, 8);
    return d;
}

static void d_ret(CPU *cpu, double v)
{
    uint64_t b;
    memcpy(&b, &v, 8);
    cpu->r[0] = (uint32_t)b;
    cpu->r[1] = (uint32_t)(b >> 32);
}

static float f_arg(CPU *cpu, int reg) { float f; memcpy(&f, &cpu->r[reg], 4); return f; }
static void f_ret(CPU *cpu, float v) { memcpy(&cpu->r[0], &v, 4); }

#define MATH_D1(fn) static void imp_##fn(CPU *cpu) { d_ret(cpu, fn(d_arg(cpu, 0))); }
#define MATH_F1(fn) static void imp_##fn(CPU *cpu) { f_ret(cpu, fn(f_arg(cpu, 0))); }
MATH_D1(acos) MATH_D1(asin) MATH_D1(atan) MATH_D1(ceil) MATH_D1(cos) MATH_D1(floor)
MATH_D1(sin) MATH_D1(sqrt) MATH_D1(tan) MATH_D1(log10) MATH_D1(exp) MATH_D1(log)
MATH_F1(acosf) MATH_F1(asinf) MATH_F1(atanf) MATH_F1(cosf) MATH_F1(expf) MATH_F1(logf)
MATH_F1(sinf) MATH_F1(sqrtf) MATH_F1(tanf) MATH_F1(floorf) MATH_F1(ceilf)
static void imp_atan2(CPU *cpu) { d_ret(cpu, atan2(d_arg(cpu, 0), d_arg(cpu, 2))); }
static void imp_pow(CPU *cpu) { d_ret(cpu, pow(d_arg(cpu, 0), d_arg(cpu, 2))); }
static void imp_atan2f(CPU *cpu) { f_ret(cpu, atan2f(f_arg(cpu, 0), f_arg(cpu, 1))); }
static void imp_powf(CPU *cpu) { f_ret(cpu, powf(f_arg(cpu, 0), f_arg(cpu, 1))); }
static void imp_lrintf(CPU *cpu) { RET((int32_t)lrintf(f_arg(cpu, 0))); }

/* ---- registry ------------------------------------------------------- */

typedef struct { const char *name; GuestFn fn; } ImportHandler;

static const ImportHandler k_imports[] = {
    {"_Znaj", imp_malloc}, {"_Znwj", imp_malloc}, {"_ZdlPv", imp_free}, {"_ZdaPv", imp_free},
    {"__aeabi_atexit", imp_aeabi_atexit}, {"__aeabi_idiv", imp_idiv},
    {"__aeabi_idivmod", imp_idivmod}, {"__aeabi_uidiv", imp_uidiv},
    {"__aeabi_uidivmod", imp_uidivmod}, {"__aeabi_unwind_cpp_pr0", imp_unwind},
    {"__aeabi_unwind_cpp_pr1", imp_unwind}, {"__android_log_print", imp_log_print},
    {"__cxa_guard_acquire", imp_guard_acquire}, {"__cxa_guard_release", imp_guard_release},
    {"__cxa_pure_virtual", imp_pure_virtual}, {"__errno", imp_errno},
    {"__stack_chk_fail", imp_stack_chk_fail}, {"abort", imp_abort}, {"exit", imp_exit},
    {"inet_addr", imp_inet_addr}, {"inet_ntoa", imp_inet_ntoa}, {"inet_ntop", imp_inet_ntop},
    {"getcwd", imp_getcwd},
    {"acos", imp_acos}, {"acosf", imp_acosf}, {"asin", imp_asin}, {"asinf", imp_asinf},
    {"atan", imp_atan}, {"atanf", imp_atanf}, {"atan2", imp_atan2}, {"atan2f", imp_atan2f},
    {"ceil", imp_ceil}, {"ceilf", imp_ceilf}, {"cos", imp_cos}, {"cosf", imp_cosf},
    {"exp", imp_exp}, {"expf", imp_expf}, {"floor", imp_floor}, {"floorf", imp_floorf},
    {"log", imp_log}, {"logf", imp_logf}, {"log10", imp_log10}, {"lrintf", imp_lrintf},
    {"pow", imp_pow}, {"powf", imp_powf}, {"sin", imp_sin}, {"sinf", imp_sinf},
    {"sqrt", imp_sqrt}, {"sqrtf", imp_sqrtf}, {"tan", imp_tan}, {"tanf", imp_tanf},
    {"atoi", imp_atoi}, {"strtol", imp_strtol}, {"strtod", imp_strtod},
    {"malloc", imp_malloc}, {"calloc", imp_calloc}, {"realloc", imp_realloc}, {"free", imp_free},
    {"memchr", imp_memchr}, {"memcmp", imp_memcmp}, {"memcpy", imp_memcpy},
    {"memmove", imp_memcpy}, {"memset", imp_memset},
    {"strcasecmp", imp_strcasecmp}, {"strcat", imp_strcat}, {"strchr", imp_strchr},
    {"strcmp", imp_strcmp}, {"strcpy", imp_strcpy}, {"strlcpy", imp_strlcpy},
    {"strlen", imp_strlen}, {"strncmp", imp_strncmp}, {"strncpy", imp_strncpy},
    {"strrchr", imp_strrchr}, {"strstr", imp_strstr}, {"qsort", imp_qsort},
    {"fopen", imp_fopen}, {"fclose", imp_fclose}, {"fread", imp_fread}, {"fwrite", imp_fwrite},
    {"fseek", imp_fseek}, {"ftell", imp_ftell}, {"fflush", imp_fflush}, {"fileno", imp_fileno},
    {"fputc", imp_fputc}, {"fprintf", imp_fprintf}, {"vfprintf", imp_vfprintf},
    {"fscanf", imp_fscanf}, {"printf", imp_printf}, {"puts", imp_puts},
    {"sprintf", imp_sprintf}, {"vsprintf", imp_vsprintf}, {"swprintf", imp_swprintf},
    {"time", imp_time}, {"gettimeofday", imp_gettimeofday},
    {"srand48", imp_srand48}, {"lrand48", imp_lrand48},
};

static GuestFn find_handler(const char *name)
{
    if (name[0] == 'g' && name[1] == 'l') return a5_gles_handler(name);
    GuestFn net = a5_net_handler(name);
    if (net) return net;
    for (size_t i = 0; i < sizeof(k_imports) / sizeof(k_imports[0]); ++i) {
        if (!strcmp(k_imports[i].name, name)) return k_imports[i].fn;
    }
    return NULL;
}

void import_dispatch(CPU *cpu, const char *name)
{
    GuestFn fn = find_handler(name);
    if (!fn) guest_abort(cpu, "unsupported Android import %s", name);
    fn(cpu);
}

static void plt_fallback(CPU *cpu)
{
    for (size_t i = 0; i < g_plt_import_count; ++i) {
        if (g_plt_imports[i].addr == cpu->pc) { import_dispatch(cpu, g_plt_imports[i].name); return; }
    }
    guest_abort(cpu, "unknown PLT target");
}

void a5_register_imports(void)
{
    init_data_imports();
    size_t missing = 0;
    for (size_t i = 0; i < g_import_def_count; ++i) {
        GuestFn fn = find_handler(g_import_defs[i].name);
        if (!fn) {
            A5_WARN("import without handler: %s", g_import_defs[i].name);
            ++missing;
            fn = g_import_defs[i].fn;  /* aborts with the name when reached */
        }
        guest_register_entry(A5_IMPORT_BASE + (uint32_t)i * A5_IMPORT_STRIDE, fn, g_import_defs[i].name);
    }
    for (size_t i = 0; i < g_plt_import_count; ++i) {
        GuestFn fn = find_handler(g_plt_imports[i].name);
        guest_register_entry(g_plt_imports[i].addr, fn ? fn : plt_fallback, g_plt_imports[i].name);
    }
    if (missing) A5_WARN("%zu imports have no handler", missing);
}
