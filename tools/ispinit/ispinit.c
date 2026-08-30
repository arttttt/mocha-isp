/*
 * ispinit -- raise the ISP by hand and give it back, from a shell.
 *
 * The first thing we do ourselves instead of watching. The sequence is
 * minimal on purpose (the contract from the lead, 2026-08-30):
 *
 *   NvRmOpen(&dev)         -- stub in libnvrm.so: writes 1 to *out, returns 0.
 *                             So dev is the VALUE 1, not an address; the live
 *                             hook log confirms the stock passes r0=1 into
 *                             NvIspOpen.
 *   NvIspOpen(dev, 1, &h)  -- instance 1 for the first instance (stock opens
 *                             1 then 2). On failure it cleans up after
 *                             itself; we free nothing by hand, NvIspClose
 *                             owns the teardown, including the host1x
 *                             channel.
 *   NvIspGetStatus(h, 6, &v, &size) -- size=4 in, actual size out. If the
 *                             value is non-zero right after Open, the clock
 *                             call is not needed on the first run.
 *   NvIspClose(h)          -- nothing released manually: a manual release
 *                             would be a double free.
 *
 * Every step prints its result. A silent failure is useless: we must see
 * at which call it stopped, not guess.
 *
 * Exit is a normal return from main. Nobody has established that the
 * kernel reclaims the channel behind a killed process, so we do not kill
 * the process and do not leave via a signal.
 *
 * Built like the shim (docs/build-abi.md): -nostdlib, PIE, no symbol
 * versioning, libdl.so is the only dependency. Output goes to stdout
 * through the write syscall, so run it over adb shell.
 */

/* libdl.so, resolved on the device */
void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
char *dlerror(void);

/*
 * dlopen flags, bionic values (Android 4.4): RTLD_NOW = 0, RTLD_LOCAL = 0.
 * See the long comment in shim/src/nvisp_shim.c; do NOT copy glibc values.
 */
#define ISPINIT_DLOPEN_FLAGS 0

static const char nvrm_path[] = "libnvrm.so";
static const char nvisp_path[] = "/system/vendor/lib/libnvisp_v3.real.so";

/* --- raw console output: the write syscall, no libc --- */

static long sys_write(int fd, const void *buf, unsigned len)
{
    register long r0 __asm__("r0") = fd;
    register long r1 __asm__("r1") = (long)buf;
    register long r2 __asm__("r2") = len;
    register long r7 __asm__("r7") = 4; /* __NR_write */
    __asm__ volatile("swi 0"
                     : "+r"(r0)
                     : "r"(r1), "r"(r2), "r"(r7)
                     : "memory");
    return r0;
}

static char out_buf[256];
static unsigned out_len;

static void out_flush(void)
{
    if (out_len != 0) {
        sys_write(1, out_buf, out_len);
        out_len = 0;
    }
}

static void out_ch(char c)
{
    if (out_len == sizeof(out_buf))
        out_flush();
    out_buf[out_len++] = c;
}

static void out_str(const char *s)
{
    while (*s)
        out_ch(*s++);
}

static void out_hex(unsigned v)
{
    static const char dig[] = "0123456789abcdef";
    int i;

    out_str("0x");
    for (i = 28; i >= 0; i -= 4)
        out_ch(dig[(v >> i) & 0xf]);
}

static void out_dec(unsigned v)
{
    char tmp[12];
    int n = 0;

    if (v == 0) {
        out_ch('0');
        return;
    }
    while (v != 0) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n != 0)
        out_ch(tmp[--n]);
}

/* --- the four calls --- */

typedef int (*NvRmOpen_fn)(void **out);
typedef int (*NvIspOpen_fn)(unsigned devHandle, unsigned instance,
                            unsigned *hIsp);
typedef int (*NvIspGetStatus_fn)(unsigned hIsp, unsigned statusId,
                                 void *value, unsigned *size);
typedef int (*NvIspClose_fn)(unsigned hIsp);

int main(void)
{
    void *nvrm;
    void *nvisp;
    NvRmOpen_fn nvRmOpen;
    NvIspOpen_fn nvIspOpen;
    NvIspGetStatus_fn nvIspGetStatus;
    NvIspClose_fn nvIspClose;
    void *dev = 0;
    unsigned hIsp = 0;
    unsigned value = 0;
    unsigned size = 4;
    int rc;
    const char *err;

    /* [1] libnvrm -- gives us NvRmOpen, the only way to a device handle */
    out_str("[1] dlopen(\"");
    out_str(nvrm_path);
    out_str("\") -> ");
    nvrm = dlopen(nvrm_path, ISPINIT_DLOPEN_FLAGS);
    out_hex((unsigned)nvrm);
    out_ch('\n');
    if (nvrm == 0) {
        err = dlerror();
        out_str("    dlerror: ");
        out_str(err != 0 ? err : "(null)");
        out_ch('\n');
        out_flush();
        return 1;
    }

    /* [2] the stock ISP library, by its explicit deployed path
       (the shim occupies libnvisp_v3.so; the stock one is .real) */
    out_str("[2] dlopen(\"");
    out_str(nvisp_path);
    out_str("\") -> ");
    nvisp = dlopen(nvisp_path, ISPINIT_DLOPEN_FLAGS);
    out_hex((unsigned)nvisp);
    out_ch('\n');
    if (nvisp == 0) {
        err = dlerror();
        out_str("    dlerror: ");
        out_str(err != 0 ? err : "(null)");
        out_ch('\n');
        out_flush();
        return 1;
    }

    /* [3] the symbols; a miss here is printed and fatal */
    nvRmOpen = (NvRmOpen_fn)dlsym(nvrm, "NvRmOpen");
    out_str("[3] dlsym NvRmOpen -> ");
    out_hex((unsigned)nvRmOpen);
    out_ch('\n');

    nvIspOpen = (NvIspOpen_fn)dlsym(nvisp, "NvIspOpen");
    out_str("    dlsym NvIspOpen -> ");
    out_hex((unsigned)nvIspOpen);
    out_ch('\n');

    nvIspGetStatus = (NvIspGetStatus_fn)dlsym(nvisp, "NvIspGetStatus");
    out_str("    dlsym NvIspGetStatus -> ");
    out_hex((unsigned)nvIspGetStatus);
    out_ch('\n');

    nvIspClose = (NvIspClose_fn)dlsym(nvisp, "NvIspClose");
    out_str("    dlsym NvIspClose -> ");
    out_hex((unsigned)nvIspClose);
    out_ch('\n');

    if (nvRmOpen == 0 || nvIspOpen == 0 || nvIspGetStatus == 0 ||
        nvIspClose == 0) {
        err = dlerror();
        out_str("    dlerror: ");
        out_str(err != 0 ? err : "(null)");
        out_ch('\n');
        out_flush();
        return 1;
    }

    /* [4] device handle: the VALUE lands in dev (the stub writes 1) */
    rc = nvRmOpen(&dev);
    out_str("[4] NvRmOpen(&dev) -> rc=");
    out_dec((unsigned)rc);
    out_str(" dev=");
    out_hex((unsigned)dev);
    out_ch('\n');
    if (rc != 0) {
        out_flush();
        return 1;
    }

    /* [5] open instance 1. On failure the library cleans up after
       itself; we release nothing here, that would be a double free. */
    rc = nvIspOpen((unsigned)dev, 1, &hIsp);
    out_str("[5] NvIspOpen(dev=");
    out_hex((unsigned)dev);
    out_str(", instance=1, &hIsp) -> rc=");
    out_hex((unsigned)rc);
    out_str(" hIsp=");
    out_hex(hIsp);
    out_ch('\n');
    if (rc != 0) {
        out_flush();
        return 1;
    }

    /* [6] status id 6, 4 bytes in, actual size out */
    size = 4;
    rc = nvIspGetStatus(hIsp, 6, &value, &size);
    out_str("[6] NvIspGetStatus(hIsp=");
    out_hex(hIsp);
    out_str(", id=6, &value, size=4) -> rc=");
    out_hex((unsigned)rc);
    out_str(" size=");
    out_dec(size);
    out_str(" value=");
    out_hex(value);
    out_ch('\n');

    /* [7] hand it back, completely: NvIspClose releases everything
       itself, including the host1x channel. */
    rc = nvIspClose(hIsp);
    out_str("[7] NvIspClose(hIsp=");
    out_hex(hIsp);
    out_str(") -> rc=");
    out_hex((unsigned)rc);
    out_ch('\n');

    out_str("done\n");
    out_flush();
    return 0;
}

/*
 * Entry point. -nostdlib means no crt0: we arrive here straight from the
 * dynamic linker, call main, and exit through the raw syscall so the
 * process terminates normally (a signal exit or a kill is untested ground
 * for channel cleanup). argc/argv stay untouched on the stack.
 */
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 4\n"
    ".globl _start\n"
    ".type _start, %function\n"
    ".thumb_func\n"
    "_start:\n"
    "  bl   main\n"
    "  mov  r7, #1\n" /* __NR_exit */
    "  swi  0\n"
    "  b    .\n");
