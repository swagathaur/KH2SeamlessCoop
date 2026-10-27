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
