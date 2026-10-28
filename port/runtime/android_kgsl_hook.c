// The custom GPU driver's ioctl() calls are pointed here (its import table is
// patched after loading: kh2_kgsl_patch_driver, from android_driver.cpp).
//
// Turnip on Qualcomm's kernel driver (KGSL) emulates Vulkan timeline semaphores;
// Mesa's clean-up of old timeline points checks each with a zero timeout,
// expecting an immediate answer. KGSL's WAITTIMESTAMP ioctl with a zero timeout
// waited for the GPU instead, so every submit and every frame-context wait
// waited for all GPU work in flight (tested on an Adreno 740: the render thread
// spent ~85% of its time there). Here a zero-timeout wait becomes a check of the
// retired timestamp that returns at once.
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <linux/ioctl.h> // _IOW/_IOWR only: bionic declares ioctl() itself as overloadable
#include <time.h>

#define KGSL_IOC_TYPE 0x09
struct kh2_kgsl_waittimestamp_ctxtid
{
    unsigned int context_id;
    unsigned int timestamp;
    unsigned int timeout; // ms
};
struct kh2_kgsl_readtimestamp_ctxtid
{
    unsigned int context_id;
    unsigned int type;
    unsigned int timestamp; // out
};
#define KH2_IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID _IOW(KGSL_IOC_TYPE, 0x7, struct kh2_kgsl_waittimestamp_ctxtid)
#define KH2_IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID _IOWR(KGSL_IOC_TYPE, 0x16, struct kh2_kgsl_readtimestamp_ctxtid)
#define KH2_KGSL_TIMESTAMP_RETIRED 2

typedef int (*ioctl_fn)(int, unsigned long, ...);

static ioctl_fn real_ioctl(void)
{
    static ioctl_fn fn;
    if (!fn)
        fn = (ioctl_fn)dlsym(RTLD_DEFAULT, "ioctl");
    return fn;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t s_zero_calls, s_zero_busy, s_waits, s_wait_ns, s_last_log;

// ---- GPU occupancy probe (KH2_GPU_PROBE=1) ------------------------------------
// The kernel's GPU-busy counters stay frozen with a custom driver, so this
// measures occupancy itself: each submission (GPU_COMMAND ioctl) is recorded
// with its timestamp, and a thread polls the retired timestamp every 0.2 ms.
// "Busy" is the time the queue held unfinished work (including work waiting on
// fences), which is an upper bound on real GPU time.
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <stdlib.h>

#define KH2_KGSL_GPU_COMMAND_NR 0x4A
struct kh2_gpu_command_tail
{
    unsigned int context_id;
    unsigned int timestamp;
};
struct kh2_pending
{
    unsigned int ts;
    int slot; // profiling slot, or -1
    uint64_t submit_ns;
};
// Kernel-written per-submission profile (KGSL_CMDBATCH_PROFILING): GPU
// always-on ticks (19.2 MHz) when the GPU started and finished the work.
struct kh2_profile
{
    uint64_t wall_s, wall_ns, ticks_queued, ticks_submitted, ticks_retired;
};
struct kh2_command_object
{
    uint64_t offset, gpuaddr, size;
    unsigned int flags, id;
};
struct kh2_gpuobj_alloc
{
    uint64_t size, flags, va_len, mmapsize;
    unsigned int id, metadata_len;
    uint64_t metadata;
};
#define KH2_IOCTL_KGSL_GPUOBJ_ALLOC _IOWR(KGSL_IOC_TYPE, 0x45, struct kh2_gpuobj_alloc)
#define KH2_KGSL_CMDBATCH_PROFILING 0x10
#define KH2_KGSL_OBJLIST_PROFILE 0x10 // msm_kgsl.h: KGSL_OBJLIST_PROFILE (MEMOBJ is 0x8)
#define KH2_PROFILE_SLOTS 1024
static volatile struct kh2_profile *s_prof;
static unsigned s_prof_id;
static unsigned s_prof_next;
static int s_prof_failed;
static pthread_mutex_t s_probe_lock = PTHREAD_MUTEX_INITIALIZER;
static struct kh2_pending s_pending[1024];
static unsigned s_pend_head, s_pend_tail;
static int s_probe_fd = -1;
static unsigned s_probe_ctx;
static uint64_t s_submits, s_submits_sync;
static int s_probe_state; // 0 unknown, 1 on, -1 off

static void *kh2_probe_thread(void *unused)
{
    (void)unused;
    uint64_t busy = 0, last_retire = 0, max_lat = 0, lat_sum = 0, lat_n = 0, t_log = now_ns();
    uint64_t exec_ticks = 0, last_end_tick = 0, prof_n = 0, wait_ticks = 0;
    for (;;)
    {
        struct timespec sl = {0, 200000};
        nanosleep(&sl, NULL);
        pthread_mutex_lock(&s_probe_lock);
        const int fd = s_probe_fd;
        const unsigned ctx = s_probe_ctx;
        const int empty = s_pend_head == s_pend_tail;
        pthread_mutex_unlock(&s_probe_lock);
        const uint64_t now = now_ns();
        if (fd >= 0 && !empty)
        {
            struct kh2_kgsl_readtimestamp_ctxtid read = {ctx, KH2_KGSL_TIMESTAMP_RETIRED, 0};
            if (real_ioctl()(fd, KH2_IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID, &read) == 0)
            {
                pthread_mutex_lock(&s_probe_lock);
                while (s_pend_head != s_pend_tail &&
                       (int32_t)(read.timestamp - s_pending[s_pend_head % 1024].ts) >= 0)
                {
                    const struct kh2_pending *e = &s_pending[s_pend_head % 1024];
                    const uint64_t start = e->submit_ns > last_retire ? e->submit_ns : last_retire;
                    busy += now - start;
                    const uint64_t lat = now - e->submit_ns;
                    lat_sum += lat;
                    lat_n++;
                    if (lat > max_lat)
                        max_lat = lat;
                    last_retire = now;
                    if (e->slot >= 0 && s_prof)
                    {
                        const volatile struct kh2_profile *pr = &s_prof[e->slot];
                        const uint64_t a = pr->ticks_submitted, b = pr->ticks_retired, q = pr->ticks_queued;
                        if (b > a && b - a < 19200000ull)
                        {
                            const uint64_t st = a > last_end_tick ? a : last_end_tick;
                            if (b > st)
                                exec_ticks += b - st;
                            if (b > last_end_tick)
                                last_end_tick = b;
                            if (q && a > q && a - q < 19200000ull)
                                wait_ticks += a - q;
                            prof_n++;
                        }
                    }
                    s_pend_head++;
                }
                pthread_mutex_unlock(&s_probe_lock);
            }
        }
        if (now - t_log > 3000000000ull)
        {
            const double secs = (double)(now - t_log) / 1e9;
            __android_log_print(ANDROID_LOG_INFO, "kh2-kgsl",
                                "[gpu-probe] queue busy %.0f%% | submits %.0f/s (%.0f/s waiting on fences) | "
                                "submit->retire avg %.2f ms, max %.2f ms | GPU executing %.0f%% (%.0f profiled/s, "
                                "queued->started avg %.2f ms)",
                                (double)busy / 1e7 / secs, (double)s_submits / secs, (double)s_submits_sync / secs,
                                lat_n ? (double)lat_sum / lat_n / 1e6 : 0.0, (double)max_lat / 1e6,
                                (double)exec_ticks / 192000.0 / secs, (double)prof_n / secs,
                                prof_n ? (double)wait_ticks / prof_n / 19200.0 : 0.0);
            exec_ticks = wait_ticks = prof_n = 0;
            busy = max_lat = lat_sum = lat_n = 0;
            s_submits = s_submits_sync = 0;
            t_log = now;
        }
    }
    return NULL;
}

static int kh2_probe_enabled(void)
{
    if (s_probe_state == 0)
    {
        const char *e = getenv("KH2_GPU_PROBE");
        s_probe_state = (e && e[0] == '1') ? 1 : -1;
        if (s_probe_state > 0)
        {
            pthread_t t;
            pthread_create(&t, NULL, kh2_probe_thread, NULL);
            pthread_detach(t);
        }
    }
    return s_probe_state > 0;
}

static void kh2_probe_alloc(int fd)
{
    if (s_prof || s_prof_failed)
        return;
    struct kh2_gpuobj_alloc al;
    memset(&al, 0, sizeof(al));
    al.size = KH2_PROFILE_SLOTS * sizeof(struct kh2_profile);
    if (real_ioctl()(fd, KH2_IOCTL_KGSL_GPUOBJ_ALLOC, &al) != 0)
    {
        s_prof_failed = 1;
        __android_log_print(ANDROID_LOG_WARN, "kh2-kgsl", "[gpu-probe] profiling buffer alloc failed (%d)", errno);
        return;
    }
    void *p = mmap(NULL, (size_t)al.mmapsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)al.id << 12);
    if (p == MAP_FAILED)
    {
        s_prof_failed = 1;
        __android_log_print(ANDROID_LOG_WARN, "kh2-kgsl", "[gpu-probe] profiling buffer mmap failed (%d)", errno);
        return;
    }
    memset(p, 0, (size_t)al.size);
    s_prof_id = al.id;
    s_prof = (volatile struct kh2_profile *)p;
}

static void kh2_probe_submit(int fd, void *arg, int slot)
{
    // struct kgsl_gpu_command ends with context_id, timestamp (offset 56); numsyncs at 52.
    const struct kh2_gpu_command_tail *tail = (const struct kh2_gpu_command_tail *)((const char *)arg + 56);
    const unsigned numsyncs = *(const unsigned int *)((const char *)arg + 52);
    pthread_mutex_lock(&s_probe_lock);
    s_probe_fd = fd;
    s_probe_ctx = tail->context_id;
    s_submits++;
    if (numsyncs)
        s_submits_sync++;
    if (s_pend_tail - s_pend_head < 1024)
    {
        s_pending[s_pend_tail % 1024].ts = tail->timestamp;
        s_pending[s_pend_tail % 1024].slot = slot;
        s_pending[s_pend_tail % 1024].submit_ns = now_ns();
        s_pend_tail++;
    }
    pthread_mutex_unlock(&s_probe_lock);
}

int kh2_kgsl_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    ioctl_fn real = real_ioctl();
    if (!real)
    {
        errno = ENOSYS;
        return -1;
    }
    if (_IOC_TYPE(request) == KGSL_IOC_TYPE && _IOC_NR(request) == KH2_KGSL_GPU_COMMAND_NR &&
        _IOC_SIZE(request) == 64u) // struct kgsl_gpu_command as laid out below
    {
        if (!kh2_probe_enabled())
            return real(fd, request, arg);
        kh2_probe_alloc(fd);
        // Ask the kernel to profile this submission: a copy of the command
        // with one more object (our profiling slot) and the profiling flag.
        unsigned char cmd[64];
        memcpy(cmd, arg, 64);
        uint64_t flags, objlist;
        unsigned objsize, numobjs;
        memcpy(&flags, cmd + 0, 8);
        memcpy(&objlist, cmd + 24, 8);
        memcpy(&objsize, cmd + 32, 4);
        memcpy(&numobjs, cmd + 36, 4);
        int slot = -1;
        struct kh2_command_object objs[17];
        if (s_prof && numobjs < 16 && (numobjs == 0 || objsize == sizeof(struct kh2_command_object)))
        {
            if (numobjs)
                memcpy(objs, (const void *)(uintptr_t)objlist, numobjs * sizeof(objs[0]));
            slot = (int)(__atomic_fetch_add(&s_prof_next, 1u, __ATOMIC_RELAXED) % KH2_PROFILE_SLOTS);
            memset((void *)&s_prof[slot], 0, sizeof(struct kh2_profile));
            objs[numobjs].offset = (uint64_t)slot * sizeof(struct kh2_profile);
            objs[numobjs].gpuaddr = 0;
            objs[numobjs].size = sizeof(struct kh2_profile);
            objs[numobjs].flags = KH2_KGSL_OBJLIST_PROFILE;
            objs[numobjs].id = s_prof_id;
            flags |= KH2_KGSL_CMDBATCH_PROFILING;
            objlist = (uint64_t)(uintptr_t)objs;
            objsize = sizeof(struct kh2_command_object);
            numobjs++;
            memcpy(cmd + 0, &flags, 8);
            memcpy(cmd + 24, &objlist, 8);
            memcpy(cmd + 32, &objsize, 4);
            memcpy(cmd + 36, &numobjs, 4);
        }
        int r = real(fd, request, slot >= 0 ? (void *)cmd : arg);
        if (r != 0 && slot >= 0 && errno != EINVAL && errno != EFAULT && errno != ENOENT)
            return r; // a transient failure (busy, interrupted): the driver retries as it would have
        if (r != 0 && slot >= 0)
        {
            // The kernel refused the profiled form: stop profiling, submit as is.
            static int warned;
            if (!warned++)
                __android_log_print(ANDROID_LOG_WARN, "kh2-kgsl", "[gpu-probe] profiled submit refused (%d)", errno);
            s_prof_failed = 1;
            s_prof = NULL;
            slot = -1;
            r = real(fd, request, arg);
        }
        else if (r == 0 && slot >= 0)
            memcpy((char *)arg + 56, cmd + 56, 8); // context_id, timestamp back to the caller
        if (r == 0)
            kh2_probe_submit(fd, arg, slot);
        return r;
    }
    if (request != KH2_IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID)
        return real(fd, request, arg);

    struct kh2_kgsl_waittimestamp_ctxtid *wait = (struct kh2_kgsl_waittimestamp_ctxtid *)arg;
    int ret;
    if (wait->timeout == 0)
    {
        // A poll: has the GPU retired this timestamp yet?
        struct kh2_kgsl_readtimestamp_ctxtid read = {wait->context_id, KH2_KGSL_TIMESTAMP_RETIRED, 0};
        s_zero_calls++;
        if (real(fd, KH2_IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID, &read) == 0)
        {
            if ((int32_t)(read.timestamp - wait->timestamp) >= 0)
                ret = 0;
            else
            {
                s_zero_busy++;
                errno = ETIMEDOUT;
                ret = -1;
            }
        }
        else
            ret = real(fd, request, arg); // could not read it: as before
    }
    else
    {
        const uint64_t t0 = now_ns();
        ret = real(fd, request, arg);
        s_waits++;
        s_wait_ns += now_ns() - t0;
    }
    const uint64_t t = now_ns();
    if (!s_last_log)
        s_last_log = t;
    if (t - s_last_log > 3000000000ull)
    {
        const double secs = (double)(t - s_last_log) / 1e9;
        __android_log_print(ANDROID_LOG_INFO, "kh2-kgsl",
                            "timestamp polls %.0f/s (%.0f/s not done yet) | real waits %.0f/s, %.1f ms/s",
                            (double)s_zero_calls / secs, (double)s_zero_busy / secs, (double)s_waits / secs,
                            (double)s_wait_ns / 1e6 / secs);
        s_zero_calls = s_zero_busy = s_waits = s_wait_ns = 0;
        s_last_log = t;
    }
    return ret;
}

// ---- Import-table patch ------------------------------------------------------
#include <elf.h>
#include <link.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct kh2_patch_ctx
{
    const char *name;
    int patched;
};

static int kh2_patch_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    struct kh2_patch_ctx *ctx = (struct kh2_patch_ctx *)data;
    if (!info->dlpi_name)
        return 0;
    if (!strstr(info->dlpi_name, ctx->name))
        return 0;
    const ElfW(Dyn) *dyn = NULL;
    for (int i = 0; i < info->dlpi_phnum; ++i)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
            dyn = (const ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    if (!dyn)
        return 0;
    const ElfW(Sym) *symtab = NULL;
    const char *strtab = NULL;
    const ElfW(Rela) *jmprel = NULL, *rela = NULL;
    size_t jmprelsz = 0, relasz = 0;
    for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; ++d)
    {
        switch (d->d_tag)
        {
        case DT_SYMTAB: symtab = (const ElfW(Sym) *)(info->dlpi_addr + d->d_un.d_ptr); break;
        case DT_STRTAB: strtab = (const char *)(info->dlpi_addr + d->d_un.d_ptr); break;
        case DT_JMPREL: jmprel = (const ElfW(Rela) *)(info->dlpi_addr + d->d_un.d_ptr); break;
        case DT_PLTRELSZ: jmprelsz = d->d_un.d_val; break;
        case DT_RELA: rela = (const ElfW(Rela) *)(info->dlpi_addr + d->d_un.d_ptr); break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        default: break;
        }
    }
    if (!symtab || !strtab)
        return 0;
    const long page = sysconf(_SC_PAGESIZE);
    const ElfW(Rela) *tables[2] = {jmprel, rela};
    const size_t sizes[2] = {jmprelsz, relasz};
    for (int t = 0; t < 2; ++t)
    {
        if (!tables[t])
            continue;
        for (size_t i = 0; i < sizes[t] / sizeof(ElfW(Rela)); ++i)
        {
            const ElfW(Rela) *r = &tables[t][i];
            const unsigned type = (unsigned)ELF64_R_TYPE(r->r_info);
            if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT)
                continue;
            const ElfW(Sym) *sym = &symtab[ELF64_R_SYM(r->r_info)];
            if (strcmp(strtab + sym->st_name, "ioctl") != 0)
                continue;
            void **slot = (void **)(info->dlpi_addr + r->r_offset);
            uintptr_t start = (uintptr_t)slot & ~(uintptr_t)(page - 1);
            if (mprotect((void *)start, (size_t)page, PROT_READ | PROT_WRITE) != 0)
                continue;
            *slot = (void *)&kh2_kgsl_ioctl;
            ctx->patched++;
        }
    }
    __android_log_print(ANDROID_LOG_INFO, "kh2-kgsl", "patched %d ioctl import(s) in %s", ctx->patched,
                        info->dlpi_name);
    return ctx->patched ? 1 : 0;
}

// Patches the import table of a library mapped at load bias `bias` with
// program headers at `phdr`.
static int kh2_patch_image(uintptr_t bias, const ElfW(Phdr) *phdr, int phnum, const char *name)
{
    struct dl_phdr_info info;
    memset(&info, 0, sizeof(info));
    info.dlpi_addr = bias;
    info.dlpi_name = name;
    info.dlpi_phdr = phdr;
    info.dlpi_phnum = (ElfW(Half))phnum;
    struct kh2_patch_ctx ctx = {name, 0};
    kh2_patch_cb(&info, sizeof(info), &ctx);
    return ctx.patched;
}

// Points the loaded driver's ioctl imports at kh2_kgsl_ioctl. libName: part
// of the driver library's path. The driver lives in a linker namespace of its
// own (libadrenotools), so it is found in /proc/self/maps rather than through
// dl_iterate_phdr. Returns how many imports were patched.
int kh2_kgsl_patch_driver(const char *libName)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps)
        return 0;
    char line[1024];
    int patched = 0;
    while (fgets(line, sizeof(line), maps))
    {
        unsigned long start, end, offset;
        char perms[8];
        int pathAt = 0;
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %n", &start, &end, perms, &offset, &pathAt) < 4 || !pathAt)
            continue;
        char *path = line + pathAt;
        path[strcspn(path, "\n")] = 0;
        if (offset != 0 || !strstr(path, libName) || perms[0] != 'r')
            continue;
        const ElfW(Ehdr) *eh = (const ElfW(Ehdr) *)start;
        if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0)
            continue;
        const ElfW(Phdr) *ph = (const ElfW(Phdr) *)(start + eh->e_phoff);
        uintptr_t firstLoad = 0;
        for (int i = 0; i < eh->e_phnum; ++i)
            if (ph[i].p_type == PT_LOAD)
            {
                firstLoad = ph[i].p_vaddr & ~(uintptr_t)0xFFF;
                break;
            }
        patched += kh2_patch_image(start - firstLoad, ph, eh->e_phnum, path);
    }
    fclose(maps);
    return patched;
}
