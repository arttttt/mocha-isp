/*
 * isp_geom — the resolution-dependent configuration, measured off the two
 * stock captures (2592x1944 preview, 1280x720 video). The stock camera
 * refuses to work when these do not agree with the frame actually arriving,
 * so they are keyed to the exact size and never guessed. Where two points
 * prove a formula -- the frame-less-thirty-two word in 0x800/0x820, the
 * width in 0xc00 -- it is applied at the call site instead.
 */
#include "viisp.h"

static const struct geom_cfg geom_2592 = {
    .in_dims   = 0x00780078,
    .in_phase  = 0x00000000,
    .d20_mode  = 0x00001101,
    .d20_step  = 0x00210000,
    .s909_w4   = 0x00000030,
    .s910_w3   = 0x0003030b,
    .s910_w7   = 0x00000036,
    .s910_w8   = 0x00001f1f,
    .s91c_w5   = 0x00000025,
    .x700_w5   = 0x00001a40,
    .x700_w11  = 0x00001a00,
    .c00_w0    = 0x00007901,
    .c00_w2_hi = 0x00000103,
    .d00       = { 0x00ca4580, 0x006522c0, 0x00ca4580, 0x010db200,
                   0x0086d900, 0x010db200, 0x05100288, 0x03cc01e6 },
    .p400_w8   = 0x00280010,
    .p400_w9   = 0x0003003f,
    .p400_w10  = 0x001d002c,
    .p400_w11  = 0x0002003f,
};

static const struct geom_cfg geom_720 = {
    .in_dims   = 0x02000200,
    .in_phase  = 0x00003333,
    .d20_mode  = 0x00003101,
    .d20_step  = 0x01ec0000,
    .s909_w4   = 0x00000000,
    .s910_w3   = 0x00177e0b,
    .s910_w7   = 0x00000039,
    .s910_w8   = 0x00000000,
    .s91c_w5   = 0x00000026,
    .x700_w5   = 0x00001dc0,
    .x700_w11  = 0x00001c50,
    .c00_w0    = 0x00005a01,
    .c00_w2_hi = 0x00000082,
    .d00       = { 0x01999990, 0x00ccccc0, 0x01999990, 0x02222220,
                   0x01111110, 0x02222220, 0x02800140, 0x01de00ee },
    .p400_w8   = 0x00130020,
    .p400_w9   = 0x0002003f,
    .p400_w10  = 0x000a0028,
    .p400_w11  = 0x0001003f,
};

const struct geom_cfg *geom_for(unsigned W, unsigned H)
{
    (void)H;
    if (W == 2592) return &geom_2592;
    if (W == 1280) return &geom_720;
    /* Unmeasured size: the nearer neighbour, with a warning at the call
     * site. The stock camera refuses to run on a mismatched set. */
    return (W > 1920) ? &geom_2592 : &geom_720;
}
