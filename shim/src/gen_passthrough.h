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

extern void *shim_slot_NvIspCtrlInitialize;
extern void *shim_slot_NvIspFlush;
extern void *shim_slot_NvIspCtrlCleanup;
extern void *shim_slot_NvIspClose;
extern void *shim_slot_NvIspOpen;
extern void *shim_slot_NvIspProcessFrame;
extern void *shim_slot_NvIspSetAttribute;
extern void *shim_slot_NvIspGetAttribute;
extern void *shim_slot_NvIspGetStatus;
extern void *shim_slot_NvIspSetMemoryBandwidth;
extern void *shim_slot_NvIspUpdateEmcClock;
extern void *shim_slot_NvIspSetIspClockRate;
extern void *shim_slot_NvIspHwSettingsDestroyClientHwSettingsList;
extern void *shim_slot_NvIspHwSettingsDestroy;
extern void *shim_slot_NvIspHwSettingsCreate;
extern void *shim_slot_NvIspHwSettingsClone;
extern void *shim_slot_NvIspHwSettingsSetAttribute;
extern void *shim_slot_NvIspGetConfiguration;
extern void *shim_slot_NvIspHwSettingsGetAppliedSettings;
extern void *shim_slot_NvIspHwSettingsGetAttribute;
extern void *shim_slot_NvIspHwSettingsApply;
extern void *shim_slot_NvIspSetConfiguration;
extern void *shim_slot_NvIspHwSettingsCopyGpp;
extern void *shim_slot_NvIspHwSettingsCopyLensShading;
extern void *shim_slot_NvIspHwSettingsCopyDemosaic;
extern void *shim_slot_NvIspHwSettingsCopyLumaEnhancement;
extern void *shim_slot_NvIspHwSettingsCopyOutputDownScaler;
extern void *shim_slot_NvIspHwSettingsCopyBitwiseOperation;
extern void *shim_slot_NvIspGetStats;
extern void *shim_slot_NvIspSetStats;
extern void *shim_slot_PopulateIspHwFunctions_T12x;
extern void *shim_slot_NvSFxFloat2Fixed;
extern void *shim_slot_NvSFxFixed2Float;
extern void *shim_slot_NvCameraHwSettingsApply;
extern void *shim_slot_NvCameraHwSettingsUpdateDirty;
extern void *shim_slot_NvCameraConvertDoubleToUFx;
extern void *shim_slot_NvCameraConvertDoubleToSFx;
extern void *shim_slot_NvCameraMatMult3x3;
extern void *shim_slot_NvCameraGetBayerComponent;
extern void *shim_slot_NvCameraConvertRGrGbBToTlTrBlBr;
extern void *shim_slot_IsBayerColorFormat;

__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspCtrlInitialize\n"
    ".type NvIspCtrlInitialize, %function\n"
    "NvIspCtrlInitialize:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspCtrlInitialize - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_0\n"
    "tramp_0:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #0\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspCtrlInitialize\n"
    "shim_slot_NvIspCtrlInitialize: .word hook_0\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_0\n"
    "hook_0:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #0\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspFlush\n"
    ".type NvIspFlush, %function\n"
    "NvIspFlush:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspFlush - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_1\n"
    "tramp_1:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #1\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspFlush\n"
    "shim_slot_NvIspFlush: .word hook_1\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_1\n"
    "hook_1:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #1\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspCtrlCleanup\n"
    ".type NvIspCtrlCleanup, %function\n"
    "NvIspCtrlCleanup:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspCtrlCleanup - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_2\n"
    "tramp_2:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #2\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspCtrlCleanup\n"
    "shim_slot_NvIspCtrlCleanup: .word hook_2\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_2\n"
    "hook_2:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #2\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspClose\n"
    ".type NvIspClose, %function\n"
    "NvIspClose:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspClose - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_3\n"
    "tramp_3:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #3\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspClose\n"
    "shim_slot_NvIspClose: .word hook_3\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_3\n"
    "hook_3:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #3\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspOpen\n"
    ".type NvIspOpen, %function\n"
    "NvIspOpen:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspOpen - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_4\n"
    "tramp_4:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #4\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspOpen\n"
    "shim_slot_NvIspOpen: .word hook_4\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_4\n"
    "hook_4:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #4\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspProcessFrame\n"
    ".type NvIspProcessFrame, %function\n"
    "NvIspProcessFrame:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspProcessFrame - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_5\n"
    "tramp_5:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #5\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspProcessFrame\n"
    "shim_slot_NvIspProcessFrame: .word hook_5\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_5\n"
    "hook_5:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #5\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspSetAttribute\n"
    ".type NvIspSetAttribute, %function\n"
    "NvIspSetAttribute:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspSetAttribute - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_6\n"
    "tramp_6:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #6\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspSetAttribute\n"
    "shim_slot_NvIspSetAttribute: .word hook_6\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_6\n"
    "hook_6:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #6\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspGetAttribute\n"
    ".type NvIspGetAttribute, %function\n"
    "NvIspGetAttribute:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspGetAttribute - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_7\n"
    "tramp_7:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #7\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspGetAttribute\n"
    "shim_slot_NvIspGetAttribute: .word hook_7\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_7\n"
    "hook_7:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #7\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspGetStatus\n"
    ".type NvIspGetStatus, %function\n"
    "NvIspGetStatus:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspGetStatus - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_8\n"
    "tramp_8:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #8\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspGetStatus\n"
    "shim_slot_NvIspGetStatus: .word hook_8\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_8\n"
    "hook_8:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #8\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspSetMemoryBandwidth\n"
    ".type NvIspSetMemoryBandwidth, %function\n"
    "NvIspSetMemoryBandwidth:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspSetMemoryBandwidth - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_9\n"
    "tramp_9:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #9\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspSetMemoryBandwidth\n"
    "shim_slot_NvIspSetMemoryBandwidth: .word hook_9\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_9\n"
    "hook_9:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #9\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspUpdateEmcClock\n"
    ".type NvIspUpdateEmcClock, %function\n"
    "NvIspUpdateEmcClock:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspUpdateEmcClock - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_10\n"
    "tramp_10:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #10\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspUpdateEmcClock\n"
    "shim_slot_NvIspUpdateEmcClock: .word hook_10\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_10\n"
    "hook_10:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #10\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspSetIspClockRate\n"
    ".type NvIspSetIspClockRate, %function\n"
    "NvIspSetIspClockRate:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspSetIspClockRate - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_11\n"
    "tramp_11:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #11\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspSetIspClockRate\n"
    "shim_slot_NvIspSetIspClockRate: .word hook_11\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_11\n"
    "hook_11:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #11\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsDestroyClientHwSettingsList\n"
    ".type NvIspHwSettingsDestroyClientHwSettingsList, %function\n"
    "NvIspHwSettingsDestroyClientHwSettingsList:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsDestroyClientHwSettingsList - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_12\n"
    "tramp_12:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #12\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsDestroyClientHwSettingsList\n"
    "shim_slot_NvIspHwSettingsDestroyClientHwSettingsList: .word hook_12\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_12\n"
    "hook_12:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #12\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsDestroy\n"
    ".type NvIspHwSettingsDestroy, %function\n"
    "NvIspHwSettingsDestroy:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsDestroy - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_13\n"
    "tramp_13:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #13\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsDestroy\n"
    "shim_slot_NvIspHwSettingsDestroy: .word hook_13\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_13\n"
    "hook_13:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #13\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsCreate\n"
    ".type NvIspHwSettingsCreate, %function\n"
    "NvIspHwSettingsCreate:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsCreate - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_14\n"
    "tramp_14:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #14\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsCreate\n"
    "shim_slot_NvIspHwSettingsCreate: .word hook_14\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_14\n"
    "hook_14:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #14\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsClone\n"
    ".type NvIspHwSettingsClone, %function\n"
    "NvIspHwSettingsClone:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsClone - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_15\n"
    "tramp_15:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #15\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsClone\n"
    "shim_slot_NvIspHwSettingsClone: .word hook_15\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_15\n"
    "hook_15:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #15\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsSetAttribute\n"
    ".type NvIspHwSettingsSetAttribute, %function\n"
    "NvIspHwSettingsSetAttribute:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsSetAttribute - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_16\n"
    "tramp_16:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #16\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
 __asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsSetAttribute\n"
    "shim_slot_NvIspHwSettingsSetAttribute: .word hook_SetAttribute\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_SetAttribute\n"
    "hook_SetAttribute:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #16\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspGetConfiguration\n"
    ".type NvIspGetConfiguration, %function\n"
    "NvIspGetConfiguration:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspGetConfiguration - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_17\n"
    "tramp_17:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #17\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspGetConfiguration\n"
    "shim_slot_NvIspGetConfiguration: .word hook_17\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_17\n"
    "hook_17:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #17\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsGetAppliedSettings\n"
    ".type NvIspHwSettingsGetAppliedSettings, %function\n"
    "NvIspHwSettingsGetAppliedSettings:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsGetAppliedSettings - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_18\n"
    "tramp_18:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #18\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsGetAppliedSettings\n"
    "shim_slot_NvIspHwSettingsGetAppliedSettings: .word hook_18\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_18\n"
    "hook_18:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #18\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsGetAttribute\n"
    ".type NvIspHwSettingsGetAttribute, %function\n"
    "NvIspHwSettingsGetAttribute:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsGetAttribute - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_19\n"
    "tramp_19:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #19\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsGetAttribute\n"
    "shim_slot_NvIspHwSettingsGetAttribute: .word hook_19\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_19\n"
    "hook_19:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #19\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsApply\n"
    ".type NvIspHwSettingsApply, %function\n"
    "NvIspHwSettingsApply:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsApply - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_20\n"
    "tramp_20:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #20\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsApply\n"
    "shim_slot_NvIspHwSettingsApply: .word hook_20\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_20\n"
    "hook_20:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #20\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspSetConfiguration\n"
    ".type NvIspSetConfiguration, %function\n"
    "NvIspSetConfiguration:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspSetConfiguration - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_21\n"
    "tramp_21:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #21\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspSetConfiguration\n"
    "shim_slot_NvIspSetConfiguration: .word hook_21\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_21\n"
    "hook_21:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #21\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsCopyGpp\n"
    ".type NvIspHwSettingsCopyGpp, %function\n"
    "NvIspHwSettingsCopyGpp:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsCopyGpp - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_22\n"
    "tramp_22:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #22\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsCopyGpp\n"
    "shim_slot_NvIspHwSettingsCopyGpp: .word hook_22\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_22\n"
    "hook_22:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #22\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsCopyLensShading\n"
    ".type NvIspHwSettingsCopyLensShading, %function\n"
    "NvIspHwSettingsCopyLensShading:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsCopyLensShading - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_23\n"
    "tramp_23:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #23\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsCopyLensShading\n"
    "shim_slot_NvIspHwSettingsCopyLensShading: .word hook_23\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_23\n"
    "hook_23:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #23\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsCopyDemosaic\n"
    ".type NvIspHwSettingsCopyDemosaic, %function\n"
    "NvIspHwSettingsCopyDemosaic:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsCopyDemosaic - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_24\n"
    "tramp_24:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #24\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsCopyDemosaic\n"
    "shim_slot_NvIspHwSettingsCopyDemosaic: .word hook_24\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_24\n"
    "hook_24:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #24\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsCopyLumaEnhancement\n"
    ".type NvIspHwSettingsCopyLumaEnhancement, %function\n"
    "NvIspHwSettingsCopyLumaEnhancement:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsCopyLumaEnhancement - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_25\n"
    "tramp_25:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #25\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsCopyLumaEnhancement\n"
    "shim_slot_NvIspHwSettingsCopyLumaEnhancement: .word hook_25\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_25\n"
    "hook_25:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #25\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsCopyOutputDownScaler\n"
    ".type NvIspHwSettingsCopyOutputDownScaler, %function\n"
    "NvIspHwSettingsCopyOutputDownScaler:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsCopyOutputDownScaler - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_26\n"
    "tramp_26:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #26\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsCopyOutputDownScaler\n"
    "shim_slot_NvIspHwSettingsCopyOutputDownScaler: .word hook_26\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_26\n"
    "hook_26:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #26\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspHwSettingsCopyBitwiseOperation\n"
    ".type NvIspHwSettingsCopyBitwiseOperation, %function\n"
    "NvIspHwSettingsCopyBitwiseOperation:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspHwSettingsCopyBitwiseOperation - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_27\n"
    "tramp_27:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #27\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspHwSettingsCopyBitwiseOperation\n"
    "shim_slot_NvIspHwSettingsCopyBitwiseOperation: .word hook_27\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_27\n"
    "hook_27:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #27\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspGetStats\n"
    ".type NvIspGetStats, %function\n"
    "NvIspGetStats:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspGetStats - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_28\n"
    "tramp_28:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #28\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspGetStats\n"
    "shim_slot_NvIspGetStats: .word hook_28\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_28\n"
    "hook_28:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #28\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvIspSetStats\n"
    ".type NvIspSetStats, %function\n"
    "NvIspSetStats:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvIspSetStats - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_29\n"
    "tramp_29:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #29\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvIspSetStats\n"
    "shim_slot_NvIspSetStats: .word hook_29\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_29\n"
    "hook_29:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #29\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl PopulateIspHwFunctions_T12x\n"
    ".type PopulateIspHwFunctions_T12x, %function\n"
    "PopulateIspHwFunctions_T12x:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_PopulateIspHwFunctions_T12x - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_30\n"
    "tramp_30:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #30\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_PopulateIspHwFunctions_T12x\n"
    "shim_slot_PopulateIspHwFunctions_T12x: .word hook_30\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_30\n"
    "hook_30:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #30\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvSFxFloat2Fixed\n"
    ".type NvSFxFloat2Fixed, %function\n"
    "NvSFxFloat2Fixed:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvSFxFloat2Fixed - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_31\n"
    "tramp_31:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #31\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvSFxFloat2Fixed\n"
    "shim_slot_NvSFxFloat2Fixed: .word hook_31\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_31\n"
    "hook_31:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #31\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvSFxFixed2Float\n"
    ".type NvSFxFixed2Float, %function\n"
    "NvSFxFixed2Float:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvSFxFixed2Float - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_32\n"
    "tramp_32:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #32\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvSFxFixed2Float\n"
    "shim_slot_NvSFxFixed2Float: .word hook_32\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_32\n"
    "hook_32:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #32\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvCameraHwSettingsApply\n"
    ".type NvCameraHwSettingsApply, %function\n"
    "NvCameraHwSettingsApply:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvCameraHwSettingsApply - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_33\n"
    "tramp_33:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #33\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvCameraHwSettingsApply\n"
    "shim_slot_NvCameraHwSettingsApply: .word hook_33\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_33\n"
    "hook_33:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #33\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvCameraHwSettingsUpdateDirty\n"
    ".type NvCameraHwSettingsUpdateDirty, %function\n"
    "NvCameraHwSettingsUpdateDirty:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvCameraHwSettingsUpdateDirty - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_34\n"
    "tramp_34:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #34\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvCameraHwSettingsUpdateDirty\n"
    "shim_slot_NvCameraHwSettingsUpdateDirty: .word hook_34\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_34\n"
    "hook_34:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #34\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvCameraConvertDoubleToUFx\n"
    ".type NvCameraConvertDoubleToUFx, %function\n"
    "NvCameraConvertDoubleToUFx:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvCameraConvertDoubleToUFx - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_35\n"
    "tramp_35:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #35\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvCameraConvertDoubleToUFx\n"
    "shim_slot_NvCameraConvertDoubleToUFx: .word hook_35\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_35\n"
    "hook_35:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #35\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvCameraConvertDoubleToSFx\n"
    ".type NvCameraConvertDoubleToSFx, %function\n"
    "NvCameraConvertDoubleToSFx:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvCameraConvertDoubleToSFx - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_36\n"
    "tramp_36:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #36\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvCameraConvertDoubleToSFx\n"
    "shim_slot_NvCameraConvertDoubleToSFx: .word hook_36\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_36\n"
    "hook_36:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #36\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvCameraMatMult3x3\n"
    ".type NvCameraMatMult3x3, %function\n"
    "NvCameraMatMult3x3:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvCameraMatMult3x3 - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_37\n"
    "tramp_37:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #37\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvCameraMatMult3x3\n"
    "shim_slot_NvCameraMatMult3x3: .word hook_37\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_37\n"
    "hook_37:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #37\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvCameraGetBayerComponent\n"
    ".type NvCameraGetBayerComponent, %function\n"
    "NvCameraGetBayerComponent:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvCameraGetBayerComponent - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_38\n"
    "tramp_38:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #38\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvCameraGetBayerComponent\n"
    "shim_slot_NvCameraGetBayerComponent: .word hook_38\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_38\n"
    "hook_38:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #38\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl NvCameraConvertRGrGbBToTlTrBlBr\n"
    ".type NvCameraConvertRGrGbBToTlTrBlBr, %function\n"
    "NvCameraConvertRGrGbBToTlTrBlBr:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_NvCameraConvertRGrGbBToTlTrBlBr - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_39\n"
    "tramp_39:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #39\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_NvCameraConvertRGrGbBToTlTrBlBr\n"
    "shim_slot_NvCameraConvertRGrGbBToTlTrBlBr: .word hook_39\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_39\n"
    "hook_39:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #39\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".globl IsBayerColorFormat\n"
    ".type IsBayerColorFormat, %function\n"
    "IsBayerColorFormat:\n"
    "  ldr  r12, 9f\n"
    "1: add  r12, pc\n"
    "  ldr  r12, [r12]\n"
    "  bx   r12\n"
    "  .align 2\n"
    "9:  .word shim_slot_IsBayerColorFormat - (1b + 4)\n");
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden tramp_40\n"
    "tramp_40:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov  r0, #40\n"
    "  bl   shim_resolve\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(
    ".data\n"
    ".align 2\n"
    ".hidden shim_slot_IsBayerColorFormat\n"
    "shim_slot_IsBayerColorFormat: .word hook_40\n");
 __asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n"
    ".thumb_func\n"
    ".hidden hook_40\n"
    "hook_40:\n"
    "  push {r0-r3, r12, lr}\n"
    "  add  r1, sp, #0\n"
    "  mov  r0, #40\n"
    "  bl   shim_log_call\n"
    "  mov  r12, r0\n"
    "  pop  {r0-r3}\n"
    "  add  sp, #4\n"
    "  pop  {lr}\n"
    "  bx   r12\n");
__asm__(".text\n");

struct shim_binding {
    const char *name;
    void **slot;
};

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
