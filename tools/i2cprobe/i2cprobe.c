/*
 * i2cprobe — read and write a camera sensor register directly.
 *
 * The receiver is configured exactly as a working stock session has it, and
 * its lane interface still reads zero: nothing arrives on the wire. The
 * sensor accepts a mode and reports success, so the question is what its
 * streaming register actually holds afterwards -- 0x0100 is the one that
 * starts and stops transmission, and the driver writes it last, only if
 * every calibration step before it succeeded.
 *
 * ov5693 sits at 0x36 on i2c-2 and takes a 16-bit register address with an
 * 8-bit value. The address is claimed by its driver, so the slave is set
 * with the forcing variant -- this reads the part underneath a driver that
 * is holding it, which is the point.
 *
 * Build: tools/i2cprobe/build-i2cprobe.sh (on the build server)
 * Usage: ./i2cprobe [--bus=2] [--addr=0x36] --reg=0x0100 [--write=0xVV]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#define I2C_SLAVE_FORCE 0x0706
#define I2C_RDWR        0x0707

struct i2c_msg {
    uint16_t addr;
    uint16_t flags;
#define I2C_M_RD 0x0001
    uint16_t len;
    uint8_t *buf;
};
struct i2c_rdwr_ioctl_data {
    struct i2c_msg *msgs;
    uint32_t nmsgs;
};

int main(int argc, char **argv)
{
    int bus = 2, addr = 0x36, reg = -1;
    long wr = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--bus=", 6) == 0)        bus = atoi(a + 6);
        else if (strncmp(a, "--addr=", 7) == 0)  addr = (int)strtol(a + 7, 0, 0);
        else if (strncmp(a, "--reg=", 6) == 0)   reg = (int)strtol(a + 6, 0, 0);
        else if (strncmp(a, "--write=", 8) == 0) wr = strtol(a + 8, 0, 0);
        else { printf("unknown option %s\n", a); return 1; }
    }
    if (reg < 0) { printf("--reg is required\n"); return 1; }

    char node[32];
    snprintf(node, sizeof node, "/dev/i2c-%d", bus);
    int fd = open(node, O_RDWR);
    if (fd < 0) { printf("open %s: %s\n", node, strerror(errno)); return 1; }

    /* Forcing, because the sensor's own driver holds this address. */
    if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0) {
        printf("set slave 0x%02x: %s\n", addr, strerror(errno));
        close(fd);
        return 1;
    }

    uint8_t ra[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xff) };
    uint8_t val = 0;

    if (wr >= 0) {
        uint8_t buf[3] = { ra[0], ra[1], (uint8_t)wr };
        struct i2c_msg m = { (uint16_t)addr, 0, 3, buf };
        struct i2c_rdwr_ioctl_data d = { &m, 1 };
        int rc = ioctl(fd, I2C_RDWR, &d);
        printf("write 0x%04x = 0x%02x: rc=%d (%s)\n", reg, (unsigned)wr, rc,
               rc < 0 ? strerror(errno) : "ok");
    }

    struct i2c_msg m[2] = {
        { (uint16_t)addr, 0, 2, ra },
        { (uint16_t)addr, I2C_M_RD, 1, &val },
    };
    struct i2c_rdwr_ioctl_data d = { m, 2 };
    int rc = ioctl(fd, I2C_RDWR, &d);
    if (rc < 0)
        printf("read 0x%04x: %s\n", reg, strerror(errno));
    else
        printf("0x%04x = 0x%02x\n", reg, val);

    close(fd);
    return rc < 0 ? 1 : 0;
}
