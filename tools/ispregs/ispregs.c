/*
 * ispregs — dump the ISP-B (or ISP-A, or VI) register file through the
 * channel's REGRDWR ioctl, the way the kernel itself reads module registers:
 * nvhost powers and clocks the module for the call. No /dev/mem, no gathers,
 * nothing written.
 *
 * The ISP's registers mirror its method space at four bytes a method
 * (0x54 is method 0x015), so this is the block's configuration as it stands
 * -- including everything nobody wrote since reset. Taken on a fresh boot
 * and again after a stock camera session, the diff is what the stock leaves
 * behind that our own init never sets.
 *
 * Usage: ispregs [--node=/dev/nvhost-isp.1] [--end=0x4000] [--all]
 *   prints "+0xOFF = 0xVALUE" for every non-zero register (--all: zeros too),
 *   sixteen registers per ioctl, flushed per line so a hang names its offset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#define NVHOST_IOCTL_MAGIC 'H'
struct regrdwr_args {
    uint32_t id;
    uint32_t num_offsets;
    uint32_t block_size;
    uint32_t offsets;
    uint32_t values;
    uint32_t write;
};
#define NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR _IOWR(NVHOST_IOCTL_MAGIC, 14, struct regrdwr_args)

int main(int argc, char **argv)
{
    const char *node = "/dev/nvhost-isp.1";
    uint32_t end = 0x4000;
    int all = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--node=", 7) == 0) node = argv[i] + 7;
        else if (strncmp(argv[i], "--end=", 6) == 0) end = (uint32_t)strtoul(argv[i] + 6, 0, 0);
        else if (strcmp(argv[i], "--all") == 0) all = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    int fd = open(node, O_RDWR);
    if (fd < 0) { printf("open %s: %s\n", node, strerror(errno)); return 1; }
    printf("=== %s registers 0x000..0x%x ===\n", node, end);
    unsigned failed = 0, nonzero = 0;
    for (uint32_t o = 0; o < end; o += 64) {
        uint32_t offs[16], vals[16];
        for (int i = 0; i < 16; i++) { offs[i] = o + i * 4; vals[i] = 0; }
        struct regrdwr_args a;
        memset(&a, 0, sizeof a);
        a.id = 0;
        a.num_offsets = 16;
        a.block_size = 4;
        a.offsets = (uint32_t)(uintptr_t)offs;
        a.values = (uint32_t)(uintptr_t)vals;
        a.write = 0;
        if (ioctl(fd, NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR, &a) < 0) {
            if (failed++ < 4) printf("  +0x%04x..: %s\n", o, strerror(errno));
            continue;
        }
        for (int i = 0; i < 16; i++)
            if (vals[i] || all) { printf("+0x%04x = 0x%08x\n", offs[i], vals[i]); nonzero += vals[i] != 0; }
    }
    printf("=== end: %u non-zero, %u blocks failed ===\n", nonzero, failed);
    close(fd);
    return 0;
}
