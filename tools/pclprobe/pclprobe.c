/*
 * pclprobe — talk to the stock camera PCL node from our own process.
 *
 * On the stock firmware the sensors are reachable without any of NVIDIA's
 * userspace: /dev/camera.pcl registers an i2c device and drives its power
 * and register sequences, and /dev/nvhost-vi is the VI channel. That is the
 * whole sensor front end, which is the side of the ISP we have never been
 * able to reach -- everything from memory enters below the demosaic.
 *
 * This program only probes: it opens the node, registers a sensor, asks for
 * the power state and optionally turns it on. Nothing is streamed yet; the
 * point is to find out which of these calls the kernel accepts from us.
 *
 * From the stock kernel's include/media/camera.h, and the board file:
 *   imx179 (rear)  i2c addr 0x10
 *   ov5693 (front) i2c addr 0x36
 *
 * Build: tools/pclprobe/build-pclprobe.sh (on the build server)
 * Usage: ./pclprobe [--sensor=imx179|ov5693] [--bus=N] [--addr=0xNN]
 *                   [--power=0|1] [--dev=/dev/camera.pcl]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#define CAMERA_MAX_NAME_LENGTH 32
#define CAMERA_DEVICE_TYPE_I2C 0

struct camera_device_info {
    uint8_t  name[CAMERA_MAX_NAME_LENGTH];
    uint32_t type;
    uint8_t  bus;
    uint8_t  addr;
};

struct nvc_param {
    uint32_t param;
    uint32_t sizeofvalue;
    void    *p_value;
};

#define PCLLK_IOCTL_DEV_REG   _IOW('o', 104, struct camera_device_info)
#define PCLLK_IOCTL_DEV_DEL   _IOW('o', 105, int)
#define PCLLK_IOCTL_DEV_FREE  _IOW('o', 106, int)
#define PCLLK_IOCTL_PWR_WR    _IOW('o', 108, int)
#define PCLLK_IOCTL_PWR_RD    _IOR('o', 109, int)
#define PCLLK_IOCTL_PARAM_RD  _IOWR('o', 141, struct nvc_param)

/* The sensors also have their own legacy nodes, and those are the useful
 * ones: the mode tables live in the kernel driver, so powering up and
 * choosing a resolution needs nothing from NVIDIA's userspace at all.
 * From the stock kernel's include/media/imx179.h. */
struct sensor_mode {
    int xres, yres;
    uint32_t frame_length;
    uint32_t coarse_time;
    uint16_t gain;
};
#define SENSOR_IOCTL_SET_MODE   _IOW('o', 1, struct sensor_mode)
#define SENSOR_IOCTL_GET_STATUS _IOR('o', 2, uint8_t)
#define SENSOR_IOCTL_SET_POWER  _IOW('o', 20, uint32_t)

static void try_ioctl(int fd, const char *what, unsigned long req, void *arg)
{
    errno = 0;
    int rc = ioctl(fd, req, arg);
    printf("  %-22s rc=%-3d errno=%d (%s)\n", what, rc, errno,
           rc == 0 ? "ok" : strerror(errno));
}

int main(int argc, char **argv)
{
    const char *node = "/dev/camera.pcl";
    const char *sensor = "imx179";
    int bus = 2, addr = 0x10, power = -1;
    int use_sensor_node = 0, mode_w = 1920, mode_h = 1080;
    int keep_running = 0, power_off_only = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--dev=", 6) == 0)         node = a + 6;
        else if (strncmp(a, "--sensor=", 9) == 0) sensor = a + 9;
        else if (strncmp(a, "--bus=", 6) == 0)    bus = atoi(a + 6);
        else if (strncmp(a, "--addr=", 7) == 0)   addr = (int)strtol(a + 7, 0, 0);
        else if (strncmp(a, "--power=", 8) == 0)  power = atoi(a + 8);
        else if (strcmp(a, "--node") == 0)        use_sensor_node = 1;
        else if (strcmp(a, "--keep") == 0)        keep_running = 1;
        else if (strcmp(a, "--off") == 0)   { use_sensor_node = 1; power_off_only = 1; }
        else if (strncmp(a, "--mode=", 7) == 0)
            sscanf(a + 7, "%dx%d", &mode_w, &mode_h);
        else { printf("unknown option %s\n", a); return 1; }
    }
    if (strcmp(sensor, "ov5693") == 0 && addr == 0x10) addr = 0x36;

    printf("=== pclprobe: %s, sensor %s on i2c-%d addr 0x%02x ===\n",
           node, sensor, bus, addr);

    /* The sensor's own node first, when asked for: it needs no chip
     * registration, because the driver already carries the tables. */
    if (use_sensor_node) {
        char sn[64];
        snprintf(sn, sizeof sn, "/dev/%s", sensor);
        int sfd = open(sn, O_RDWR);
        printf("open %s: fd=%d%s%s\n", sn, sfd,
               sfd < 0 ? " -- " : "", sfd < 0 ? strerror(errno) : "");
        if (sfd < 0) return 1;

        /* --off: just clean up after an earlier run and leave. */
        if (power_off_only) {
            uint32_t off = 0;
            try_ioctl(sfd, "SET_POWER off", SENSOR_IOCTL_SET_POWER, &off);
            close(sfd);
            printf("=== done ===\n");
            return 0;
        }

        uint8_t st = 0;
        try_ioctl(sfd, "GET_STATUS (before)", SENSOR_IOCTL_GET_STATUS, &st);
        printf("  status: 0x%02x\n", st);

        uint32_t on = 1;
        try_ioctl(sfd, "SET_POWER on", SENSOR_IOCTL_SET_POWER, &on);

        st = 0;
        try_ioctl(sfd, "GET_STATUS (powered)", SENSOR_IOCTL_GET_STATUS, &st);
        printf("  status: 0x%02x\n", st);

        struct sensor_mode m;
        memset(&m, 0, sizeof m);
        m.xres = mode_w;
        m.yres = mode_h;
        m.frame_length = 0;
        m.coarse_time = 0;
        m.gain = 0;
        printf("  setting mode %dx%d\n", m.xres, m.yres);
        try_ioctl(sfd, "SET_MODE", SENSOR_IOCTL_SET_MODE, &m);

        /* Leave the part as we found it unless asked to keep it running:
         * a probe that exits with the sensor still streaming leaves the
         * rails up and the MIPI link live for whoever comes next. */
        if (keep_running) {
            printf("  leaving the sensor streaming (--keep)\n");
        } else {
            uint32_t off = 0;
            try_ioctl(sfd, "SET_POWER off", SENSOR_IOCTL_SET_POWER, &off);
        }
        close(sfd);
        printf("=== done ===\n");
        return 0;
    }

    int fd = open(node, O_RDWR);
    if (fd < 0) {
        printf("open %s: %s\n", node, strerror(errno));
        return 1;
    }
    printf("opened, fd=%d\n", fd);

    /* Ask before touching anything: does the node answer a read at all? */
    int pwr = -1;
    try_ioctl(fd, "PWR_RD (before)", PCLLK_IOCTL_PWR_RD, &pwr);
    printf("  power state reported: %d\n", pwr);

    struct camera_device_info info;
    memset(&info, 0, sizeof info);
    strncpy((char *)info.name, sensor, CAMERA_MAX_NAME_LENGTH - 1);
    info.type = CAMERA_DEVICE_TYPE_I2C;
    info.bus  = (uint8_t)bus;
    info.addr = (uint8_t)addr;
    try_ioctl(fd, "DEV_REG", PCLLK_IOCTL_DEV_REG, &info);

    if (power >= 0) {
        int p = power;
        try_ioctl(fd, "PWR_WR", PCLLK_IOCTL_PWR_WR, &p);
        pwr = -1;
        try_ioctl(fd, "PWR_RD (after)", PCLLK_IOCTL_PWR_RD, &pwr);
        printf("  power state reported: %d\n", pwr);
    }

    close(fd);
    printf("=== done ===\n");
    return 0;
}
