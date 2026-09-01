/*
 * isp_demosaic.h — the demosaic, exactly as the stock camera sends it.
 *
 * Taken word for word out of a captured opening sequence: the stock stack
 * pushes these six blocks in one job, and it does so at a particular
 * moment -- after the ISP has been enabled, after the routing from the
 * receiver is set up, and after the first frame has already gone through.
 * Not during the opening configuration, which is where we had been putting
 * them, and that difference is the whole point of the file.
 *
 * Captured with /proc/isp_trace/once on the front sensor; extracted from
 * traces/stock_front_gathers.txt at word 25870.
 */

static const uint32_t isp_dm_902[1] = {
    0x00000000,
};

static const uint32_t isp_dm_903[64] = {
    0x07ff0007, 0x07f707f4, 0x07f70037, 0x07e707df, 0x07f707f4, 0x005307f8,
    0x07b907a3, 0x00e607ea, 0x07f60037, 0x07b907a3, 0x07b901a9, 0x073b0701,
    0x07e707df, 0x00e707ea, 0x073b0701, 0x027e07c4, 0x07fd07fc, 0x07fc001a,
    0x07ed07e7, 0x07f40048, 0x001707fe, 0x077f0758, 0x00af07ef, 0x00620080,
    0x07ec07e7, 0x07de00c9, 0x076b073d, 0x07a2022e, 0x003f07fa, 0x0062007f,
    0x01e707d2, 0x058004be, 0x07fd07fc, 0x001707fe, 0x07ed07e7, 0x003f07fa,
    0x07fc001a, 0x077f0758, 0x07de00c9, 0x00620080, 0x07ec07e7, 0x00af07ef,
    0x076b073d, 0x01e707d2, 0x07f40048, 0x0062007f, 0x07a2022e, 0x058004be,
    0x000707ff, 0x07f707f4, 0x003007fc, 0x07e707df, 0x07f707f4, 0x07f0005f,
    0x07b907a3, 0x07d40108, 0x003007fb, 0x07b907a3, 0x017207dd, 0x073b0701,
    0x07e707df, 0x07d40108, 0x073b0701, 0x078402dc,
};

static const uint32_t isp_dm_904[2] = {
    0x00004444, 0x00000001,
};

static const uint32_t isp_dm_906[1] = {
    0x00000000,
};

static const uint32_t isp_dm_907[36] = {
    0x00000000, 0x000006e3, 0x00000004, 0x000006e3, 0x000000be, 0x0000036a,
    0x00000005, 0x0000036a, 0x00000032, 0x00000749, 0x00000001, 0x00000051,
    0x0000000e, 0x0000004c, 0x000000a2, 0x00000051, 0x0000000f, 0x000002c8,
    0x00000749, 0x0000000e, 0x00000051, 0x00000001, 0x0000004c, 0x0000000f,
    0x00000051, 0x000000a2, 0x000002c8, 0x00000004, 0x000006e3, 0x0000002f,
    0x000006e3, 0x00000011, 0x0000036a, 0x0000002e, 0x0000036a, 0x0000022e,
   
};

static const uint32_t isp_dm_908[1] = {
    0x00004334,
};
