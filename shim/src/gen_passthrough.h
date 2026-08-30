/* GENERATED FILE -- do not edit.
 *
 * Source binary : /Users/artem/Projects/isp-lab/ref/stock-system/system/vendor/lib/libnvisp_v3.so
 *                  md5 85e94d9a902968251b1967a17d013dad
 * Derived from  : exports_func.tsv (md5 a570b6cf592008a344071bc42c78ca7f)
 * Produced by   : tools/expdump /Users/artem/Projects/isp-lab/ref/stock-system/system/vendor/lib/libnvisp_v3.so | grep -vE '__bss_start|_edata|_end$' | sort > shim/src/exports_func.tsv
 * Exports       : 41 (all defined exports of the source binary;
 *                 linker synthetics __bss_start/_edata/_end excluded:
 *                 imported by no one, checked against the UND sets
 *                 of all importers in the device snapshot)
 * Regenerate    : python3 gen_passthrough.py exports_func.tsv
 *                 gen_passthrough.h /Users/artem/Projects/isp-lab/ref/stock-system/system/vendor/lib/libnvisp_v3.so
 */

void *shim_slot_NvIspCtrlInitialize;
void *shim_slot_NvIspFlush;
void *shim_slot_NvIspCtrlCleanup;
void *shim_slot_NvIspClose;
void *shim_slot_NvIspOpen;
void *shim_slot_NvIspProcessFrame;
void *shim_slot_NvIspSetAttribute;
void *shim_slot_NvIspGetAttribute;
void *shim_slot_NvIspGetStatus;
void *shim_slot_NvIspSetMemoryBandwidth;
void *shim_slot_NvIspUpdateEmcClock;
void *shim_slot_NvIspSetIspClockRate;
void *shim_slot_NvIspHwSettingsDestroyClientHwSettingsList;
void *shim_slot_NvIspHwSettingsDestroy;
void *shim_slot_NvIspHwSettingsCreate;
void *shim_slot_NvIspHwSettingsClone;
void *shim_slot_NvIspHwSettingsSetAttribute;
void *shim_slot_NvIspGetConfiguration;
void *shim_slot_NvIspHwSettingsGetAppliedSettings;
void *shim_slot_NvIspHwSettingsGetAttribute;
void *shim_slot_NvIspHwSettingsApply;
void *shim_slot_NvIspSetConfiguration;
void *shim_slot_NvIspHwSettingsCopyGpp;
void *shim_slot_NvIspHwSettingsCopyLensShading;
void *shim_slot_NvIspHwSettingsCopyDemosaic;
void *shim_slot_NvIspHwSettingsCopyLumaEnhancement;
void *shim_slot_NvIspHwSettingsCopyOutputDownScaler;
void *shim_slot_NvIspHwSettingsCopyBitwiseOperation;
void *shim_slot_NvIspGetStats;
void *shim_slot_NvIspSetStats;
void *shim_slot_PopulateIspHwFunctions_T12x;
void *shim_slot_NvSFxFloat2Fixed;
void *shim_slot_NvSFxFixed2Float;
void *shim_slot_NvCameraHwSettingsApply;
void *shim_slot_NvCameraHwSettingsUpdateDirty;
void *shim_slot_NvCameraConvertDoubleToUFx;
void *shim_slot_NvCameraConvertDoubleToSFx;
void *shim_slot_NvCameraMatMult3x3;
void *shim_slot_NvCameraGetBayerComponent;
void *shim_slot_NvCameraConvertRGrGbBToTlTrBlBr;
void *shim_slot_IsBayerColorFormat;

__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspCtrlInitialize:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspCtrlInitialize\n");
extern void NvIspCtrlInitialize(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspFlush:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspFlush\n");
extern void NvIspFlush(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspCtrlCleanup:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspCtrlCleanup\n");
extern void NvIspCtrlCleanup(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspClose:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspClose\n");
extern void NvIspClose(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspOpen:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspOpen\n");
extern void NvIspOpen(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspProcessFrame:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspProcessFrame\n");
extern void NvIspProcessFrame(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspSetAttribute:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspSetAttribute\n");
extern void NvIspSetAttribute(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspGetAttribute:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspGetAttribute\n");
extern void NvIspGetAttribute(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspGetStatus:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspGetStatus\n");
extern void NvIspGetStatus(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspSetMemoryBandwidth:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspSetMemoryBandwidth\n");
extern void NvIspSetMemoryBandwidth(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspUpdateEmcClock:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspUpdateEmcClock\n");
extern void NvIspUpdateEmcClock(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspSetIspClockRate:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspSetIspClockRate\n");
extern void NvIspSetIspClockRate(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsDestroyClientHwSettingsList:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsDestroyClientHwSettingsList\n");
extern void NvIspHwSettingsDestroyClientHwSettingsList(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsDestroy:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsDestroy\n");
extern void NvIspHwSettingsDestroy(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsCreate:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsCreate\n");
extern void NvIspHwSettingsCreate(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsClone:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsClone\n");
extern void NvIspHwSettingsClone(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsSetAttribute:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsSetAttribute\n");
extern void NvIspHwSettingsSetAttribute(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspGetConfiguration:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspGetConfiguration\n");
extern void NvIspGetConfiguration(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsGetAppliedSettings:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsGetAppliedSettings\n");
extern void NvIspHwSettingsGetAppliedSettings(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsGetAttribute:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsGetAttribute\n");
extern void NvIspHwSettingsGetAttribute(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsApply:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsApply\n");
extern void NvIspHwSettingsApply(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspSetConfiguration:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspSetConfiguration\n");
extern void NvIspSetConfiguration(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsCopyGpp:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsCopyGpp\n");
extern void NvIspHwSettingsCopyGpp(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsCopyLensShading:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsCopyLensShading\n");
extern void NvIspHwSettingsCopyLensShading(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsCopyDemosaic:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsCopyDemosaic\n");
extern void NvIspHwSettingsCopyDemosaic(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsCopyLumaEnhancement:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsCopyLumaEnhancement\n");
extern void NvIspHwSettingsCopyLumaEnhancement(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsCopyOutputDownScaler:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsCopyOutputDownScaler\n");
extern void NvIspHwSettingsCopyOutputDownScaler(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspHwSettingsCopyBitwiseOperation:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspHwSettingsCopyBitwiseOperation\n");
extern void NvIspHwSettingsCopyBitwiseOperation(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspGetStats:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspGetStats\n");
extern void NvIspGetStats(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvIspSetStats:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvIspSetStats\n");
extern void NvIspSetStats(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "PopulateIspHwFunctions_T12x:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_PopulateIspHwFunctions_T12x\n");
extern void PopulateIspHwFunctions_T12x(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvSFxFloat2Fixed:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvSFxFloat2Fixed\n");
extern void NvSFxFloat2Fixed(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvSFxFixed2Float:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvSFxFixed2Float\n");
extern void NvSFxFixed2Float(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvCameraHwSettingsApply:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvCameraHwSettingsApply\n");
extern void NvCameraHwSettingsApply(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvCameraHwSettingsUpdateDirty:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvCameraHwSettingsUpdateDirty\n");
extern void NvCameraHwSettingsUpdateDirty(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvCameraConvertDoubleToUFx:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvCameraConvertDoubleToUFx\n");
extern void NvCameraConvertDoubleToUFx(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvCameraConvertDoubleToSFx:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvCameraConvertDoubleToSFx\n");
extern void NvCameraConvertDoubleToSFx(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvCameraMatMult3x3:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvCameraMatMult3x3\n");
extern void NvCameraMatMult3x3(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvCameraGetBayerComponent:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvCameraGetBayerComponent\n");
extern void NvCameraGetBayerComponent(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "NvCameraConvertRGrGbBToTlTrBlBr:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_NvCameraConvertRGrGbBToTlTrBlBr\n");
extern void NvCameraConvertRGrGbBToTlTrBlBr(void);
__asm__(
    ".text\n"
    ".align 2\n"
    "IsBayerColorFormat:\n"
    "  ldr r12, 1f\n"
    "  bx  r12\n"
    "  .align 2\n"
    "1: .word shim_slot_IsBayerColorFormat\n");
extern void IsBayerColorFormat(void);

static const struct shim_binding shim_bindings[] = {
    { "NvIspCtrlInitialize", &shim_slot_NvIspCtrlInitialize },
    { "NvIspFlush", &shim_slot_NvIspFlush },
    { "NvIspCtrlCleanup", &shim_slot_NvIspCtrlCleanup },
    { "NvIspClose", &shim_slot_NvIspClose },
    { "NvIspOpen", &shim_slot_NvIspOpen },
    { "NvIspProcessFrame", &shim_slot_NvIspProcessFrame },
    { "NvIspSetAttribute", &shim_slot_NvIspSetAttribute },
    { "NvIspGetAttribute", &shim_slot_NvIspGetAttribute },
    { "NvIspGetStatus", &shim_slot_NvIspGetStatus },
    { "NvIspSetMemoryBandwidth", &shim_slot_NvIspSetMemoryBandwidth },
    { "NvIspUpdateEmcClock", &shim_slot_NvIspUpdateEmcClock },
    { "NvIspSetIspClockRate", &shim_slot_NvIspSetIspClockRate },
    { "NvIspHwSettingsDestroyClientHwSettingsList", &shim_slot_NvIspHwSettingsDestroyClientHwSettingsList },
    { "NvIspHwSettingsDestroy", &shim_slot_NvIspHwSettingsDestroy },
    { "NvIspHwSettingsCreate", &shim_slot_NvIspHwSettingsCreate },
    { "NvIspHwSettingsClone", &shim_slot_NvIspHwSettingsClone },
    { "NvIspHwSettingsSetAttribute", &shim_slot_NvIspHwSettingsSetAttribute },
    { "NvIspGetConfiguration", &shim_slot_NvIspGetConfiguration },
    { "NvIspHwSettingsGetAppliedSettings", &shim_slot_NvIspHwSettingsGetAppliedSettings },
    { "NvIspHwSettingsGetAttribute", &shim_slot_NvIspHwSettingsGetAttribute },
    { "NvIspHwSettingsApply", &shim_slot_NvIspHwSettingsApply },
    { "NvIspSetConfiguration", &shim_slot_NvIspSetConfiguration },
    { "NvIspHwSettingsCopyGpp", &shim_slot_NvIspHwSettingsCopyGpp },
    { "NvIspHwSettingsCopyLensShading", &shim_slot_NvIspHwSettingsCopyLensShading },
    { "NvIspHwSettingsCopyDemosaic", &shim_slot_NvIspHwSettingsCopyDemosaic },
    { "NvIspHwSettingsCopyLumaEnhancement", &shim_slot_NvIspHwSettingsCopyLumaEnhancement },
    { "NvIspHwSettingsCopyOutputDownScaler", &shim_slot_NvIspHwSettingsCopyOutputDownScaler },
    { "NvIspHwSettingsCopyBitwiseOperation", &shim_slot_NvIspHwSettingsCopyBitwiseOperation },
    { "NvIspGetStats", &shim_slot_NvIspGetStats },
    { "NvIspSetStats", &shim_slot_NvIspSetStats },
    { "PopulateIspHwFunctions_T12x", &shim_slot_PopulateIspHwFunctions_T12x },
    { "NvSFxFloat2Fixed", &shim_slot_NvSFxFloat2Fixed },
    { "NvSFxFixed2Float", &shim_slot_NvSFxFixed2Float },
    { "NvCameraHwSettingsApply", &shim_slot_NvCameraHwSettingsApply },
    { "NvCameraHwSettingsUpdateDirty", &shim_slot_NvCameraHwSettingsUpdateDirty },
    { "NvCameraConvertDoubleToUFx", &shim_slot_NvCameraConvertDoubleToUFx },
    { "NvCameraConvertDoubleToSFx", &shim_slot_NvCameraConvertDoubleToSFx },
    { "NvCameraMatMult3x3", &shim_slot_NvCameraMatMult3x3 },
    { "NvCameraGetBayerComponent", &shim_slot_NvCameraGetBayerComponent },
    { "NvCameraConvertRGrGbBToTlTrBlBr", &shim_slot_NvCameraConvertRGrGbBToTlTrBlBr },
    { "IsBayerColorFormat", &shim_slot_IsBayerColorFormat },
    { 0, 0 },
};
