/*
 * objread -- dump a region of a LIVE process through /proc/<pid>/mem.
 *
 * The external half of the stock-object capture: the shim prints the
 * object address (ctx+0x1318) inside mediaserver, this program reads
 * the bytes from outside. A wrong address is a read ERROR here, not a
 * crash -- the target process is never touched.
 *
 * Usage: objread <pid> <addr_hex> <outfile> [size]
 *   size in bytes, default 0x4000 (lead's 16 KiB with slack).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char mpath[64];
    unsigned char *buf;
    unsigned long addr, size = 0x4000, done = 0;
    long chunk = 4096;
    int mf, out;
    char *end = 0;

    if (argc < 4) {
        fprintf(stderr,
                "usage: objread <pid> <addr_hex> <outfile> [size]\\n");
        return 2;
    }
    snprintf(mpath, sizeof(mpath), "/proc/%s/mem", argv[1]);
    addr = strtoul(argv[2], &end, 16);
    if (end == argv[2] || *end != '\0') {
        fprintf(stderr, "bad address '%s'\n", argv[2]);
        return 2;
    }
    if (argc > 4) {
        size = strtoul(argv[4], &end, 0);
        if (end == argv[4] || *end != '\0' || size == 0) {
            fprintf(stderr, "bad size '%s'\n", argv[4]);
            return 2;
        }
    }

    mf = open(mpath, O_RDONLY);
    if (mf < 0) {
        perror("open /proc/pid/mem");
        return 1;
    }
    out = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        perror("open outfile");
        return 1;
    }
    buf = malloc(chunk);
    if (buf == 0) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }
    while (done < size) {
        long now = size - done < chunk ? (long)(size - done) : chunk;
        long r = pread(mf, buf, now, (off_t)(addr + done));
        if (r <= 0) {
            printf("read stopped at +0x%lx (%s)\n", done,
                   r < 0 ? "error" : "eof");
            break;
        }
        if (write(out, buf, (size_t)r) != r) {
            fprintf(stderr, "write failed\n");
            return 1;
        }
        done += (unsigned long)r;
    }
    printf("read 0x%lx bytes from 0x%lx -> %s\n", done, addr, argv[3]);
    close(mf);
    close(out);
    return done == size ? 0 : 3;
}
