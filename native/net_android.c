/* Network and thread imports for the engine's local Wi-Fi multiplayer
 * (CAndroidSocket: UDP broadcast discovery, non-blocking TCP game sockets)
 * and its worker threads.
 *
 * Sockets are real host descriptors.  Structures whose layout differs between
 * 32-bit ARM bionic and AArch64 are converted: timeval, ifconf/ifreq, hostent
 * and addrinfo.  sockaddr, socklen_t and fd_set have identical layouts and are
 * passed through.
 */
#include "runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define A0 (cpu->r[0])
#define A1 (cpu->r[1])
#define A2 (cpu->r[2])
#define A3 (cpu->r[3])
#define STK(i) rd32(cpu->r[13] + 4u * (i))
#define RET(v) (cpu->r[0] = (uint32_t)(v))

/* Guest scratch for results that bionic returned in static storage. */
#define NET_STATIC 0x70004000u     /* hostent: 0x400 bytes */

static void *buf(CPU *cpu, uint32_t addr, uint32_t len)
{
    if (!len) return G(addr);
    if (!a5_guest_range(addr, len)) guest_abort(cpu, "network buffer 0x%08x+0x%x", addr, len);
    return G(addr);
}

static void *opt_buf(CPU *cpu, uint32_t addr, uint32_t len)
{
    return addr ? buf(cpu, addr, len) : NULL;
}

static int32_t result(long value)
{
    if (value < 0) a5_set_errno(errno);
    return (int32_t)value;
}

/* ---- descriptor ownership ------------------------------------------- */

#define MAX_FDS 4096
static _Atomic uint8_t g_owned[MAX_FDS];

static bool owned(int fd) { return fd >= 0 && fd < MAX_FDS && g_owned[fd]; }
static void own(int fd) { if (fd >= 0 && fd < MAX_FDS) g_owned[fd] = 1; }

static bool check_fd(CPU *cpu, int fd)
{
    if (owned(fd)) return true;
    a5_set_errno(EBADF);
    RET(-1);
    return false;
}

/* ---- sockets -------------------------------------------------------- */

static void imp_socket(CPU *cpu)
{
    int fd = socket((int)A0, (int)A1 | SOCK_CLOEXEC, (int)A2);
    if (fd >= MAX_FDS) { close(fd); fd = -1; errno = EMFILE; }
    own(fd);
    RET(result(fd));
}

static void imp_close(CPU *cpu)
{
    int fd = (int)A0;
    /* Only descriptors the engine created are closed; others were never real. */
    if (!owned(fd)) { RET(0); return; }
    g_owned[fd] = 0;
    RET(result(close(fd)));
}

static void imp_bind(CPU *cpu)
{
    if (!check_fd(cpu, (int)A0)) return;
    RET(result(bind((int)A0, buf(cpu, A1, A2), (socklen_t)A2)));
}

static void imp_connect(CPU *cpu)
{
    if (!check_fd(cpu, (int)A0)) return;
    RET(result(connect((int)A0, buf(cpu, A1, A2), (socklen_t)A2)));
}

static void imp_listen(CPU *cpu)
{
    if (!check_fd(cpu, (int)A0)) return;
    RET(result(listen((int)A0, (int)A1)));
}

static void imp_accept(CPU *cpu)
{
    if (!check_fd(cpu, (int)A0)) return;
    socklen_t *len = opt_buf(cpu, A2, 4);
    void *addr = A1 && len ? buf(cpu, A1, *len) : NULL;
    int fd = accept4((int)A0, addr, addr ? len : NULL, SOCK_CLOEXEC);
    own(fd);
    RET(result(fd));
}

static void imp_send(CPU *cpu)
{
    if (!check_fd(cpu, (int)A0)) return;
    RET(result(send((int)A0, buf(cpu, A1, A2), A2, (int)A3 | MSG_NOSIGNAL)));
}

static void imp_recv(CPU *cpu)
{
    if (!check_fd(cpu, (int)A0)) return;
    RET(result(recv((int)A0, buf(cpu, A1, A2), A2, (int)A3)));
}

static void imp_sendto(CPU *cpu)
{
    if (!check_fd(cpu, (int)A0)) return;
    uint32_t addr = STK(0), len = STK(1);
    RET(result(sendto((int)A0, buf(cpu, A1, A2), A2, (int)A3 | MSG_NOSIGNAL,
                      opt_buf(cpu, addr, len), (socklen_t)len)));
}

static void imp_recvfrom(CPU *cpu)
{
    if (!check_fd(cpu, (int)A0)) return;
    uint32_t addr = STK(0), len_addr = STK(1);
    socklen_t *len = opt_buf(cpu, len_addr, 4);
    void *from = addr && len ? buf(cpu, addr, *len) : NULL;
    RET(result(recvfrom((int)A0, buf(cpu, A1, A2), A2, (int)A3, from, from ? len : NULL)));
}

static bool timeval_option(int level, int name)
{
    return level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO);
}

static void imp_setsockopt(CPU *cpu)
{
    int fd = (int)A0, level = (int)A1, name = (int)A2;
    uint32_t value = A3, len = STK(0);
    if (!check_fd(cpu, fd)) return;
    if (timeval_option(level, name) && len == 8) {
        struct timeval tv = {(int32_t)rd32(value), (int32_t)rd32(value + 4)};
        buf(cpu, value, 8);
        RET(result(setsockopt(fd, level, name, &tv, sizeof(tv))));
        return;
    }
    RET(result(setsockopt(fd, level, name, opt_buf(cpu, value, len), (socklen_t)len)));
}

static void imp_getsockopt(CPU *cpu)
{
    int fd = (int)A0, level = (int)A1, name = (int)A2;
    uint32_t value = A3, len_addr = STK(0);
    if (!check_fd(cpu, fd)) return;
    socklen_t *len = buf(cpu, len_addr, 4);
    if (timeval_option(level, name) && *len >= 8) {
        struct timeval tv;
        socklen_t host_len = sizeof(tv);
        int r = getsockopt(fd, level, name, &tv, &host_len);
        if (r == 0) { buf(cpu, value, 8); wr32(value, (uint32_t)tv.tv_sec); wr32(value + 4, (uint32_t)tv.tv_usec); *len = 8; }
        RET(result(r));
        return;
    }
    RET(result(getsockopt(fd, level, name, buf(cpu, value, *len), len)));
}

static void imp_getsockname(CPU *cpu)
{
    if (!check_fd(cpu, (int)A0)) return;
    socklen_t *len = buf(cpu, A2, 4);
    RET(result(getsockname((int)A0, buf(cpu, A1, *len), len)));
}

static void imp_select(CPU *cpu)
{
    /* fd_set is a 1024-bit little-endian bitmap on both ABIs. */
    int nfds = (int)A0;
    if (nfds < 0 || nfds > FD_SETSIZE) { a5_set_errno(EINVAL); RET(-1); return; }
    fd_set *sets[3];
    uint32_t guest[3] = {A1, A2, A3};
    for (int i = 0; i < 3; ++i) sets[i] = opt_buf(cpu, guest[i], sizeof(fd_set));
    for (int i = 0; i < 3; ++i) {
        if (!sets[i]) continue;
        for (int fd = 0; fd < nfds; ++fd) {
            if (FD_ISSET(fd, sets[i]) && !owned(fd)) { a5_set_errno(EBADF); RET(-1); return; }
        }
    }
    uint32_t tv_addr = STK(0);
    struct timeval tv, *timeout = NULL;
    if (tv_addr) {
        buf(cpu, tv_addr, 8);
        tv.tv_sec = (int32_t)rd32(tv_addr);
        tv.tv_usec = (int32_t)rd32(tv_addr + 4);
        timeout = &tv;
    }
    int r = select(nfds, sets[0], sets[1], sets[2], timeout);
    if (timeout) { wr32(tv_addr, (uint32_t)tv.tv_sec); wr32(tv_addr + 4, (uint32_t)tv.tv_usec); }
    RET(result(r));
}

static void imp_fcntl(CPU *cpu)
{
    int fd = (int)A0, cmd = (int)A1;
    if (!owned(fd)) { RET(0); return; }
    switch (cmd) {
    case F_GETFL: case F_GETFD:
        RET(result(fcntl(fd, cmd)));
        return;
    case F_SETFL: case F_SETFD:   /* O_NONBLOCK and FD_CLOEXEC share values */
        RET(result(fcntl(fd, cmd, (int)A2)));
        return;
    default:
        a5_set_errno(EINVAL);
        RET(-1);
    }
}

/* 32-bit ifreq: 16-byte name + 16-byte union. */
#define GUEST_IFREQ 32u

static void get_ifconf(CPU *cpu, uint32_t ifc)
{
    buf(cpu, ifc, 8);
    int32_t capacity = (int32_t)rd32(ifc);
    uint32_t out = rd32(ifc + 4);
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) { RET(result(-1)); return; }
    uint32_t used = 0;
    for (struct ifaddrs *it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if (used + GUEST_IFREQ > (uint32_t)capacity) break;
        uint8_t *entry = buf(cpu, out + used, GUEST_IFREQ);
        memset(entry, 0, GUEST_IFREQ);
        snprintf((char *)entry, IFNAMSIZ, "%s", it->ifa_name);
        memcpy(entry + 16, it->ifa_addr, sizeof(struct sockaddr_in));
        used += GUEST_IFREQ;
    }
    freeifaddrs(list);
    wr32(ifc, used);
    RET(0);
}

static void ifreq_call(CPU *cpu, int fd, unsigned long request, uint32_t guest)
{
    struct ifreq req;
    memset(&req, 0, sizeof(req));
    memcpy(req.ifr_name, buf(cpu, guest, GUEST_IFREQ), IFNAMSIZ);
    req.ifr_name[IFNAMSIZ - 1] = 0;
    memcpy(&req.ifr_addr, G(guest + 16), 16);
    int r = ioctl(fd, request, &req);
    if (r == 0) memcpy(G(guest + 16), &req.ifr_addr, 16);   /* flags / address / netmask */
    RET(result(r));
}

static void imp_ioctl(CPU *cpu)
{
    int fd = (int)A0;
    uint32_t request = A1, arg = A2;
    if (!owned(fd)) { a5_set_errno(EBADF); RET(-1); return; }
    switch (request) {
    case FIONBIO: case FIONREAD:
        RET(result(ioctl(fd, request, (int *)buf(cpu, arg, 4))));
        return;
    case SIOCGIFCONF:
        get_ifconf(cpu, arg);
        return;
    case SIOCGIFFLAGS: case SIOCGIFADDR: case SIOCGIFBRDADDR: case SIOCGIFNETMASK:
        ifreq_call(cpu, fd, request, arg);
        return;
    default:
        A5_WARN("unsupported ioctl 0x%x", request);
        a5_set_errno(EINVAL);
        RET(-1);
    }
}

/* ---- name resolution ------------------------------------------------ */

/* Player name shown in the local lobby (the engine uses gethostname()). */
static char g_hostname[32];

void a5_set_hostname(const char *name)
{
    size_t n = 0;
    for (const char *p = name; *p && n + 1 < sizeof(g_hostname); ++p) {
        unsigned char c = (unsigned char)*p;
        g_hostname[n++] = c >= 32 && c < 127 ? (char)c : '_';
    }
    g_hostname[n] = 0;
}

/* First non-loopback IPv4 address (Wi-Fi or hotspot), network byte order. */
static bool local_ipv4(struct in_addr *out)
{
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) return false;
    bool found = false;
    for (struct ifaddrs *it = list; it && !found; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET || (it->ifa_flags & IFF_LOOPBACK) ||
            !(it->ifa_flags & IFF_UP)) continue;
        *out = ((struct sockaddr_in *)it->ifa_addr)->sin_addr;
        found = true;
    }
    freeifaddrs(list);
    return found;
}

static void imp_gethostname(CPU *cpu)
{
    char name[256];
    if (g_hostname[0]) snprintf(name, sizeof(name), "%s", g_hostname);
    else if (gethostname(name, sizeof(name)) != 0) snprintf(name, sizeof(name), "localhost");
    if (!A1) { RET(0); return; }
    snprintf(buf(cpu, A0, A1), A1, "%s", name);
    RET(0);
}

static pthread_mutex_t g_resolver_lock = PTHREAD_MUTEX_INITIALIZER;

static void imp_gethostbyname(CPU *cpu)
{
    const char *name = a5_guest_cstr(cpu, A0);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *list;
    struct sockaddr_in own_address = {.sin_family = AF_INET};
    struct addrinfo own = {.ai_family = AF_INET, .ai_addrlen = sizeof(own_address),
                           .ai_addr = (struct sockaddr *)&own_address};
    bool self = g_hostname[0] && !strcmp(name, g_hostname);
    if (self) {
        /* Our own player name resolves to the Wi-Fi address. */
        if (!local_ipv4(&own_address.sin_addr)) { RET(0); return; }
        list = &own;
    } else if (getaddrinfo(name, NULL, &hints, &list) != 0) {
        RET(0);
        return;
    }
    pthread_mutex_lock(&g_resolver_lock);
    /* bionic: struct hostent { char *h_name; char **h_aliases; int h_addrtype;
     *                          int h_length; char **h_addr_list; } (20 bytes) */
    const uint32_t h = NET_STATIC, aliases = h + 20, addr_list = h + 24, addrs = h + 24 + 4 * 17,
                   name_copy = addrs + 4 * 16;
    uint32_t count = 0;
    for (struct addrinfo *it = list; it && count < 16; it = it->ai_next) {
        memcpy(G(addrs + 4 * count), &((struct sockaddr_in *)it->ai_addr)->sin_addr, 4);
        wr32(addr_list + 4 * count, addrs + 4 * count);
        ++count;
    }
    wr32(addr_list + 4 * count, 0);
    if (!self) freeaddrinfo(list);
    snprintf((char *)G(name_copy), 256, "%s", name);
    wr32(h, name_copy);
    wr32(h + 4, aliases);
    wr32(aliases, 0);
    wr32(h + 8, AF_INET);
    wr32(h + 12, 4);
    wr32(h + 16, addr_list);
    pthread_mutex_unlock(&g_resolver_lock);
    RET(h);
}

static void imp_getaddrinfo(CPU *cpu)
{
    /* 32-bit addrinfo: flags, family, socktype, protocol, addrlen,
     * canonname*, addr*, next* (32 bytes). */
    const char *node = A0 ? a5_guest_cstr(cpu, A0) : NULL;
    const char *service = A1 ? a5_guest_cstr(cpu, A1) : NULL;
    struct addrinfo hints = {0}, *list;
    if (A2) {
        buf(cpu, A2, 32);
        hints.ai_flags = (int)rd32(A2);
        hints.ai_family = (int)rd32(A2 + 4);
        hints.ai_socktype = (int)rd32(A2 + 8);
        hints.ai_protocol = (int)rd32(A2 + 12);
    }
    buf(cpu, A3, 4);
    int r = getaddrinfo(node, service, A2 ? &hints : NULL, &list);
    if (r != 0) { wr32(A3, 0); RET(r); return; }
    uint32_t head = 0, *link = &head;
    uint32_t tail_slot = 0;
    for (struct addrinfo *it = list; it; it = it->ai_next) {
        if (it->ai_addrlen > 28) continue;
        uint32_t node_addr = guest_alloc_zero(32 + 28);
        wr32(node_addr, (uint32_t)it->ai_flags);
        wr32(node_addr + 4, (uint32_t)it->ai_family);
        wr32(node_addr + 8, (uint32_t)it->ai_socktype);
        wr32(node_addr + 12, (uint32_t)it->ai_protocol);
        wr32(node_addr + 16, (uint32_t)it->ai_addrlen);
        memcpy(G(node_addr + 32), it->ai_addr, it->ai_addrlen);
        wr32(node_addr + 24, node_addr + 32);
        if (tail_slot) wr32(tail_slot, node_addr); else *link = node_addr;
        tail_slot = node_addr + 28;
    }
    freeaddrinfo(list);
    wr32(A3, head);
    RET(head ? 0 : EAI_NONAME);
}

static void imp_freeaddrinfo(CPU *cpu)
{
    for (uint32_t node = A0; node; ) {
        uint32_t next = rd32(node + 28);
        guest_free(node);
        node = next;
    }
}

static void imp_gethostbyaddr(CPU *cpu) { (void)cpu; RET(0); }
static void imp_herror(CPU *cpu) { (void)cpu; }

static void imp_sigaction(CPU *cpu)
{
    /* The engine ignores SIGPIPE (CAndroidSocket::Startup).  Sends use
     * MSG_NOSIGNAL; nothing else is installed into the host process. */
    if (A2) { buf(cpu, A2, 16); memset(G(A2), 0, 16); }
    RET(0);
}

/* ---- threads -------------------------------------------------------- */

#define MAX_THREADS 64
#define THREAD_STACK (1u << 20)

typedef struct {
    pthread_t host;
    uint32_t start, arg, result;
    bool used, joined;
} GuestThread;

static GuestThread g_threads[MAX_THREADS];
static pthread_mutex_t g_thread_lock = PTHREAD_MUTEX_INITIALIZER;

static void *thread_main(void *data)
{
    GuestThread *t = data;
    uint32_t stack = guest_alloc(THREAD_STACK);
    CPU cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[13] = (stack + THREAD_STACK - 16u) & ~7u;
    cpu.cpsr = 0x10;
    cpu.tid = (uint32_t)(t - g_threads) + 2u;
    cpu.r[0] = t->arg;
    a5_jni_attach_thread();
    a5_call(&cpu, t->start);
    t->result = cpu.r[0];
    a5_jni_detach_thread();
    guest_free(stack);
    return NULL;
}

static void imp_pthread_create(CPU *cpu)
{
    uint32_t thread_out = A0, start = A2, arg = A3;
    pthread_mutex_lock(&g_thread_lock);
    int slot = -1;
    for (int i = 0; i < MAX_THREADS; ++i) if (!g_threads[i].used) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&g_thread_lock); RET(EAGAIN); return; }
    GuestThread *t = &g_threads[slot];
    *t = (GuestThread){.start = start, .arg = arg, .used = true};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    int r = pthread_create(&t->host, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
    if (r != 0) t->used = false;
    pthread_mutex_unlock(&g_thread_lock);
    if (r == 0 && thread_out) { buf(cpu, thread_out, 4); wr32(thread_out, (uint32_t)slot + 1u); }
    RET(r);
}

static void imp_pthread_join(CPU *cpu)
{
    uint32_t id = A0;
    if (!id || id > MAX_THREADS || !g_threads[id - 1].used) { RET(ESRCH); return; }
    GuestThread *t = &g_threads[id - 1];
    int r = pthread_join(t->host, NULL);
    if (r == 0) {
        if (A1) { buf(cpu, A1, 4); wr32(A1, t->result); }
        pthread_mutex_lock(&g_thread_lock);
        t->used = false;
        pthread_mutex_unlock(&g_thread_lock);
    }
    RET(r);
}

/* ---- registry ------------------------------------------------------- */

typedef struct { const char *name; GuestFn fn; } NetEntry;

static const NetEntry k_net[] = {
    {"socket", imp_socket}, {"close", imp_close}, {"bind", imp_bind}, {"connect", imp_connect},
    {"listen", imp_listen}, {"accept", imp_accept}, {"send", imp_send}, {"recv", imp_recv},
    {"sendto", imp_sendto}, {"recvfrom", imp_recvfrom}, {"setsockopt", imp_setsockopt},
    {"getsockopt", imp_getsockopt}, {"getsockname", imp_getsockname}, {"select", imp_select},
    {"fcntl", imp_fcntl}, {"ioctl", imp_ioctl}, {"gethostname", imp_gethostname},
    {"gethostbyname", imp_gethostbyname}, {"gethostbyaddr", imp_gethostbyaddr},
    {"getaddrinfo", imp_getaddrinfo}, {"freeaddrinfo", imp_freeaddrinfo}, {"herror", imp_herror},
    {"sigaction", imp_sigaction}, {"pthread_create", imp_pthread_create},
    {"pthread_join", imp_pthread_join},
};

GuestFn a5_net_handler(const char *name)
{
    for (size_t i = 0; i < sizeof(k_net) / sizeof(k_net[0]); ++i) {
        if (!strcmp(k_net[i].name, name)) return k_net[i].fn;
    }
    return NULL;
}
