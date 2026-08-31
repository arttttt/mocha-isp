/*
 * memprobe — read and write a physical register block through /dev/mem.
 *
 * The VI and CSI registers now match a live stock session exactly and the
 * capture still does not happen, so what is missing is outside that
 * aperture: the MIPI physical layer. Its calibration block sits at
 * 0x700e3000 by /proc/iomem, and the pads' deep-power-down release lives in
 * the PMC -- neither is reachable through nvhost's per-module register
 * ioctl, but both are ordinary physical addresses and we are root.
 *
 * Reads by default. Writing needs --write, one address at a time, because
 * a wrong store here reaches the whole SoC and not just the camera.
 *
 * Build: tools/memprobe/build-memprobe.sh (on the build server)
 * Usage: ./memprobe --addr=0x700e3000 [--count=N]
 *        ./memprobe --addr=0x700e3000 --write=0xVALUE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
    unsigned long addr = 0x700e3000UL;
    unsigned count = 16;
    long write_val = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--addr=", 7) == 0)       addr = strtoul(a + 7, 0, 0);
        else if (strncmp(a, "--count=", 8) == 0) count = (unsigned)strtoul(a + 8, 0, 0);
        else if (strncmp(a, "--write=", 8) == 0) write_val = (long)strtoul(a + 8, 0, 0);
        else { printf("unknown option %s\n", a); return 1; }
    }

    long page = sysconf(_SC_PAGESIZE);
    unsigned long base = addr & ~(unsigned long)(page - 1);
    unsigned long skew = addr - base;

    int fd = open("/dev/mem", write_val >= 0 ? O_RDWR | O_SYNC : O_RDONLY | O_SYNC);
    if (fd < 0) { printf("open /dev/mem: %s\n", strerror(errno)); return 1; }

    size_t len = (size_t)page * 2;
    void *m = mmap(0, len, write_val >= 0 ? (PROT_READ | PROT_WRITE) : PROT_READ,
                   MAP_SHARED, fd, (off_t)base);
    if (m == MAP_FAILED) {
        printf("mmap 0x%08lx: %s\n", base, strerror(errno));
        close(fd);
        return 1;
    }
    volatile uint32_t *r = (volatile uint32_t *)((char *)m + skew);

    if (write_val >= 0) {
        printf("0x%08lx: 0x%08x -> 0x%08lx\n", addr, r[0], (unsigned long)write_val);
        r[0] = (uint32_t)write_val;
        __sync_synchronize();
        printf("  readback 0x%08x\n", r[0]);
    } else {
        printf("=== 0x%08lx, %u words ===\n", addr, count);
        for (unsigned i = 0; i < count; i += 4) {
            printf("  +0x%02x:", i * 4);
            for (unsigned j = i; j < i + 4 && j < count; j++)
                printf(" %08x", r[j]);
            printf("\n");
        }
    }

    munmap(m, len);
    close(fd);
    return 0;
}
