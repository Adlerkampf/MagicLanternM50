/* ML/LiveView 14-bit packing (8 px in 7 little-endian u16, MSB-first)
 * -> Canon photo packing (little-endian LSB-first bitstream, 8 px = 14 bytes).
 * Rows must be a multiple of 8 px; works on the whole frame as one stream
 * when pitch == width * 14 / 8. */
static inline void repack_blocks_ml_to_canon(const uint16_t *in, uint8_t *out, uint32_t nblocks)
{
    while (nblocks--)
    {
        uint32_t h0 = in[0], h1 = in[1], h2 = in[2], h3 = in[3], h4 = in[4], h5 = in[5], h6 = in[6];
        in += 7;
        uint64_t p0 = h0 >> 2;
        uint64_t p1 = ((h0 & 3)    << 12) | (h1 >> 4);
        uint64_t p2 = ((h1 & 15)   << 10) | (h2 >> 6);
        uint64_t p3 = ((h2 & 63)   << 8)  | (h3 >> 8);
        uint64_t p4 = ((h3 & 255)  << 6)  | (h4 >> 10);
        uint64_t p5 = ((h4 & 1023) << 4)  | (h5 >> 12);
        uint64_t p6 = ((h5 & 4095) << 2)  | (h6 >> 14);
        uint64_t p7 = h6 & 16383;
        uint64_t lo = p0 | (p1 << 14) | (p2 << 28) | (p3 << 42) | (p4 << 56);
        uint64_t hi = (p4 >> 8) | (p5 << 6) | (p6 << 20) | (p7 << 34);
        uint32_t l0 = (uint32_t) lo, l1 = (uint32_t)(lo >> 32);
        uint32_t k0 = (uint32_t) hi, k1 = (uint32_t)(hi >> 32);
        /* out is only 2-byte aligned (14-byte blocks): halfword stores */
        uint16_t *o = (uint16_t *) out;
        o[0] = l0; o[1] = l0 >> 16; o[2] = l1; o[3] = l1 >> 16;
        o[4] = k0; o[5] = k0 >> 16; o[6] = k1;
        out += 14;
    }
}
