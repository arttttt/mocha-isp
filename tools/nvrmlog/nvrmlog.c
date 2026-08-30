/*
 * nvrmlog -- LD_PRELOAD interceptor for NvRmMemHandleAllocAttr.
 *
 * Purpose: the stock's own working allocations happen in OUR process
 * during NvIspOpen (ten CREATE/ALLOC/MMAP triples, rc=0, seen in
 * strace) -- but we do not know the arguments. This library catches
 * them: it exports NvRmMemHandleAllocAttr, prints everything, and
 * forwards the call to the real libnvrm.so unchanged.
 *
 * Usage:
 *   LD_PRELOAD=/data/local/tmp/nvrmlog.so ispinit 4194303 rggb
 *
 * Discipline: log-and-forward, nothing substituted. The only behavior
 * change is the degenerate one -- if the real function cannot be
 * resolved at all we return 0xb without calling, which is announced in
 * the log; there is no way to "proceed" there anyway.
 *
 * Note on coverage: calls made through the dynamic linker's symbol
 * resolution (libnvisp_v3 -> libnvrm) go through this interceptor. A call
 * made through dlsym(libnvrm_handle, ...) -- as ispinit itself does --
 * resolves to the REAL symbol and bypasses us; ispinit prints its own
 * line for that call, so the comparison still lands in one log.
 *
 * Built like ispinit (crt0+libc, no -nostdlib games). This runs only in
 * our own test process; it never goes near mediaserver.
 */

#include <dlfcn.h>
#include <stdio.h>

/*
 * dlopen flags, bionic 4.4: RTLD_NOW | RTLD_LOCAL == 0.
 * (Do NOT copy NDK header constants; see shim/src/nvisp_shim.c.)
 */
#define NVRMLOG_DLOPEN_FLAGS 0

typedef int (*AllocAttr_fn)(unsigned a1, void *attrs, void **out);

static void *real_handle;

/*
 * Lazy resolver against the real libnvrm.so. Cached per function; the
 * library handle is opened once process-wide.
 */
static void *resolve_real(const char *name, void **cache)
{
    if (*cache == 0) {
        if (real_handle == 0)
            real_handle = dlopen("libnvrm.so", NVRMLOG_DLOPEN_FLAGS);
        if (real_handle != 0)
            *cache = dlsym(real_handle, name);
    }
    return *cache;
}

int NvRmMemHandleAllocAttr(unsigned a1, void *attrs, void **out)
{
    static AllocAttr_fn real_fn;
    int rc;

    real_fn = (AllocAttr_fn)resolve_real("NvRmMemHandleAllocAttr",
                                         (void **)&real_fn);

    printf("[nvrmlog] AllocAttr arg1=0x%x attrs=%p out=%p\n",
           a1, attrs, (void *)out);

    if (attrs != 0) {
        unsigned *a = (unsigned *)attrs;
        int k;

        printf("[nvrmlog]   attrs:");
        for (k = 0; k < 8; k++)
            printf(" [%d]=0x%x", k, a[k]);
        printf("\n");

        if (a[0] != 0) {
            /* the tag list: the FIRST word decides -- outside 1..6 ends
               the loop immediately (tagless). Print eight words, mark
               the verdict. */
            unsigned *t = (unsigned *)a[0];
            printf("[nvrmlog]   tags@0x%x:", a[0]);
            for (k = 0; k < 8; k++)
                printf(" %08x", t[k]);
            printf("  (w0 %s 1..6 -> %s)\n",
                   (t[0] >= 1 && t[0] <= 6) ? "in" : "outside",
                   (t[0] >= 1 && t[0] <= 6) ? "tagged" : "tagless");
        } else {
            printf("[nvrmlog]   tags: attrs[0]==0\n");
        }
    }

    if (real_fn == 0) {
        printf("[nvrmlog]   real function UNAVAILABLE -- returning 0xb "
               "without calling (announced deviation)\n");
        return 0xb;
    }

    rc = real_fn(a1, attrs, out);
    printf("[nvrmlog]   -> rc=0x%x memh=%p\n", (unsigned)rc,
           out != 0 ? *out : 0);
    return rc;
}

/*
 * NvRmMemRead: libnvisp_v3 imports it, so live calls occur during normal
 * ISP operation. Arity unestablished -- four words printed and forwarded
 * as-is (ARM AAPCS ignores extra registers harmlessly; we print the
 * first four, the shape will be visible from the values).
 */
int NvRmMemRead(unsigned a1, unsigned a2, unsigned a3, unsigned a4)
{
    static int (*real_fn)(unsigned, unsigned, unsigned, unsigned);
    int rc;

    real_fn = (int (*)(unsigned, unsigned, unsigned, unsigned))
        resolve_real("NvRmMemRead", (void **)&real_fn);

    printf("[nvrmlog] Read a1=0x%x a2=0x%x a3=0x%x a4=0x%x\n",
           a1, a2, a3, a4);

    if (real_fn == 0) {
        printf("[nvrmlog]   real function UNAVAILABLE -- returning 0xb\n");
        return 0xb;
    }
    rc = real_fn(a1, a2, a3, a4);
    printf("[nvrmlog]   -> rc=0x%x\n", (unsigned)rc);
    return rc;
}

/*
 * NvRmMemHandleFree: the paired teardown -- captured so we see WHEN the
 * stock frees surfaces and with which handle (our own teardown waits
 * for a clean submission first).
 */
int NvRmMemHandleFree(unsigned a1)
{
    static int (*real_fn)(unsigned);
    int rc;

    real_fn = (int (*)(unsigned))resolve_real("NvRmMemHandleFree",
                                              (void **)&real_fn);

    printf("[nvrmlog] HandleFree a1=0x%x\n", a1);

    if (real_fn == 0) {
        printf("[nvrmlog]   real function UNAVAILABLE -- returning 0xb\n");
        return 0xb;
    }
    rc = real_fn(a1);
    printf("[nvrmlog]   -> rc=0x%x\n", (unsigned)rc);
    return rc;
}
