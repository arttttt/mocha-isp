/*
 * bayer_gen.h -- the synthetic Bayer generator, shared by two builds:
 *   ispinit.c   (armv7, NDK, for the device run)
 *   bayer_test.c (host clang, the positive control -- bayer_test.sh)
 * Everything is static: one translation unit each, no link-time surface.
 */

/*
 * RAW10 sample packing: 10 bits, LSB-aligned in two bytes, little-endian.
 * THE parameterizable spot: a different packing changes only this
 * function, nothing else.
 */
static void bayer_put10(unsigned char *p, unsigned v)
{
    v &= 0x3ffu;
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0x03);
}

/*
 * Fill buf with a synthetic Bayer frame.
 *
 *   width, height  in pixels
 *   stride         bytes per row, >= width*bpp; the TAIL of each row
 *                  (width*bpp .. stride) is ZEROED -- predictable, and
 *                  it visually separates rows on the output image
 *   bpp            bytes per sample (2 for RAW10-as-u16, see bayer_put10)
 *   order          0=RGGB 1=BGGR 2=GRBG 3=GBRG
 *
 * Returns a u32 checksum (sum of all sample values). Permutation
 * invariant: the four position codes appear once per tile regardless of
 * order, so the checksum is the same for all four orders.
 *
 * The pattern: every 2x2 tile carries four DISTINCT position codes
 * (R=0x11, Gr=0x22, Gb=0x33, B=0x44), and the frame splits into four
 * brightness quadrants (+0x000/+0x100/+0x200/+0x300, quadrant =
 * qy*2+qx). Sixteen distinct values total, max 0x344 < 0x3ff: the
 * mosaic order reads straight off the output image, and a flip or a
 * mirror shows as an obvious quadrant swap -- unlike a checkerboard,
 * which only proves that something happened.
 */
static unsigned bayer_fill(unsigned char *buf, unsigned width, unsigned height,
                           unsigned stride, unsigned bpp, unsigned order)
{
    /* [row][col] within the 2x2 tile -> position (0=R 1=Gr 2=Gb 3=B) */
    static const unsigned char orders[4][4] = {
        {0, 1, 2, 3}, /* RGGB */
        {3, 2, 1, 0}, /* BGGR */
        {1, 0, 3, 2}, /* GRBG */
        {2, 3, 1, 0}, /* GBRG */
    };
    static const unsigned pos_code[4] = {0x11, 0x22, 0x33, 0x44};
    unsigned sum = 0;
    unsigned x, y;

    if (order > 3)
        order = 0;
    for (y = 0; y < height; y++) {
        unsigned char *row = buf + (unsigned)y * stride;
        for (x = 0; x < width; x++) {
            unsigned pos = orders[order][(y & 1) * 2 + (x & 1)];
            unsigned quadrant = ((y >= height / 2) << 1) | (x >= width / 2);
            unsigned v = pos_code[pos] + (quadrant << 8);
            if (bpp == 2)
                bayer_put10(row + x * 2, v);
            else if (bpp == 1)
                row[x] = (unsigned char)(v >> 2); /* fallback: top 8 bits */
            sum += v;
        }
        for (x = width * bpp; x < stride; x++)
            row[x] = 0;
    }
    return sum;
}

static const char order_names[4][5] = { "rggb", "bggr", "grbg", "gbrg" };
