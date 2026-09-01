/*
 * ispshadow — read the ISP's register shadow out of a running process.
 *
 * The library that drives this ISP keeps a chain of descriptors, one per
 * block of registers it will push: the method, how many words, whether the
 * block is dirty, and the words themselves. The camera stack fills that
 * chain from its own defaults at open time -- before any command trace we
 * can capture begins, which is why the values never appear in a gather we
 * recorded, and why sweeping registers was never going to find them.
 *
 * They are, however, sitting in the process's heap the whole time the
 * camera is open. So read them: attach to the process by name, scan its
 * anonymous mappings for the head of the chain, and print it.
 *
 * This only reads another process's memory. It does not touch the ISP, the
 * VI, or /dev/mem, and it does not write anything anywhere.
 *
 * A descriptor, twelve bytes then the data:
 *     +0x00 u16 method
 *     +0x04 u16 count     (high byte of the word: mode)
 *     +0x06 u8  dirty
 *     +0x07 u8  mode      0 = INCR(method, count)
 *                         1 = INCR(method,1) + NONINCR(method+1, count-1)
 *     +0x08 u8  version   0 on a real entry, non-zero on the terminator
 *     +0x0C     data[count]
 *
 * Build: tools/ispshadow/build-ispshadow.sh (on the build server)
 * Usage: ./ispshadow [--proc=mediaserver] [--anchor=0x900] [--max=64]
 *        ./ispshadow --pid=1234 --c-array
 */

#define _LARGEFILE64_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>

#define MAX_WORDS_PER_BLOCK 1024

static int pid_of(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *e;
    int found = -1, self = (int)getpid();
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int p = atoi(e->d_name);
        if (p == self) continue;        /* our own arguments name it too */
        char path[64], buf[256];
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        int n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        /* The first token only: arguments can name another program. */
        if (strstr(buf, name)) { found = p; break; }
    }
    closedir(d);
    return found;
}

/* One descriptor's total size, or zero if it does not look like one. */
static size_t desc_size(const uint32_t *w, size_t avail)
{
    if (avail < 3) return 0;
    uint32_t method = w[0], cw = w[1];
    unsigned count = cw & 0xFFFF, mode = (cw >> 24) & 0xFF;
    if (method == 0 || method > 0xFFF) return 0;
    if (count == 0 || count > MAX_WORDS_PER_BLOCK) return 0;
    if (mode > 1) return 0;
    if ((cw >> 16) & 0xFF) return 0;          /* nothing lives here */
    if (w[2] & 0xFF) return 0;                /* a real entry, not the end */
    if (3 + count > avail) return 0;
    return 3 + count;
}

int main(int argc, char **argv)
{
    const char *pname = "mediaserver";
    long anchor = 0x900;
    int pid = -1, c_array = 0, want_all = 0, verbose = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--proc=", 7) == 0)        pname = a + 7;
        else if (strncmp(a, "--pid=", 6) == 0)    pid = atoi(a + 6);
        else if (strncmp(a, "--anchor=", 9) == 0) anchor = strtol(a + 9, 0, 0);
        else if (strcmp(a, "--c-array") == 0)     c_array = 1;
        else if (strcmp(a, "--all") == 0)         want_all = 1;
        else if (strcmp(a, "--verbose") == 0)     verbose = 1;
        else { printf("unknown option %s\n", a); return 1; }
    }

    if (pid < 0) pid = pid_of(pname);
    if (pid < 0) { printf("no process matching '%s'\n", pname); return 1; }
    /* Diagnostics go to the error stream so the generated header can be
     * redirected straight into a file. */
    fprintf(stderr, "process %d\n", pid);

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *mf = fopen(path, "r");
    if (!mf) { printf("open maps: %s\n", strerror(errno)); return 1; }
    snprintf(path, sizeof path, "/proc/%d/mem", pid);
    int mem = open(path, O_RDONLY);
    if (mem < 0) { printf("open mem: %s (run as root)\n", strerror(errno));
                   fclose(mf); return 1; }

    char line[512];
    size_t scanned = 0;
    int hits = 0, best_n = 0;
    uintptr_t best_addr = 0;
    uint32_t *best_buf = 0;
    size_t best_words = 0, best_at = 0;

    while (fgets(line, sizeof line, mf)) {
        uintptr_t s, e;
        char perms[8], rest[256];
        rest[0] = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]",
                   (unsigned long *)&s, (unsigned long *)&e, perms, rest) < 3)
            continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        /* An unnamed mapping still comes back with the trailing space of
         * the line, so trim before deciding whether it has a name at all. */
        char *nm = rest;
        while (*nm == ' ' || *nm == '\t') nm++;
        /* Anonymous and heap only: the chain is allocated, not mapped from
         * a file, and skipping the rest keeps this quick and harmless. */
        if (*nm && !strstr(nm, "[heap]") && !strstr(nm, "[anon")
            && !strstr(nm, "libc_malloc") && !strstr(nm, "[stack"))
            continue;
        size_t len = e - s;
        if (len < 0x1000 || len > (64u << 20)) continue;

        uint32_t *buf = malloc(len);
        if (!buf) continue;
        /* The wide read: on a thirty-two bit build an ordinary offset is
         * signed and everything above two gigabytes -- which is where the
         * heap lives -- comes back as an invalid argument. */
        ssize_t got = pread64(mem, buf, len, (off64_t)s);
        if (verbose)
            printf("  %08lx-%08lx %s %-16s -> %ld%s\n", (unsigned long)s,
                   (unsigned long)e, perms, nm, (long)got,
                   got < 0 ? strerror(errno) : "");
        if (got <= 0) { free(buf); continue; }
        size_t words = (size_t)got / 4;
        scanned += words;

        /* The longest chain in the region, found in one pass: how many
         * descriptors follow a position depends only on the position after
         * it, so work backwards and each answer is already known. Hunting
         * for one anchoring method and guessing at the head was finding
         * whichever fragment happened to come first. */
        int32_t *chain = calloc(words + 1, sizeof *chain);
        if (!chain) { free(buf); continue; }
        for (size_t i = words; i-- > 0; ) {
            size_t sz = desc_size(buf + i, words - i);
            if (!sz) continue;
            if (buf[i] == (uint32_t)anchor) hits++;
            chain[i] = 1 + ((i + sz < words) ? chain[i + sz] : 0);
        }
        /* Text passes the header test often enough to win on length alone
         * -- a run of English reads as method 0x143, count 0x6e and so on.
         * So a chain only counts if it carries one of the big tables no
         * sentence is going to imitate: the shading grid or a tone curve,
         * hundreds of words to a single method. */
        size_t head = 0;
        int n = 0;
        for (size_t i = 0; i < words; i++) {
            if (chain[i] <= n) continue;
            int genuine = 0;
            for (size_t j = i; j < words; ) {
                size_t sz = desc_size(buf + j, words - j);
                if (!sz) break;
                unsigned cnt = buf[j + 1] & 0xFFFF;
                if (cnt >= 200) { genuine = 1; break; }
                j += sz;
            }
            if (genuine) { n = chain[i]; head = i; }
        }
        free(chain);

        if (n > best_n) {
            best_n = n;
            best_addr = s + head * 4;
            free(best_buf);
            best_buf = buf;
            best_words = words;
            best_at = head;
            buf = 0;
        }
        free(buf);
    }
    fclose(mf);
    close(mem);

    if (!best_buf || best_n < 2) {
        printf("scanned %zu words, %d place(s) hold 0x%lx, longest chain %d\n",
               scanned, hits, anchor, best_n);
        printf("no descriptor chain found (is the camera open?)\n");
        free(best_buf);
        return 1;
    }

    fprintf(stderr, "chain of %d blocks at %08lx\n\n", best_n,
            (unsigned long)best_addr);

    const uint32_t *w = best_buf + best_at;
    size_t avail = best_words - best_at;
    size_t i = 0;

    /* A header the other tool can include as-is: every block with its
     * method, its length, how it is pushed, and its words. Copying
     * thirty-one blocks across by hand is how a digit goes missing. */
    if (c_array) {
        printf("/* Generated by ispshadow from a running camera.\n"
               " * The ISP's own configuration, as the camera stack fills\n"
               " * it in at open time: %d blocks.\n"
               " *\n"
               " * mode 0 -- INCR(method, count)\n"
               " * mode 1 -- INCR(method, 1) then NONINCR(method+1, count-1)\n"
               " */\n\n", best_n);
        for (int b = 0; b < best_n; b++) {
            size_t sz = desc_size(w + i, avail - i);
            if (!sz) break;
            unsigned method = w[i], count = w[i + 1] & 0xFFFF;
            const uint32_t *d = w + i + 3;
            printf("static const uint32_t isp_blk_%03x[%u] = {\n   ",
                   method, count);
            for (unsigned k = 0; k < count; k++)
                printf(" 0x%08x,%s", d[k],
                       (k % 6 == 5 && k + 1 != count) ? "\n   " : "");
            printf("\n};\n");
            i += sz;
        }
        printf("\nstatic const struct isp_block {\n"
               "    unsigned short method, count;\n"
               "    unsigned char mode;\n"
               "    const uint32_t *data;\n"
               "} isp_stock_blocks[] = {\n");
        i = 0;
        for (int b = 0; b < best_n; b++) {
            size_t sz = desc_size(w + i, avail - i);
            if (!sz) break;
            unsigned method = w[i], count = w[i + 1] & 0xFFFF;
            unsigned mode = (w[i + 1] >> 24) & 0xFF;
            printf("    { 0x%03x, %u, %u, isp_blk_%03x },\n",
                   method, count, mode, method);
            i += sz;
        }
        printf("};\n");
        free(best_buf);
        return 0;
    }

    for (int b = 0; b < best_n; b++) {
        size_t sz = desc_size(w + i, avail - i);
        if (!sz) break;
        unsigned method = w[i], count = w[i + 1] & 0xFFFF;
        unsigned mode = (w[i + 1] >> 24) & 0xFF;
        unsigned dirty = (w[i + 1] >> 16) & 0xFF;   /* byte +6 */
        const uint32_t *d = w + i + 3;

        int nonzero = 0;
        for (unsigned k = 0; k < count; k++) if (d[k]) { nonzero = 1; break; }
        if (!nonzero && !want_all) {
            printf("0x%03x x%-3u mode=%u dirty=%u  (all zero)\n",
                   method, count, mode, dirty);
            i += sz;
            continue;
        }

        printf("0x%03x x%-3u mode=%u dirty=%u\n", method, count, mode, dirty);
        for (unsigned k = 0; k < count; k++)
            printf("   [%3u] %08x%s", k, d[k],
                   (k % 6 == 5 || k + 1 == count) ? "\n" : "");
        i += sz;
    }

    free(best_buf);
    return 0;
}
