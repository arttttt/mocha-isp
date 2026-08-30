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
 *   NvIspSetIspClockRate(h, 0x003fffff) -- nonzero on purpose. impl-2: on a
 *                             fresh context the rate cache is zero, so
 *                             GetStatus(6) alone would read that same zero
 *                             and we could not tell a raised ISP from an
 *                             unraised one. The stock itself calls this with
 *                             r1=0, which is a no-op by cache equality.
 *   NvIspGetStatus(h, 6, &v, &size) -- size=4 in, actual size out; after the
 *                             clock call it must print the written rate,
 *                             not zero.
 *   NvIspClose(h)          -- nothing released manually: a manual release
 *                             would be a double free.
 *
 * Every step prints its result BEFORE the next call that may not return;
 * stdout is unbuffered, so whatever ran reaches the shell even mid-crash.
 *
 * Exit is a normal return from main. Nobody has established that the
 * kernel reclaims the channel behind a killed process, so we do not kill
 * the process and do not leave via a signal.
 *
 * Built WITH crt0 and libc (build-ispinit.sh), not -nostdlib: the -nostdlib
 * build died inside the linker on the very first dlopen -- loading a
 * library whose dependencies include libc runs constructors against a
 * process whose libc was never initialised. This is a separate process, so
 * the mediaserver rule (no symbol versioning, ever) does not extend here:
 * if it does not load, it does not load, nothing breaks.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bayer_gen.h"

/*
 * dlopen flags are the bionic 4.4 values, NOT the NDK header values: on
 * this device RTLD_NOW is 0 and RTLD_GLOBAL is 2, while the r21e header
 * constants encode modern bionic and would read as RTLD_GLOBAL here.
 * Verified on the device; see the long comment in shim/src/nvisp_shim.c.
 * We want NOW | LOCAL, which is 0.
 */
#define ISPINIT_DLOPEN_FLAGS 0

static const char nvrm_path[] = "libnvrm.so";
static const char nvisp_path[] = "/system/vendor/lib/libnvisp_v3.real.so";

/*
 * The stock's configuration words, verbatim from the live dump
 * (out/hook-config.txt, identical for both instances):
 *   mode 1, size 64: sixteen words of format selectors;
 *   mode 2, size 4:  the single word 2.
 * Cross-checked twice: the hook dump and the lead's mail agree word for
 * word. A typo here is a wrong format with no visible cause.
 */
static const unsigned cfg_mode1[16] = {
    0x00000001, 0x00000007, 0x00000009, 0x0000000a,
    0x00000003, 0x00000000, 0x00000006, 0x00000008,
    0x00000011, 0x0000000f, 0x0000000c, 0x0000000e,
    0x0000000b, 0x00000000, 0x00000010, 0x0000000d,
};
static const unsigned cfg_mode2 = 2;


/* --- the five calls --- */

typedef int (*NvRmOpen_fn)(void **out);
typedef int (*NvRmMemHandleCreate_fn)(unsigned param0, void **out,
                                      unsigned size);
typedef int (*NvRmMemHandleAllocAttr_fn)(unsigned size, void *attrs,
                                         void **out);
typedef int (*NvRmMemWriteStrided_fn)(unsigned handle, void *bufPtr,
                                      unsigned bufStride, unsigned offset,
                                      unsigned memStride, unsigned widthBytes,
                                      unsigned numRows, unsigned x8);
typedef int (*NvRmMemReadStrided_fn)(unsigned handle, void *bufPtr,
                                     unsigned bufStride, unsigned offset,
                                     unsigned memStride, unsigned widthBytes,
                                     unsigned numRows, unsigned x8);
typedef int (*NvRmMemRW_fn)(unsigned handle, void *ptr, unsigned a3,
                            unsigned a4);
typedef int (*NvIspOpen_fn)(unsigned devHandle, unsigned instance,
                            unsigned *hIsp);
typedef int (*NvIspSetConfiguration_fn)(unsigned hIsp, unsigned mode,
                                        const void *buf, unsigned *size);
typedef int (*NvIspSetIspClockRate_fn)(unsigned hIsp, unsigned rate);
typedef int (*NvIspGetStatus_fn)(unsigned hIsp, unsigned statusId,
                                 void *value, unsigned *size);
typedef int (*NvIspClose_fn)(unsigned hIsp);

/*
 * ProcessFrame: 4 register args + 9 stack words. The hook's live stack
 * (caller-sp coordinates) shows the five pointers at +0x10..+0x20 and
 * four words the library never reads at +0x00..+0x0c -- in C terms the
 * stack argument area starts at +0x00, so the descriptors are
 * parameters 9..13 and the unread words are parameters 5..8, mirrored
 * as zeros here (the stock's 3280/2460 there were session sensor
 * geometry; we encode nothing of the session).
 */
typedef int (*NvIspProcessFrame_fn)(unsigned hIsp, unsigned mode,
                                    unsigned r2, unsigned r3,
                                    unsigned s5, unsigned s6,
                                    unsigned s7, unsigned s8,
                                    unsigned inDesc, unsigned aux1,
                                    unsigned aux2, unsigned aux3,
                                    unsigned aux4);


/*
 * Stage-table interception. The ISP handler table lives in the context;
 * entries are function pointers (thumb bit set). stage=<off>:on swaps
 * an entry for one of our hooks; the hook prints entry args (r0-r3
 * only -- arity unestablished), calls the original, prints its return
 * code, and returns it unchanged. Nothing else is touched; the stack
 * the library sees is exactly the stack it built. Fired/never-called
 * prints at the end.
 */
#define STAGE_SLOTS 8
extern void stage_hook_0(void);
extern void stage_hook_1(void);
extern void stage_hook_2(void);
extern void stage_hook_3(void);
extern void stage_hook_4(void);
extern void stage_hook_5(void);
extern void stage_hook_6(void);
extern void stage_hook_7(void);
static const void *stage_hook_syms[STAGE_SLOTS] = {
    stage_hook_0, stage_hook_1, stage_hook_2, stage_hook_3,
    stage_hook_4, stage_hook_5, stage_hook_6, stage_hook_7
};
static unsigned stage_hook_orig[STAGE_SLOTS];   /* table value we replaced */
static unsigned stage_hook_fired[STAGE_SLOTS];
static unsigned stage_cur;                      /* slot currently entered */
static unsigned stage_pending_rc;

/*
 * Called from asm with (slot, &saved[r0..r3,r12,lr]). Prints the entry
 * args and returns the ORIGINAL function address; the asm tail-jumps
 * to it with lr = the shared continuation.
 */
__attribute__((used, noinline))
static void *stage_pre(unsigned slot, unsigned *saved)
{
    printf("[stage %u] enter r0=0x%x r1=0x%x r2=0x%x r3=0x%x\n",
           slot, saved[0], saved[1], saved[2], saved[3]);
    return (void *)stage_hook_orig[slot];
}

/* the shared continuation: the original returned here with its code
   in r0; print it and go back to the library */
__attribute__((used, noinline, naked))
static void stage_cont(void)
{
    __asm__(
        ".thumb\n"
        ".thumb_func\n"
        "stage_cont:\n"
        "  push {r0, lr}\n"
        "  bl   stage_post\n"
        "  ldr  r0, [sp]\n"
        "  add  sp, #8\n"
        "  ldr.w pc, [sp, #-4]\n");
}

__attribute__((used, noinline))
static void stage_post(unsigned rc)
{
    stage_hook_fired[stage_cur] = 1;
    stage_pending_rc = rc;
    printf("[stage %u] leave rc=0x%x\n", stage_cur, rc);
}

/*
 * First n 32-bit LE words of a byte buffer, tagged. Used for the three
 * provenance states of the submission experiment: output-after-alloc,
 * input-after-write, output-after-submit. Twice-taken emptiness or
 * twice-taken garbage must be DISTINGUISHABLE, not assumed.
 */
static void print_first_words(const char *tag, const unsigned char *b,
                              int n)
{
    int m;

    printf("%s:", tag);
    for (m = 0; m < n; m++)
        printf(" %08x",
               (unsigned)b[m * 4] | ((unsigned)b[m * 4 + 1] << 8) |
               ((unsigned)b[m * 4 + 2] << 16) |
               ((unsigned)b[m * 4 + 3] << 24));
    printf("\n");
}

/*
 * Post-call state: the words themselves plus WHICH words changed.
 * A library answer written into one word of one buffer is easy to miss
 * in sixteen lines -- the diff makes it unmissable.
 */
static void print_state_diff(const char *tag, const unsigned *before,
                             const unsigned *after, int n)
{
    int k, c = 0;

    printf("%s:", tag);
    for (k = 0; k < n; k++)
        printf(" +%02x:%08x", k * 4, after[k]);
    printf(" | changed:");
    for (k = 0; k < n; k++)
        if (before[k] != after[k]) {
            printf(" +%02x", k * 4);
            c++;
        }
    if (c == 0)
        printf(" none");
    printf("\n");
}

int main(int argc, char **argv)
{
    void *nvrm;
    void *nvisp;
    NvRmOpen_fn nvRmOpen;
    NvRmMemHandleCreate_fn nvRmMemHandleCreate;
    NvRmMemHandleAllocAttr_fn nvRmMemHandleAllocAttr;
    NvRmMemWriteStrided_fn nvRmMemWriteStrided;
    NvRmMemReadStrided_fn nvRmMemReadStrided;
    NvRmMemRW_fn nvRmMemWrite;
    NvRmMemRW_fn nvRmMemRead;
    NvIspOpen_fn nvIspOpen;
    NvIspSetConfiguration_fn nvIspSetConfiguration;
    NvIspProcessFrame_fn nvIspProcessFrame;
    NvIspSetIspClockRate_fn nvIspSetIspClockRate;
    NvIspGetStatus_fn nvIspGetStatus;
    NvIspClose_fn nvIspClose;
    void *dev = 0;
    unsigned hIsp = 0;
    unsigned value = 0;
    unsigned size = 4;
    unsigned rate = 0x003fffffu;
    unsigned order;
    unsigned a1_val;         /* AllocAttr's first argument override; the
                                 working value is the device VALUE (1 in
                                 our runs), used when unset */
    int a1_set = 0;
    /* round-trip control knobs, rt=<key>:<val> on the command line */
    int rt_on = 1;
    unsigned rt_w = 8, rt_h = 8;          /* pixels */
    unsigned rt_bs = 0x100, rt_ms = 0x100;/* host / nvmap row stride */
    unsigned rt_wb = 0, rt_nr = 0;        /* 0 = follow w*h and h */
    unsigned rt_off = 0;                  /* offset into the nvmap buffer */
    unsigned rt_x8 = 0;                   /* 0 = auto: memStride*numRows */
    /* simple (hops) is the DEFAULT: it is the verified-working path;
       the strided form stays available under rt=strided for signature
       re-checks. Its rc=0x100 rejection on every run was pure noise. */
    int rt_simple = 1;
    unsigned pf_mode = 1;   /* ProcessFrame's 2nd arg: mode=<val>; the
                               mode=2 run is the control we never ran */
    int slot14_aux = 0;     /* slot14=aux: put a plain aux buffer into
                               +0x14 instead of the output descriptor
                               (impl-2 once called it a Flush token --
                               never tested) */
    unsigned wait_ms = 0;                 /* wait=<ms>: delay before the
                                             post-submit output read */
    unsigned rt_size = 0;                 /* simple-mode byte count */
    char rt_order[5] = "hops";            /* simple-mode argument order:
        NvRmMemWrite(handle, offset, ptr, size) -- found by enumerating
        the six h-first permutations, hpos/hosp/hpso/hsop refused,
        hops PASSED at 64/512/2048 bytes byte-identical */
    unsigned attr_val[8];
    unsigned attr_set[8] = {0};
    /* seven stack slots: +00 +04 +08 +0c +18 +1c +20. The first four
       default OFF (zeros -- the historical behavior, their role is the
       open question), the last three default ON. */
    int aux_on[7] = {0, 0, 0, 0, 1, 1, 1};
    unsigned aux_val[7][44];
    unsigned char aux_set[7][44] = {{0}};
    unsigned char aux_isptr[7][44] = {{0}}; /* value was 'ptr' */
    unsigned cfg_val[16];                 /* cfg=<idx>:<val> overrides */
    unsigned char cfg_set[16] = {{0}};
    unsigned cfg2_val = cfg_mode2;        /* mode-2 word, stock value */
    int cfg2_set = 0;
    unsigned nvisp_base;
    unsigned din_val[44], dout_val[44];  /* descriptor word overrides */
    /* ctx=<off>:<count> -- ISP-context dumps, default none */
    unsigned ctx_off[4], ctx_cnt[4];
    int ctx_n = 0;
    unsigned ctxp_off[4], ctxp_cnt[4];
    int ctxp_n = 0;
    unsigned char din_set[44] = {{0}};
    unsigned char dout_set[44] = {{0}};
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);

    /*
     * Optional rate argument: "ispinit [rate]". Control experiment -- the
     * first run read 600000 (0x927c0) after we asked for the max, which
     * proves life only if a smaller request comes back as requested.
     * strtol, no frills; anything unparseable keeps the default.
     */
    if (argc > 1) {
        char *end = 0;
        long v = strtol(argv[1], &end, 0);
        if (end != argv[1] && *end == '\0' && v > 0)
            rate = (unsigned)v;
    }

    /* mosaic order, CLI-switchable like the rate: our sensor is SRGGB10
       per the kernel, but the ISP-visible order depends on readout
       mirroring, so we will be cycling through them. Default RGGB when
       the argument is absent; an explicit but unknown name is an error. */
    order = 0;
    if (argc > 2) {
        int k, found = 0;
        for (k = 0; k < 4; k++) {
            if (strcmp(argv[2], order_names[k]) == 0) {
                order = (unsigned)k;
                found = 1;
            }
        }
        if (!found) {
            printf("[0] bad order '%s', use rggb|bggr|grbg|gbrg\n", argv[2]);
            return 1;
        }
    }

    /* [attr=]idx:val allocation-attribute overrides and the buffer SIZE,
       several overrides allowed (hex or decimal). A token WITH ':' is an
       attribute override (idx 0..7; attrs[0] is a pointer -- overridable
       but almost certainly wrong); a token WITHOUT ':' is the buffer
       size. Unspecified values stay per the recipe. */
    for (int ai = 3; ai < argc; ai++) {
        char *tok = argv[ai];
        char *colon;
        char *e1 = 0, *e2 = 0;
        long idx, val;

        if (strncmp(tok, "rt=", 3) == 0) {
            /* rt=on|off, or rt=<key>:<val> with keys
               w,h (pixels), bs (host row stride), ms (nvmap row
               stride), wb (widthBytes), nr (numRows), off (nvmap
               offset), x8 (unknown 8th arg; 0 = auto ms*nr) */
            char *rest = tok + 3;
            char *colon;
            char *e3 = 0;
            long v;
            unsigned *dst = 0;
            if (strcmp(rest, "on") == 0) { rt_on = 1; continue; }
            if (strcmp(rest, "off") == 0) { rt_on = 0; continue; }
            if (strcmp(rest, "simple") == 0) { rt_simple = 1; continue; }
            if (strcmp(rest, "strided") == 0) { rt_simple = 0; continue; }
            if (strncmp(rest, "order:", 6) == 0) {
                /* simple-mode argument order, a permutation of hpos:
                   h=handle p=ptr o=offset s=size. Lives BEFORE the
                   generic numeric path: the value is a word, not a
                   number, and the colon must survive for the match. */
                const char *o = rest + 6;
                if (strlen(o) != 4 || strspn(o, "hpos") != 4 ||
                    o[0] == o[1] || o[0] == o[2] || o[0] == o[3] ||
                    o[1] == o[2] || o[1] == o[3] || o[2] == o[3]) {
                    printf("[0] bad rt order '%s', permutation of hpos\n",
                           o);
                    return 1;
                }
                memcpy(rt_order, o, 5);
                continue;
            }
            colon = strchr(rest, ':');
            if (colon == 0) {
                printf("[0] bad rt '%s', use rt=on|off or rt=<key>:<val>\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            v = strtol(colon + 1, &e3, 0);
            if (e3 == colon + 1 || *e3 != '\0' || v < 0) {
                printf("[0] bad rt value in '%s'\n", argv[ai]);
                return 1;
            }
            if (strcmp(rest, "size") == 0)
                dst = &rt_size;
            else if (strcmp(rest, "w") == 0) dst = &rt_w;
            else if (strcmp(rest, "h") == 0) dst = &rt_h;
            else if (strcmp(rest, "bs") == 0) dst = &rt_bs;
            else if (strcmp(rest, "ms") == 0) dst = &rt_ms;
            else if (strcmp(rest, "wb") == 0) dst = &rt_wb;
            else if (strcmp(rest, "nr") == 0) dst = &rt_nr;
            else if (strcmp(rest, "off") == 0) dst = &rt_off;
            else if (strcmp(rest, "x8") == 0) dst = &rt_x8;
            else {
                printf("[0] bad rt key '%s'\n", argv[ai]);
                return 1;
            }
            *dst = (unsigned)v;
            continue;
        }
        if (tok[0] == 'a' && tok[3] == '=' &&
            ((tok[1] >= '0' && tok[1] <= '9') ||
             (tok[1] >= 'a' && tok[1] <= 'f')) &&
            ((tok[2] >= '0' && tok[2] <= '9') ||
             (tok[2] >= 'a' && tok[2] <= 'f'))) {
            /* a<slot>=<idx>:<val|ptr> -- set one word of an aux buffer
               (slot two hex digits: 00 04 08 0c 18 1c 20); several
               allowed; idx decimal or hex, 0..43; 'ptr' puts the
               address of a shared zeroed buffer into the word (for the
               stock's pointer at +14 of the +1c structure) */
            static const unsigned aux_slots[7] = {
                0x00, 0x04, 0x08, 0x0c, 0x18, 0x1c, 0x20
            };
            char *colon;
            long idx, val, slot;
            int which = -1;
            for (int wi = 0; wi < 7; wi++)
                if (aux_slots[wi] ==
                    (unsigned)strtol(tok + 1, 0, 16))
                    which = wi;
            if (which < 0) {
                printf("[0] unknown aux slot in '%s'\n", argv[ai]);
                return 1;
            }
            slot = 0; /* reserved */
            (void)slot;
            colon = strchr(tok + 4, ':');
            if (colon == 0) {
                printf("[0] bad aux word '%s', use a<slot>=<idx>:<val|ptr>\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            idx = strtol(tok + 4, &e1, 0);
            if (e1 == tok + 4 || *e1 != '\0' || idx < 0 || idx > 43) {
                printf("[0] bad aux word index '%s'\n", argv[ai]);
                return 1;
            }
            if (strcmp(colon + 1, "ptr") == 0) {
                aux_isptr[which][idx] = 1;
                aux_set[which][idx] = 1;
            } else {
                val = strtol(colon + 1, &e2, 0);
                if (e2 == colon + 1 || *e2 != '\0') {
                    printf("[0] bad aux word value '%s'\n", argv[ai]);
                    return 1;
                }
                aux_val[which][idx] = (unsigned)val;
                aux_set[which][idx] = 1;
            }
            continue;
        }
        if (strncmp(tok, "mode=", 5) == 0) {
            char *e6 = 0;
            long v = strtol(tok + 5, &e6, 0);
            if (e6 == tok + 5 || *e6 != '\0' || v < 0) {
                printf("[0] bad mode '%s', use mode=<val>\n", argv[ai]);
                return 1;
            }
            pf_mode = (unsigned)v;
            continue;
        }
        if (strncmp(tok, "slot14=", 7) == 0) {
            if (strcmp(tok + 7, "aux") == 0)
                slot14_aux = 1;
            else if (strcmp(tok + 7, "desc") == 0)
                slot14_aux = 0;
            else {
                printf("[0] bad slot14 '%s', use slot14=desc|aux\n",
                       argv[ai]);
                return 1;
            }
            continue;
        }
        if (strncmp(tok, "ctx=", 4) == 0) {
            /* ctx=<off>:<count> -- dump <count> words of the ISP
               context from <off>; several regions allowed, up to four.
               The context is ours, valid between Open and Close,
               read-only. Default: no dump. */
            char *colon;
            char *e5 = 0;
            long off, cnt;
            colon = strchr(tok + 4, ':');
            if (colon == 0) {
                printf("[0] bad ctx '%s', use ctx=<off>:<count>\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            off = strtol(tok + 4, &e5, 0);
            cnt = strtol(colon + 1, &e5, 0);
            if (e5 == colon + 1 || *e5 != '\0' || off < 0 || cnt <= 0 ||
                cnt > 256) {
                printf("[0] bad ctx region in '%s' (count 1..256)\n",
                       argv[ai]);
                return 1;
            }
            if (ctx_n == 4) {
                printf("[0] too many ctx regions (max 4)\n");
                return 1;
            }
            ctx_off[ctx_n] = (unsigned)off;
            ctx_cnt[ctx_n] = (unsigned)cnt;
            ctx_n++;
            continue;
        }
        if (strncmp(tok, "ctxp=", 5) == 0) {
            /* ctxp=<off>:<count> -- take the WORD at ctx+<off>, treat
               it as an address, print <count> words from there. One
               level of dereference only; a null pointer prints as null
               instead of crashing. The pointer comes from our own
               context, valid between Open and Close. */
            char *colon;
            char *e7 = 0;
            long off, cnt;
            colon = strchr(tok + 5, ':');
            if (colon == 0) {
                printf("[0] bad ctxp '%s', use ctxp=<off>:<count>\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            off = strtol(tok + 5, &e7, 0);
            cnt = strtol(colon + 1, &e7, 0);
            if (e7 == colon + 1 || *e7 != '\0' || off < 0 || cnt <= 0 ||
                cnt > 256) {
                printf("[0] bad ctxp region in '%s' (count 1..256)\n",
                       argv[ai]);
                return 1;
            }
            if (ctxp_n == 4) {
                printf("[0] too many ctxp regions (max 4)\n");
                return 1;
            }
            ctxp_off[ctxp_n] = (unsigned)off;
            ctxp_cnt[ctxp_n] = (unsigned)cnt;
            ctxp_n++;
            continue;
        }
        if (strncmp(tok, "wait=", 5) == 0) {
            /* wait=<ms> -- delay before the post-submit output read.
               A crutch for the async hypothesis: if the output content
               changes with delay and not without, processing is
               asynchronous and the real answer is a fence wait. */
            char *e4 = 0;
            long v = strtol(tok + 5, &e4, 0);
            if (e4 == tok + 5 || *e4 != '\0' || v < 0) {
                printf("[0] bad wait '%s', use wait=<milliseconds>\n",
                       argv[ai]);
                return 1;
            }
            wait_ms = (unsigned)v;
            continue;
        }
        if (strncmp(tok, "cfg2=", 5) == 0) {
            /* cfg2=<val> -- the single mode-2 configuration word
               (stock sends 2) */
            char *e5 = 0;
            long v = strtol(tok + 5, &e5, 0);
            if (e5 == tok + 5 || *e5 != '\0' || v < 0) {
                printf("[0] bad cfg2 '%s', use cfg2=<val>\n", argv[ai]);
                return 1;
            }
            cfg2_val = (unsigned)v;
            cfg2_set = 1;
            continue;
        }
        if (strncmp(tok, "cfg=", 4) == 0) {
            /* cfg=<idx>:<val> -- override one of the sixteen mode-1
               configuration words; defaults are the stock words
               (1,7,9,a,3,0,6,8,11,f,c,e,b,0,10,d). impl-2 read word 0
               from {0,1,2}: possibly the frame SOURCE for the session
               -- the stock only walks the sensor path, and our
               memory-mode submission may need a different value. */
            char *colon;
            long idx, val;
            colon = strchr(tok + 4, ':');
            if (colon == 0) {
                printf("[0] bad cfg '%s', use cfg=<idx>:<val>\n", argv[ai]);
                return 1;
            }
            *colon = '\0';
            idx = strtol(tok + 4, &e1, 0);
            if (e1 == tok + 4 || *e1 != '\0' || idx < 0 || idx > 15) {
                printf("[0] bad cfg index in '%s'\n", argv[ai]);
                return 1;
            }
            val = strtol(colon + 1, &e2, 0);
            if (e2 == colon + 1 || *e2 != '\0' || val < 0) {
                printf("[0] bad cfg value in '%s'\n", argv[ai]);
                return 1;
            }
            cfg_val[idx] = (unsigned)val;
            cfg_set[idx] = 1;
            continue;
        }
        if (strncmp(tok, "din=", 4) == 0 || strncmp(tok, "dout=", 5) == 0) {
            /* din=<idx>:<val>, dout=... -- descriptor word overrides;
               several allowed, idx 0..43. Word 5 (the memory handle) is
               PROGRAM-SET and rejected here: an override there is not a
               configurable, it is a broken run. */
            int is_out = (tok[1] == 'o');
            char *colon;
            long idx, val;
            colon = strchr(tok + (is_out ? 5 : 4), ':');
            if (colon == 0) {
                printf("[0] bad descriptor override '%s', use "
                       "din=<idx>:<val>\n", argv[ai]);
                return 1;
            }
            *colon = '\0';
            idx = strtol(tok + (is_out ? 5 : 4), &e1, 0);
            if (e1 == tok + (is_out ? 5 : 4) || *e1 != '\0' ||
                idx < 0 || idx > 43) {
                printf("[0] bad descriptor index in '%s'\n", argv[ai]);
                return 1;
            }
            if (idx == 5) {
                printf("[0] %s=5 rejected: the memory handle is "
                       "program-set\n", is_out ? "dout" : "din");
                return 1;
            }
            val = strtol(colon + 1, &e2, 0);
            if (e2 == colon + 1 || *e2 != '\0' || val < 0) {
                printf("[0] bad descriptor value in '%s'\n", argv[ai]);
                return 1;
            }
            if (is_out) {
                dout_val[idx] = (unsigned)val;
                dout_set[idx] = 1;
            } else {
                din_val[idx] = (unsigned)val;
                din_set[idx] = 1;
            }
            continue;
        }
        if (strncmp(tok, "aux=", 4) == 0) {
            /* aux=<hexslot>:on|off -- fill one of the three remaining
               stack slots (+18, +1c, +20) with a zeroed buffer, or pass
               a deliberate NULL; default is all on */
            char *colon = strchr(tok + 4, ':');
            long slot;
            int *flag;
            static const unsigned aux_slots[7] = {
                0x00, 0x04, 0x08, 0x0c, 0x18, 0x1c, 0x20
            };
            int wi, hit = -1;
            if (colon == 0) {
                printf("[0] bad aux '%s', use aux=<hexslot>:on|off\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            slot = strtol(tok + 4, &e1, 16); /* slots are hex offsets
                                                ("0x" prefix ok too) */
            if (e1 == tok + 4 || *e1 != '\0') {
                printf("[0] bad aux slot '%s'\n", argv[ai]);
                return 1;
            }
            for (wi = 0; wi < 7; wi++)
                if (aux_slots[wi] == (unsigned)slot)
                    hit = wi;
            if (hit < 0) {
                printf("[0] unknown aux slot 0x%lx, use "
                       "00|04|08|0c|18|1c|20\n", slot);
                return 1;
            }
            flag = &aux_on[hit];
            if (strcmp(colon + 1, "on") == 0)
                *flag = 1;
            else if (strcmp(colon + 1, "off") == 0)
                *flag = 0;
            else {
                printf("[0] bad aux state '%s', use on|off\n", colon + 1);
                return 1;
            }
            continue;
        }
        if (strncmp(tok, "attr=", 5) == 0)
            tok += 5;
        colon = strchr(tok, ':');
        if (colon == 0) {
            /* no colon: override of AllocAttr's first argument */
            val = strtol(tok, &e2, 0);
            if (e2 == tok || *e2 != '\0' || val <= 0) {
                printf("[0] bad value '%s', use a positive number\n",
                       argv[ai]);
                return 1;
            }
            a1_val = (unsigned)val;
            a1_set = 1;
            continue;
        }
        *colon = '\0';
        idx = strtol(tok, &e1, 0);
        val = strtol(colon + 1, &e2, 0);
        if (e1 == tok || *e1 != '\0' || e2 == colon + 1 || *e2 != '\0' ||
            idx < 0 || idx > 7 || val < 0 || val > 0xffffffffl) {
            printf("[0] bad attr '%s', use [attr=]idx:val, idx 0..7\n",
                   argv[ai]);
            return 1;
        }
        attr_set[idx] = 1;
        attr_val[idx] = (unsigned)val;
    }
    printf("[0] requested rate = 0x%x, order = %s\n", rate,
           order_names[order]);

    /*
     * [gen] Generator self-test on a HOST staging buffer: FIRST, before
     * anything that can crash -- it needs nothing but malloc, and its
     * whole point is to be visible when everything else dies. 8x8
     * stride 256 bpp 2 (the dummy geometry, deliberate): for RGGB the
     * first four u32 words must be
     *   00220011 00220011 01220111 01220111
     * (row 0 spans four WORDS because stride is 256, not 16: pixels 4-7
     * sit in quadrant 1, +0x100) and the checksum must be 0x6AA0 --
     * permutation invariant, the same for all four orders. The numbers
     * are asserted on the host by bayer_test.sh.
     */
    {
        unsigned char *stage = malloc(4096);
        if (stage == 0) {
            printf("[gen] malloc failed -- generator untested\n");
        } else {
            unsigned sum = bayer_fill(stage, 8, 8, 256, 2, order);
            unsigned w[4];
            int k;
            for (k = 0; k < 4; k++)
                w[k] = (unsigned)stage[k * 4] |
                       ((unsigned)stage[k * 4 + 1] << 8) |
                       ((unsigned)stage[k * 4 + 2] << 16) |
                       ((unsigned)stage[k * 4 + 3] << 24);
            printf("[gen] bayer_fill(8x8, stride=256, bpp=2, order=%s) "
                   "checksum=0x%x first=0x%08x 0x%08x 0x%08x 0x%08x\n",
                   order_names[order], sum, w[0], w[1], w[2], w[3]);
            free(stage);
        }
    }

    /* [1] libnvrm -- gives us NvRmOpen, the only way to a device handle */
    printf("[1] dlopen(\"%s\") -> ", nvrm_path);
    nvrm = dlopen(nvrm_path, ISPINIT_DLOPEN_FLAGS);
    printf("%p\n", nvrm);
    if (nvrm == 0) {
        printf("    dlerror: %s\n", dlerror());
        return 1;
    }

    /* [2] the stock ISP library, by its explicit deployed path
       (the shim occupies libnvisp_v3.so; the stock one is .real) */
    printf("[2] dlopen(\"%s\") -> ", nvisp_path);
    nvisp = dlopen(nvisp_path, ISPINIT_DLOPEN_FLAGS);
    printf("%p\n", nvisp);
    if (nvisp == 0) {
        printf("    dlerror: %s\n", dlerror());
        return 1;
    }

    /* [3] the symbols; a miss here is printed and fatal */
    nvRmOpen = (NvRmOpen_fn)dlsym(nvrm, "NvRmOpen");
    nvRmMemHandleCreate =
        (NvRmMemHandleCreate_fn)dlsym(nvrm, "NvRmMemHandleCreate");
    nvRmMemHandleAllocAttr =
        (NvRmMemHandleAllocAttr_fn)dlsym(nvrm, "NvRmMemHandleAllocAttr");
    nvRmMemWriteStrided =
        (NvRmMemWriteStrided_fn)dlsym(nvrm, "NvRmMemWriteStrided");
    nvRmMemReadStrided =
        (NvRmMemReadStrided_fn)dlsym(nvrm, "NvRmMemReadStrided");
    nvRmMemWrite = (NvRmMemRW_fn)dlsym(nvrm, "NvRmMemWrite");
    nvRmMemRead = (NvRmMemRW_fn)dlsym(nvrm, "NvRmMemRead");
    nvIspOpen = (NvIspOpen_fn)dlsym(nvisp, "NvIspOpen");
    nvIspSetConfiguration =
        (NvIspSetConfiguration_fn)dlsym(nvisp, "NvIspSetConfiguration");
    nvIspProcessFrame =
        (NvIspProcessFrame_fn)dlsym(nvisp, "NvIspProcessFrame");
    nvIspSetIspClockRate =
        (NvIspSetIspClockRate_fn)dlsym(nvisp, "NvIspSetIspClockRate");
    nvIspGetStatus = (NvIspGetStatus_fn)dlsym(nvisp, "NvIspGetStatus");
    nvIspClose = (NvIspClose_fn)dlsym(nvisp, "NvIspClose");

    printf("[3] dlsym: NvRmOpen=%p HandleCreate=%p AllocAttr=%p NvIspOpen=%p "
           "SetConfiguration=%p ProcessFrame=%p SetIspClockRate=%p "
           "GetStatus=%p Close=%p\n",
           nvRmOpen, nvRmMemHandleCreate, nvRmMemHandleAllocAttr, nvIspOpen,
           nvIspSetConfiguration,
           nvIspProcessFrame, nvIspSetIspClockRate, nvIspGetStatus,
           nvIspClose);
    /*
     * Base of libnvisp_v3: dlsym address minus the symbol's static
     * offset (NvIspProcessFrame st_value = 0x1784 + thumb bit, per the
     * snapshot). Table pointers read from the context translate to
     * file offsets by subtracting this -- impl-2 gets exact addresses
     * instead of computed ones.
     */
    nvisp_base = ((unsigned)nvIspProcessFrame & ~1u) - 0x1784u;
    printf("[3] libnvisp_v3 base = 0x%x\n", nvisp_base);

    if (nvRmOpen == 0 || nvRmMemHandleCreate == 0 ||
        nvRmMemHandleAllocAttr == 0 || nvIspOpen == 0 ||
        nvIspSetConfiguration == 0 || nvIspProcessFrame == 0 ||
        nvIspSetIspClockRate == 0 || nvIspGetStatus == 0 ||
        nvIspClose == 0) {
        printf("    dlerror: %s\n", dlerror());
        return 1;
    }

    /* [4] device handle: the VALUE lands in dev (the stub writes 1) */
    printf("[4] NvRmOpen(&dev) -> ");
    rc = nvRmOpen(&dev);
    printf("rc=%d dev=%p\n", rc, dev);
    if (rc != 0)
        return 1;

    /*
     * [5] ROUND TRIP: allocate, write a known pattern, read it back,
     * compare. Touches NO ISP state -- this validates the memory path
     * alone and settles empirically whether an explicit mapping is
     * needed for direct Write/Read (impl-2 says the stock maps; if the
     * plain Strided calls agree without one, we do not need it).
     *
     * Expected and received words both print, so a mismatch shows its
     * SHAPE -- a shift, a byte order, a stride -- instead of a bare
     * verdict. Everything is CLI-tunable: rt=w:8 rt=h:8 rt=bs:0x100
     * rt=ms:0x100 rt=wb:16 rt=nr:8 rt=off:0 rt=x8:... (x8 unset in the
     * decode; wrappers pass the size there, so 0 means auto = ms*nr).
     */
    if (rt_on) {
        static NvRmMemHandleAllocAttr_fn alloc_fn;
        unsigned attrs_rt[8] = { 0, 0, 0x20, 2, 0x40000u, 0, 0, 0 };
        void *memh_rt = 0;
        unsigned char *stage = 0;
        unsigned char *back = 0;
        unsigned wb = rt_wb ? rt_wb : rt_w * 2;
        unsigned nr = rt_nr ? rt_nr : rt_h;
        unsigned x8 = rt_x8 ? rt_x8 : rt_ms * nr;
        unsigned region = rt_ms * nr;
        unsigned bad = 0, first_bad = 0xffffffffu;
        unsigned cmp_region;
        int k;

        if (rt_wb == 0 && wb > rt_bs)
            wb = rt_bs; /* the pattern cannot write past its own row */
        for (k = 0; k < 8; k++)
            if (attr_set[k])
                attrs_rt[k] = attr_val[k];

        printf("[5] round trip (%s): w=%u h=%u bs=0x%x ms=0x%x wb=%u "
               "nr=%u off=0x%x x8=0x%x size=%u order=%s\n",
               rt_simple ? "simple" : "strided",
               rt_w, rt_h, rt_bs, rt_ms, wb, nr, rt_off, x8,
               rt_size ? rt_size : region, rt_order);

        if (nvRmMemWriteStrided == 0 || nvRmMemReadStrided == 0) {
            printf("[5] Strided symbols missing -- round trip skipped\n");
        } else {
            alloc_fn = nvRmMemHandleAllocAttr;
            printf("[5] NvRmMemHandleAllocAttr(dev=0x%x, attrs, &memh_rt) "
                   "-> ", (unsigned)dev);
            rc = alloc_fn((unsigned)dev, attrs_rt, &memh_rt);
            printf("rc=0x%x memh_rt=%p\n", (unsigned)rc, memh_rt);

            stage = malloc(region > rt_bs * nr ? region : rt_bs * nr + 16);
            back = malloc(region + 16);
            if (memh_rt == 0 || rc != 0 || stage == 0 || back == 0) {
                printf("[5] allocation failed -- round trip skipped\n");
            } else {
                unsigned sum = bayer_fill(stage, rt_w, rt_h, rt_bs, 2, order);
                printf("[5] pattern: bayer_fill(%s) checksum=0x%x\n",
                       order_names[order], sum);

                if (rt_simple) {
                    /* plain Write/Read, four arguments; the order of
                       ptr/offset/size is not certain, so it is a CLI
                       permutation: rt=order:hpos etc. */
                    unsigned aw[4], ar[4], m;
                    if (nvRmMemWrite == 0 || nvRmMemRead == 0) {
                        printf("[5] NvRmMemWrite/Read symbols missing -- "
                               "simple mode unavailable\n");
                        free(stage);
                        free(back);
                        goto round_trip_end;
                    }
                    for (m = 0; m < 4; m++) {
                        if (rt_order[m] == 'h') aw[m] = (unsigned)memh_rt;
                        else if (rt_order[m] == 'p') aw[m] = (unsigned)stage;
                        else if (rt_order[m] == 'o') aw[m] = rt_off;
                        else aw[m] = rt_size ? rt_size : region;
                    }
                    rc = nvRmMemWrite(aw[0], (void *)aw[1], aw[2], aw[3]);
                    printf("[5] NvRmMemWrite(%s) -> rc=0x%x\n", rt_order,
                           (unsigned)rc);

                    memset(back, 0xEE, region);
                    for (m = 0; m < 4; m++) {
                        if (rt_order[m] == 'h') ar[m] = (unsigned)memh_rt;
                        else if (rt_order[m] == 'p') ar[m] = (unsigned)back;
                        else if (rt_order[m] == 'o') ar[m] = rt_off;
                        else ar[m] = rt_size ? rt_size : region;
                    }
                    rc = nvRmMemRead(ar[0], (void *)ar[1], ar[2], ar[3]);
                    printf("[5] NvRmMemRead(%s) -> rc=0x%x\n", rt_order,
                           (unsigned)rc);
                } else {
                    rc = nvRmMemWriteStrided((unsigned)memh_rt, stage,
                                             rt_bs, rt_off, rt_ms, wb, nr,
                                             x8);
                    printf("[5] NvRmMemWriteStrided -> rc=0x%x\n",
                           (unsigned)rc);

                    memset(back, 0xEE, region);
                    rc = nvRmMemReadStrided((unsigned)memh_rt, back, rt_bs,
                                            rt_off, rt_ms, wb, nr, x8);
                    printf("[5] NvRmMemReadStrided -> rc=0x%x\n",
                           (unsigned)rc);
                }

                cmp_region = rt_simple && rt_size ? rt_size : region;
                for (k = 0; k < (int)cmp_region; k++) {
                    if (stage[k] != back[k]) {
                        bad++;
                        if (first_bad == 0xffffffffu)
                            first_bad = (unsigned)k;
                    }
                }

                {
                    unsigned ew[8], rw[8];
                    int m;
                    for (m = 0; m < 8; m++) {
                        int b = m * 4;
                        ew[m] = (unsigned)stage[b] |
                                ((unsigned)stage[b + 1] << 8) |
                                ((unsigned)stage[b + 2] << 16) |
                                ((unsigned)stage[b + 3] << 24);
                        rw[m] = (unsigned)back[b] |
                                ((unsigned)back[b + 1] << 8) |
                                ((unsigned)back[b + 2] << 16) |
                                ((unsigned)back[b + 3] << 24);
                    }
                    printf("[5] expected:");
                    for (m = 0; m < 8; m++)
                        printf(" %08x", ew[m]);
                    printf("\n");
                    printf("[5] received:");
                    for (m = 0; m < 8; m++)
                        printf(" %08x", rw[m]);
                    printf("\n");
                }

                if (bad == 0) {
                    printf("[5] ROUND TRIP PASSED: %u bytes identical\n",
                           cmp_region);
                } else {
                    printf("[5] ROUND TRIP FAILED: %u of %u bytes differ, "
                           "first at byte 0x%x (row %u, col %u)\n",
                           bad, cmp_region, first_bad,
                           first_bad / rt_ms, (first_bad % rt_ms) / 4);
                }
            }
        }
    }

round_trip_end:;
    /* [5] open instance 1. On failure the library cleans up after
       itself; we release nothing here, that would be a double free. */
    printf("[5] NvIspOpen(dev=%p, instance=1, &hIsp) -> ", dev);
    rc = nvIspOpen((unsigned)dev, 1, &hIsp);
    printf("rc=0x%x hIsp=0x%x\n", (unsigned)rc, hIsp);
    if (rc != 0)
        return 1;

    /* [6] configure, mode 1: the stock's sixteen selector words
       (CLI-overridable, cfg=), size 64 in. The size AFTER the call
       matters: rc 10 means the library disagreed and wrote the size it
       wants -- the fix-and-retry mechanism. We print what it said
       instead of guessing. */
    {
        unsigned cfgw[16];
        int k;
        for (k = 0; k < 16; k++)
            cfgw[k] = cfg_set[k] ? cfg_val[k] : cfg_mode1[k];
        printf("[6] cfg words:");
        for (k = 0; k < 16; k++)
            printf(" [%d]=0x%x%s", k, cfgw[k], cfg_set[k] ? "*" : "");
        printf("\n");
        size = 64;
        printf("[6] NvIspSetConfiguration(hIsp=0x%x, mode=1, cfg16, "
               "&size=64) -> ", hIsp);
        rc = nvIspSetConfiguration(hIsp, 1, cfgw, &size);
        printf("rc=0x%x size-after=%u\n", (unsigned)rc, size);
    }

    /* [7] configure, mode 2: the single word (stock 2, cfg2= to
       override), size 4 in */
    size = 4;
    printf("[7] NvIspSetConfiguration(hIsp=0x%x, mode=2, cfg2=0x%x%s, "
           "&size=4) -> ", hIsp, cfg2_val, cfg2_set ? "*" : "");
    rc = nvIspSetConfiguration(hIsp, 2, &cfg2_val, &size);
    printf("rc=0x%x size-after=%u\n", (unsigned)rc, size);

    /* [8] set the requested rate: the zero cache would answer the next
       read with zero and hide whether the ISP is alive at all; the max
       is clipped by the clock tree (600000 of 0x3fffff), and a smaller
       request must come back as requested -- that is the control */
    printf("[8] NvIspSetIspClockRate(hIsp=0x%x, rate=0x%x) -> ", hIsp, rate);
    rc = nvIspSetIspClockRate(hIsp, rate);
    printf("rc=0x%x\n", (unsigned)rc);

    /* [9] status id 6, 4 bytes in, actual size out; must be the rate
       just written, not zero */
    size = 4;
    printf("[9] NvIspGetStatus(hIsp=0x%x, id=6, &value, size=4) -> ", hIsp);
    rc = nvIspGetStatus(hIsp, 6, &value, &size);
    printf("rc=0x%x size=%u value=0x%x\n", (unsigned)rc, size, value);

    /* [9b] ISP-context dumps, only where requested (ctx=<off>:<count>).
       The context is ours, valid between Open and Close, read-only.
       Table pointers translate to file offsets via the base printed
       at [3]. */
    for (int ci = 0; ci < ctx_n; ci++) {
        unsigned *cp = (unsigned *)((unsigned)hIsp + ctx_off[ci]);
        printf("[9b] ctx+0x%x (%u words):", ctx_off[ci], ctx_cnt[ci]);
        for (unsigned k2 = 0; k2 < ctx_cnt[ci]; k2++) {
            if (k2 != 0 && k2 % 8 == 0)
                printf("\n[9b]   ");
            printf(" +%x:%08x", ctx_off[ci] + k2 * 4, cp[k2]);
        }
        printf("\n");
    }

    /* [9b] pointer-following dumps: one level deep, null-safe */
    for (int cj = 0; cj < ctxp_n; cj++) {
        unsigned ptr = *(unsigned *)((unsigned)hIsp + ctxp_off[cj]);
        printf("[9b] ctxp+0x%x -> 0x%x", ctxp_off[cj], ptr);
        if (ptr == 0) {
            printf(" (null)\n");
            continue;
        }
        {
            unsigned *cp = (unsigned *)ptr;
            printf(" (%u words):", ctxp_cnt[cj]);
            for (unsigned k2 = 0; k2 < ctxp_cnt[cj]; k2++) {
                if (k2 != 0 && k2 % 8 == 0)
                    printf("\n[9b]   ");
                printf(" +%x:%08x", k2 * 4, cp[k2]);
            }
            printf("\n");
        }
    }

    /*
     * [10..12] submit the placeholder frame. First reproduce what the
     * hardware already accepted (the stock's 8x8 dummy: format 0x105a500c
     * from the stock pool, stride 256, one plane), with OUR buffer and
     * OUR handle; geometry and format change one step at a time after
     * this works. The numbers 8x8 are copied deliberately as known-good,
     * the handle is ours from the allocation -- never copied, it lives
     * one run.
     */

    /*
     * [10] the WORKING ATTRIBUTES -- read from INTERCEPTION, not derived.
     * nvrmlog caught the stock's live calls in our own process (ten
     * CREATE/ALLOC/MMAP triples, rc=0, identical attrs every time):
     *
     *   arg1      = 0x1      the device VALUE. Three derived answers --
     *                        pointer, Create handle, bare size -- were
     *                        all wrong; this one reproduced rc=0 first try.
     *   attrs[0]  = 0        no tag list at all
     *   attrs[1]  = 0        not the tagged path
     *   attrs[2]  = 0x20     alignment
     *   attrs[3]  = 2        memory type (2, not 1)
     *   attrs[4]  = 0x40000  256 KiB -- the size DOES live in attrs[4]
     *
     * CLI overrides still land on top of these defaults; they exist for
     * exactly this kind of one-run experiment. (The two-step Create+Alloc
     * experiment lives in git history, commit be257b4.)
     */
    {
        unsigned attrs_base[8] = { 0, 0, 0x20, 2, 0x40000u, 0, 0, 0 };
        unsigned attrs_in[8];
        unsigned attrs_out[8];
        void *memh_in = 0;
        void *memh_out = 0;
        unsigned char *pat_out = 0;   /* the 0xDEADBEEF reference */
        unsigned desc_in[44] = {0};  /* 0xb0 bytes each, the stock
                                        record size */
        unsigned desc_out[44] = {0};
        unsigned gh, gw, gf, gt, gs, gp;
        unsigned bufs[7][44] = {{0}}; /* one buffer per stack slot */
        unsigned slot14_buf[44] = {0}; /* +0x14 in slot14=aux mode */
        /* pre-call snapshots for the post-call diff */
        unsigned snap_in[16], snap_out[16], snap14[16];
        unsigned snap_aux[7][16];
        unsigned ptr_target[44] = {0}; /* shared target for word=ptr */
        unsigned a1;
        int i, k;
        static const char aux_names[7][3] = {
            "00", "04", "08", "0c", "18", "1c", "20"
        };

        for (i = 0; i < 7; i++) {
            for (k = 0; k < 44; k++) {
                if (aux_isptr[i][k])
                    bufs[i][k] = (unsigned)ptr_target;
                else if (aux_set[i][k])
                    bufs[i][k] = aux_val[i][k];
            }
        }

        for (k = 0; k < 8; k++) {
            attrs_in[k] = attrs_base[k];
            attrs_out[k] = attrs_base[k];
        }
        for (k = 0; k < 8; k++) {
            if (attr_set[k]) {
                attrs_in[k] = attr_val[k];
                attrs_out[k] = attr_val[k];
            }
        }

        printf("[10] attrs in|out:");
        for (k = 0; k < 8; k++)
            printf(" [%d]=0x%x|0x%x%s", k, attrs_in[k], attrs_out[k],
                   attr_set[k] ? "*" : "");
        printf("\n");

        a1 = a1_set ? a1_val : (unsigned)dev;
        printf("[10] NvRmMemHandleAllocAttr(a1=0x%x, attrs, &memh_in) "
               "[input] -> ", a1);
        rc = nvRmMemHandleAllocAttr(a1, attrs_in, &memh_in);
        printf("rc=0x%x memh_in=%p\n", (unsigned)rc, memh_in);

        if (rc == 0 && memh_in != 0) {
            printf("[10] NvRmMemHandleAllocAttr(a1=0x%x, attrs, &memh_out) "
                   "[output] -> ", a1);
            rc = nvRmMemHandleAllocAttr(a1, attrs_out, &memh_out);
            printf("rc=0x%x memh_out=%p\n", (unsigned)rc, memh_out);
        }

        if (rc != 0 || memh_in == 0 || memh_out == 0) {
            printf("    alloc failed -- skipping submission, closing\n");
        } else {
            /* [11] two descriptors, stock dummy layout with seven live
               words. Geometry/format fields are CLI-overridable
               (din=/dout=); the geometry drives the pattern fill and
               the write size so all three never disagree. The handle
               word is program-set LAST, an override there is rejected
               at parse time. */
            gh = din_set[0] ? din_val[0] : 8;
            gw = din_set[1] ? din_val[1] : 8;
            gf = din_set[2] ? din_val[2] : 0x105a500cu;
            gt = din_set[3] ? din_val[3] : 1;
            gs = din_set[4] ? din_val[4] : 256;
            gp = din_set[9] ? din_val[9] : 1;

            desc_in[0] = gh;
            desc_in[1] = gw;
            desc_in[2] = gf;
            desc_in[3] = gt;
            desc_in[4] = gs;
            desc_in[9] = gp;
            desc_out[0] = dout_set[0] ? dout_val[0] : gh;
            desc_out[1] = dout_set[1] ? dout_val[1] : gw;
            desc_out[2] = dout_set[2] ? dout_val[2] : gf;
            desc_out[3] = dout_set[3] ? dout_val[3] : gt;
            desc_out[4] = dout_set[4] ? dout_val[4] : gs;
            desc_out[9] = dout_set[9] ? dout_val[9] : gp;
            for (k = 0; k < 44; k++) {
                if (din_set[k] && k != 5)
                    desc_in[k] = din_val[k];
                if (dout_set[k] && k != 5)
                    desc_out[k] = dout_val[k];
            }
            desc_in[5] = (unsigned)memh_in;   /* OUR input handle */
            desc_out[5] = (unsigned)memh_out; /* OUR output handle */

            /*
             * Output pre-fill: 0xDEADBEEF repeated, written through the
             * same Write path. Pages come back NOT zeroed (measured),
             * so post-submit content proves nothing by itself -- but
             * with a known pattern in place the post-submit rule is
             * simple: everything that DIFFERS from the pattern, the
             * ISP wrote. Zero diffs = untouched; partial = started and
             * stopped; full = frame written. The pattern buffer is
             * kept alive for that comparison.
             */
            {
                unsigned pbytes = gs * gh;
                unsigned wq;
                pat_out = malloc(pbytes);
                if (pat_out == 0) {
                    printf("[11] output pre-fill malloc failed -- "
                           "post-submit diff will be blind\n");
                } else {
                    for (wq = 0; wq + 4 <= pbytes; wq += 4) {
                        pat_out[wq] = (unsigned char)0xEF;
                        pat_out[wq + 1] = (unsigned char)0xBE;
                        pat_out[wq + 2] = (unsigned char)0xAD;
                        pat_out[wq + 3] = (unsigned char)0xDE;
                    }
                    rc = nvRmMemWrite((unsigned)memh_out, (void *)0,
                                      (unsigned)pat_out, pbytes);
                    printf("[11] output pre-fill 0xDEADBEEF: %u bytes -> "
                           "rc=0x%x\n", pbytes, (unsigned)rc);
                }
            }

            printf("[11] desc_in=%p (h=%u w=%u fmt=0x%x type=%u stride=%u "
                   "memh=0x%x planes=%u)\n",
                   desc_in, desc_in[0], desc_in[1], desc_in[2], desc_in[3],
                   desc_in[4], desc_in[5], desc_in[9]);
            printf("[11] desc_out=%p (h=%u w=%u fmt=0x%x type=%u stride=%u "
                   "memh=0x%x planes=%u)\n",
                   desc_out, desc_out[0], desc_out[1], desc_out[2],
                   desc_out[3], desc_out[4], desc_out[5], desc_out[9]);

            /* state 1 of 3: the OUTPUT buffer as allocation handed it,
               before anything else. Same junk as after the submission
               means reused memory; zeros there but content after means
               the ISP wrote. */
            {
                unsigned pbytes = gs * gh;
                unsigned char *chk = malloc(pbytes);
                unsigned rrc;
                if (chk == 0) {
                    printf("[11] output-after-alloc: malloc failed\n");
                } else {
                    memset(chk, 0xEE, pbytes);
                    rrc = nvRmMemRead((unsigned)memh_out, (void *)0,
                                      (unsigned)chk, pbytes);
                    print_first_words("[11] output-after-alloc first words",
                                      chk, 8);
                    printf("[11] output-after-alloc: rc=0x%x, "
                           "non-poison bytes %u of %u\n",
                           (unsigned)rrc,
                           pbytes - chk[0] * 0, pbytes); /* count below */
                    {
                        unsigned nz = 0;
                        int q;
                        for (q = 0; q < (int)pbytes; q++)
                            if (chk[q] != 0xEE)
                                nz++;
                        printf("[11] output-after-alloc: bytes differing "
                               "from poison: %u of %u\n", nz, pbytes);
                    }
                    free(chk);
                }
            }

            /* [12] the submission itself. The intent line goes out
               before the call: if the call never returns (a fence wait,
               for instance), the log shows exactly where it stopped. */
            for (i = 0; i < 7; i++) {
                if (!aux_on[i])
                    continue;
                printf("[11] aux%s=", aux_names[i]);
                for (k = 0; k < 16; k++)
                    printf("%s%02x:%08x", k != 0 ? "," : "+", k * 4,
                           bufs[i][k]);
                printf("\n");
            }
            /*
             * [11b] fill the INPUT buffer with the generated Bayer --
             * the buffer the ISP actually reads. Found order hops =
             * (handle, offset, ptr, size), offset 0, size stride*height,
             * so rows land in nvmap as bayer_fill laid them out. Then
             * state 2 of 3: read it back and compare byte-for-byte
             * with what we sent. (Commit 2b521e1 claimed this block;
             * a silent patch failure left it out -- the presence rule
             * in argtest.sh now catches that class.)
             */
            {
                unsigned pat_bytes = gs * gh;
                unsigned char *pat = malloc(pat_bytes);
                unsigned char *chk = malloc(pat_bytes);
                unsigned wrc, rrc, diff = 0;
                int q;

                if (pat == 0 || chk == 0) {
                    printf("[11] pattern malloc failed -- input buffer "
                           "stays as allocated\n");
                } else {
                    bayer_fill(pat, gw, gh, gs, 2, order);
                    wrc = nvRmMemWrite((unsigned)memh_in, (void *)0,
                                       (unsigned)pat, pat_bytes);
                    printf("[11] NvRmMemWrite(hops) memh_in <- %u pattern "
                           "bytes -> rc=0x%x\n", pat_bytes, (unsigned)wrc);

                    memset(chk, 0xEE, pat_bytes);
                    rrc = nvRmMemRead((unsigned)memh_in, (void *)0,
                                      (unsigned)chk, pat_bytes);
                    for (q = 0; q < (int)pat_bytes; q++)
                        if (chk[q] != pat[q])
                            diff++;
                    print_first_words(
                        "[11] input-after-write first words", chk, 8);
                    printf("[11] input-after-write: rc=0x%x, %u of %u "
                           "bytes differ from pattern\n",
                           (unsigned)rrc, diff, pat_bytes);
                }
                free(pat);
                free(chk);
            }

            for (i = 0; i < 7; i++)
                for (k = 0; k < 16; k++)
                    snap_aux[i][k] = bufs[i][k];
            for (k = 0; k < 16; k++) {
                snap_in[k] = desc_in[k];
                snap_out[k] = desc_out[k];
                snap14[k] = slot14_buf[k];
            }

            printf("[12] aux slots:");
            for (i = 0; i < 7; i++)
                printf(" +%s=%p(%s)", aux_names[i],
                       aux_on[i] ? (void *)bufs[i] : (void *)0,
                       aux_on[i] ? "on" : "off");
            printf("\n");
            printf("[12] NvIspProcessFrame(hIsp=0x%x, mode=%u, "
                   "in@+0x10=%p, +0x14=%s=%p) -> calling...\n",
                   hIsp, pf_mode, desc_in,
                   slot14_aux ? "aux" : "desc",
                   slot14_aux ? (void *)slot14_buf : (void *)desc_out);
            rc = nvIspProcessFrame(hIsp, pf_mode, 0, 0,
                                   aux_on[0] ? (unsigned)bufs[0] : 0,
                                   aux_on[1] ? (unsigned)bufs[1] : 0,
                                   aux_on[2] ? (unsigned)bufs[2] : 0,
                                   aux_on[3] ? (unsigned)bufs[3] : 0,
                                   (unsigned)desc_in,
                                   slot14_aux ? (unsigned)slot14_buf
                                              : (unsigned)desc_out,
                                   aux_on[4] ? (unsigned)bufs[4] : 0,
                                   aux_on[5] ? (unsigned)bufs[5] : 0,
                                   aux_on[6] ? (unsigned)bufs[6] : 0);
            printf("    ProcessFrame returned rc=0x%x\n", (unsigned)rc);

            /* post-call state of EVERY buffer we hand over, with the
               changed-word list: a "fix-and-repeat" answer (rc=10
               protocol) would be exactly one written word somewhere,
               invisible across sixteen lines of values. */
            print_state_diff("[12b] post desc_in ", snap_in, desc_in, 16);
            print_state_diff("[12b] post desc_out", snap_out, desc_out, 16);
            for (i = 0; i < 7; i++) {
                char tag[40];
                if (!aux_on[i])
                    continue;
                snprintf(tag, sizeof(tag), "[12b] post aux%s ",
                         aux_names[i]);
                print_state_diff(tag, snap_aux[i], bufs[i], 16);
            }
            if (slot14_aux)
                print_state_diff("[12b] post slot14 ", snap14,
                                 slot14_buf, 16);

            /* [12b] read the OUTPUT buffer back whatever the rc was:
               the first submission that works must meet the read side
               already in place. Poisoned with 0xEE first, so untouched
               bytes are visible as such. */
            {
                unsigned pat_bytes = gs * gh;
                unsigned char *outb = malloc(pat_bytes);
                unsigned rrc, pat_diff = 0;
                int m, q;
                if (outb == 0) {
                    printf("[12] output malloc failed -- nothing to read\n");
                } else {
                    /* async hypothesis, crutch stage: wait=<ms> before
                       the read. Content that appears only with the
                       delay proves asynchronous processing. */
                    if (wait_ms != 0) {
                        struct timespec ts;
                        ts.tv_sec = (time_t)(wait_ms / 1000);
                        ts.tv_nsec = (long)(wait_ms % 1000) * 1000000L;
                        printf("[12] waiting %u ms before read\n", wait_ms);
                        nanosleep(&ts, 0);
                    }
                    memset(outb, 0xEE, pat_bytes);
                    rrc = nvRmMemRead((unsigned)memh_out, (void *)0,
                                      (unsigned)outb, pat_bytes);
                    printf("[12] output-after-submit: NvRmMemRead(hops) "
                           "memh_out %u bytes -> rc=0x%x\n",
                           pat_bytes, (unsigned)rrc);
                    print_first_words("[12] output-after-submit first words",
                                      outb, 8);
                    if (pat_out != 0) {
                        for (q = 0; q < (int)pat_bytes; q++)
                            if (outb[q] != pat_out[q])
                                pat_diff++;
                        printf("[12] post-submit: %u of %u bytes differ "
                               "from the 0xDEADBEEF pattern (%s)\n",
                               pat_diff, pat_bytes,
                               pat_diff == 0 ? "ISP did not touch it" :
                               pat_diff == pat_bytes ? "fully written" :
                               "partially written -- processing started");
                    }
                    for (m = 0; m < 8; m++)
                        printf(" %08x",
                               (unsigned)outb[m * 4] |
                               ((unsigned)outb[m * 4 + 1] << 8) |
                               ((unsigned)outb[m * 4 + 2] << 16) |
                               ((unsigned)outb[m * 4 + 3] << 24));
                    printf("\n");
                    free(outb);
                }
            }
        }
    }

    /* [13] hand it back, completely: NvIspClose releases everything
       itself, including the host1x channel. */
    printf("[13] NvIspClose(hIsp=0x%x) -> ", hIsp);
    rc = nvIspClose(hIsp);
    printf("rc=0x%x\n", (unsigned)rc);

    printf("done\n");
    return 0;
}


/*
 * Stage hooks: same register discipline as the verified shim
 * trampolines. Frame after push {r0-r3, lr, r12} (24 bytes):
 *   sp+0..12 = r0..r3, sp+16 = caller lr, sp+20 = scratch slot.
 * stage_pre prints and returns the original; caller lr is parked in
 * the scratch slot, the continuation address goes into lr, args are
 * restored, sp returns to entry value (stack args stay valid), and
 * bx jumps to the original. stage_cont logs the return code and
 * returns to the parked caller lr.
 */
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n");
__asm__(
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden stage_hook_0\n"
    "stage_hook_0:\n"
    "  push {r0-r3, lr, r12}\n"
    "  mov  r0, #0\n"
    "  add  r1, sp, #4\n"
    "  bl   stage_pre\n"
    "  mov  r12, r0\n"
    "  ldr  r0, [sp, #16]\n"
    "  str  r0, [sp, #20]\n"
    "  ldr  lr, [pc, #8]\n"
    "  b    2f\n"
    "  .align 2\n"
    "  .word stage_cont\n"
    "2:  add  sp, #4\n"
    "  ldm  sp!, {r0-r3}\n"
    "  bx   r12\n");
__asm__(
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden stage_hook_1\n"
    "stage_hook_1:\n"
    "  push {r0-r3, lr, r12}\n"
    "  mov  r0, #1\n"
    "  add  r1, sp, #4\n"
    "  bl   stage_pre\n"
    "  mov  r12, r0\n"
    "  ldr  r0, [sp, #16]\n"
    "  str  r0, [sp, #20]\n"
    "  ldr  lr, [pc, #8]\n"
    "  b    2f\n"
    "  .align 2\n"
    "  .word stage_cont\n"
    "2:  add  sp, #4\n"
    "  ldm  sp!, {r0-r3}\n"
    "  bx   r12\n");
__asm__(
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden stage_hook_2\n"
    "stage_hook_2:\n"
    "  push {r0-r3, lr, r12}\n"
    "  mov  r0, #2\n"
    "  add  r1, sp, #4\n"
    "  bl   stage_pre\n"
    "  mov  r12, r0\n"
    "  ldr  r0, [sp, #16]\n"
    "  str  r0, [sp, #20]\n"
    "  ldr  lr, [pc, #8]\n"
    "  b    2f\n"
    "  .align 2\n"
    "  .word stage_cont\n"
    "2:  add  sp, #4\n"
    "  ldm  sp!, {r0-r3}\n"
    "  bx   r12\n");
__asm__(
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden stage_hook_3\n"
    "stage_hook_3:\n"
    "  push {r0-r3, lr, r12}\n"
    "  mov  r0, #3\n"
    "  add  r1, sp, #4\n"
    "  bl   stage_pre\n"
    "  mov  r12, r0\n"
    "  ldr  r0, [sp, #16]\n"
    "  str  r0, [sp, #20]\n"
    "  ldr  lr, [pc, #8]\n"
    "  b    2f\n"
    "  .align 2\n"
    "  .word stage_cont\n"
    "2:  add  sp, #4\n"
    "  ldm  sp!, {r0-r3}\n"
    "  bx   r12\n");
__asm__(
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden stage_hook_4\n"
    "stage_hook_4:\n"
    "  push {r0-r3, lr, r12}\n"
    "  mov  r0, #4\n"
    "  add  r1, sp, #4\n"
    "  bl   stage_pre\n"
    "  mov  r12, r0\n"
    "  ldr  r0, [sp, #16]\n"
    "  str  r0, [sp, #20]\n"
    "  ldr  lr, [pc, #8]\n"
    "  b    2f\n"
    "  .align 2\n"
    "  .word stage_cont\n"
    "2:  add  sp, #4\n"
    "  ldm  sp!, {r0-r3}\n"
    "  bx   r12\n");
__asm__(
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden stage_hook_5\n"
    "stage_hook_5:\n"
    "  push {r0-r3, lr, r12}\n"
    "  mov  r0, #5\n"
    "  add  r1, sp, #4\n"
    "  bl   stage_pre\n"
    "  mov  r12, r0\n"
    "  ldr  r0, [sp, #16]\n"
    "  str  r0, [sp, #20]\n"
    "  ldr  lr, [pc, #8]\n"
    "  b    2f\n"
    "  .align 2\n"
    "  .word stage_cont\n"
    "2:  add  sp, #4\n"
    "  ldm  sp!, {r0-r3}\n"
    "  bx   r12\n");
__asm__(
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden stage_hook_6\n"
    "stage_hook_6:\n"
    "  push {r0-r3, lr, r12}\n"
    "  mov  r0, #6\n"
    "  add  r1, sp, #4\n"
    "  bl   stage_pre\n"
    "  mov  r12, r0\n"
    "  ldr  r0, [sp, #16]\n"
    "  str  r0, [sp, #20]\n"
    "  ldr  lr, [pc, #8]\n"
    "  b    2f\n"
    "  .align 2\n"
    "  .word stage_cont\n"
    "2:  add  sp, #4\n"
    "  ldm  sp!, {r0-r3}\n"
    "  bx   r12\n");
__asm__(
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden stage_hook_7\n"
    "stage_hook_7:\n"
    "  push {r0-r3, lr, r12}\n"
    "  mov  r0, #7\n"
    "  add  r1, sp, #4\n"
    "  bl   stage_pre\n"
    "  mov  r12, r0\n"
    "  ldr  r0, [sp, #16]\n"
    "  str  r0, [sp, #20]\n"
    "  ldr  lr, [pc, #8]\n"
    "  b    2f\n"
    "  .align 2\n"
    "  .word stage_cont\n"
    "2:  add  sp, #4\n"
    "  ldm  sp!, {r0-r3}\n"
    "  bx   r12\n");
