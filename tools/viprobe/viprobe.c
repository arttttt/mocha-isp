/*
 * viprobe — can we reach the VI registers from userspace on stock?
 *
 * The sensor already streams from our own program: /dev/imx179 takes a
 * power call and a mode, and the kernel driver carries the tables. What is
 * missing between the sensor and the ISP is VI -- the CSI receiver and the
 * pixel path -- and on the stock firmware nothing of ours drives it.
 *
 * Before writing a capture program on top of that assumption, check the
 * mechanism: nvhost exposes a register read/write ioctl on a module's
 * channel, the same one libnvisp_v3 uses to put 0x20 into the ISP's 0xFC.
 * If it answers for VI, the whole capture path is reachable; if it does
 * not, no amount of register knowledge helps and the approach has to
 * change.
 *
 * Reads only. Offsets from the 24.1 tree's registers.h, where the CSI
 * channel block sits at 0x100 + n*0x100 in the VI aperture.
 *
 * Build: tools/viprobe/build-viprobe.sh (on the build server)
 * Usage: ./viprobe [--dev=/dev/nvhost-vi] [--off=0xNNN] [--count=N]
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

struct nvhost32_ctrl_module_regrdwr_args {
    uint32_t id;
    uint32_t num_offsets;
    uint32_t block_size;
    uint32_t offsets;      /* pointer, 32-bit userspace */
    uint32_t values;       /* pointer */
    uint32_t write;
};

/* NR=14 on a channel fd, NR=5 on the ctrl fd -- try both. */
#define NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR \
    _IOWR(NVHOST_IOCTL_MAGIC, 14, struct nvhost32_ctrl_module_regrdwr_args)
#define NVHOST32_IOCTL_CTRL_MODULE_REGRDWR \
    _IOWR(NVHOST_IOCTL_MAGIC, 5, struct nvhost32_ctrl_module_regrdwr_args)

/* VI CSI channel 0, from registers.h: base 0x100 + n*0x100 */
#define VI_CSI0_BASE            0x100
#define VI_CSI_SINGLE_SHOT      0x004
#define VI_CSI_IMAGE_DEF        0x00c
#define VI_CSI_IMAGE_SIZE       0x018
#define VI_CSI_ERROR_STATUS     0x084

static int read_regs(int fd, unsigned long req, const char *how,
                     uint32_t id, uint32_t off, uint32_t count)
{
    uint32_t offsets[16], values[16];
    if (count > 16) count = 16;
    for (uint32_t i = 0; i < count; i++) offsets[i] = off + i * 4;
    memset(values, 0, sizeof values);

    struct nvhost32_ctrl_module_regrdwr_args a;
    memset(&a, 0, sizeof a);
    a.id = id;
    a.num_offsets = count;
    a.block_size = 4;
    a.offsets = (uint32_t)(uintptr_t)offsets;
    a.values = (uint32_t)(uintptr_t)values;
    a.write = 0;

    errno = 0;
    int rc = ioctl(fd, req, &a);
    printf("  %-10s id=%u off=0x%03x x%u: rc=%d errno=%d (%s)\n",
           how, id, off, count, rc, errno, rc == 0 ? "ok" : strerror(errno));
    if (rc == 0)
        for (uint32_t i = 0; i < count; i++)
            printf("      +0x%03x = 0x%08x\n", off + i * 4, values[i]);
    return rc;
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/nvhost-vi";
    uint32_t off = VI_CSI0_BASE + VI_CSI_IMAGE_DEF, count = 4, id = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--dev=", 6) == 0)        dev = a + 6;
        else if (strncmp(a, "--off=", 6) == 0)   off = (uint32_t)strtoul(a + 6, 0, 0);
        else if (strncmp(a, "--count=", 8) == 0) count = (uint32_t)strtoul(a + 8, 0, 0);
        else if (strncmp(a, "--id=", 5) == 0)    id = (uint32_t)strtoul(a + 5, 0, 0);
        else { printf("unknown option %s\n", a); return 1; }
    }

    printf("=== viprobe: %s, reading 0x%03x x%u ===\n", dev, off, count);
    int fd = open(dev, O_RDWR);
    printf("open %s: fd=%d%s%s\n", dev, fd, fd < 0 ? " -- " : "",
           fd < 0 ? strerror(errno) : "");
    if (fd < 0) return 1;

    /* The channel spelling first, then the ctrl one; whichever answers
     * tells us which node and ioctl number the path really is. */
    if (read_regs(fd, NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR, "channel",
                  id, off, count) != 0)
        read_regs(fd, NVHOST32_IOCTL_CTRL_MODULE_REGRDWR, "ctrl",
                  id, off, count);

    close(fd);
    printf("=== done ===\n");
    return 0;
}
