/*
 * ispattr — which attributes does the ISP library accept?
 *
 * The demosaic is not switched on by a register. Its settings object holds a
 * byte at offset zero, and the library's own copy routine skips the whole
 * nine-word coefficient block when that byte is clear -- so what turns the
 * stage on is a field, set through the library, and applied by it. That is
 * why it never appears in a command trace: a trace shows the result, not the
 * decision, and no amount of poking registers was going to find it.
 *
 * This asks the library directly. Create a settings object, then walk the
 * attribute numbers and see which ones it accepts. The ones that answer are
 * the surface we can actually drive; among them is the one we want.
 *
 * Nothing here touches the hardware: it opens the library, allocates a
 * settings object and asks questions.
 *
 * Build: tools/ispattr/build-ispattr.sh (on the build server)
 * Usage: ./ispattr [--instance=N] [--max=N] [--set]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

int main(int argc, char **argv)
{
    int instance = 2;          /* ISP-B; the other tool uses 1 for ISP-A */
    unsigned maxattr = 256;
    int do_set = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--instance=", 11) == 0) instance = atoi(a + 11);
        else if (strncmp(a, "--max=", 6) == 0)  maxattr = (unsigned)atoi(a + 6);
        else if (strcmp(a, "--set") == 0)       do_set = 1;
        else { printf("unknown option %s\n", a); return 1; }
    }

    void *rm = dlopen("libnvrm.so", RTLD_NOW);
    void *isp = dlopen("libnvisp_v3.so", RTLD_NOW);
    if (!rm || !isp) { printf("dlopen: %s\n", dlerror()); return 1; }

    int (*NvRmOpen)(void **, uint32_t) = dlsym(rm, "NvRmOpen");
    int (*NvIspOpen)(void *, int, void **) = dlsym(isp, "NvIspOpen");
    int (*NvIspClose)(void *) = dlsym(isp, "NvIspClose");
    int (*HwSettingsCreate)(void *, void **) =
        dlsym(isp, "NvIspHwSettingsCreate");
    int (*HwSettingsSetAttribute)(void *, uint32_t, const void *, uint32_t) =
        dlsym(isp, "NvIspHwSettingsSetAttribute");
    int (*HwSettingsGetAttribute)(void *, uint32_t, void *, uint32_t) =
        dlsym(isp, "NvIspHwSettingsGetAttribute");
    int (*HwSettingsApply)(void *, void *) =
        dlsym(isp, "NvIspHwSettingsApply");
    int (*HwSettingsDestroy)(void *) = dlsym(isp, "NvIspHwSettingsDestroy");

    printf("symbols: RmOpen=%p IspOpen=%p Create=%p Set=%p Get=%p Apply=%p\n",
           (void *)NvRmOpen, (void *)NvIspOpen, (void *)HwSettingsCreate,
           (void *)HwSettingsSetAttribute, (void *)HwSettingsGetAttribute,
           (void *)HwSettingsApply);
    if (!NvRmOpen || !NvIspOpen || !HwSettingsCreate ||
        !HwSettingsSetAttribute) {
        printf("the library is missing what this needs\n");
        return 1;
    }

    void *hRm = 0;
    int rc = NvRmOpen(&hRm, 0);
    printf("NvRmOpen: rc=%d handle=%p\n", rc, hRm);
    if (!hRm) return 1;

    void *hIsp = 0;
    rc = NvIspOpen(hRm, instance, &hIsp);
    printf("NvIspOpen(instance %d): rc=%d handle=%p\n", instance, rc, hIsp);
    if (!hIsp) return 1;

    void *hSet = 0;
    rc = HwSettingsCreate(hIsp, &hSet);
    printf("HwSettingsCreate: rc=%d handle=%p\n", rc, hSet);
    if (!hSet) { if (NvIspClose) NvIspClose(hIsp); return 1; }

    /* Read first. An attribute that answers a read exists; one that also
     * accepts a write is one we can drive. Reading is the safer half, so it
     * goes first and --set is what asks for the other. */
    printf("attributes that answer:\n");
    unsigned readable = 0, writable = 0;
    for (unsigned id = 0; id < maxattr; id++) {
        uint32_t val = 0;
        int grc = HwSettingsGetAttribute
                ? HwSettingsGetAttribute(hSet, id, &val, sizeof val) : -1;
        if (grc == 0) {
            readable++;
            int src = -1;
            if (do_set) {
                uint32_t one = 1;
                src = HwSettingsSetAttribute(hSet, id, &one, sizeof one);
                if (src == 0) writable++;
            }
            printf("  attr %3u (0x%02x): read 0x%08x%s\n", id, id, val,
                   do_set ? (src == 0 ? "  [write ok]" : "  [write refused]")
                          : "");
        }
    }
    printf("%u readable", readable);
    if (do_set) printf(", %u writable", writable);
    printf(" of %u\n", maxattr);

    if (HwSettingsDestroy) HwSettingsDestroy(hSet);
    if (NvIspClose) NvIspClose(hIsp);
    return 0;
}
