#pragma once
// Minimal standalone shim so LibRaw's src/decoders/crx.cpp compiles directly
// with zero external dependencies and zero-copy in-memory bitstream reading.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <intrin.h>
#pragma warning(disable: 4244 4267 4018 4100 4146 4334)
#ifndef DWORD
typedef unsigned long DWORD;
#endif
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

typedef int64_t INT64;
typedef uint64_t UINT64;
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;

#ifndef LIBRAW_CRXTRACKS_MAXCOUNT
#define LIBRAW_CRXTRACKS_MAXCOUNT 16
#endif

enum LibRaw_exceptions
{
    LIBRAW_EXCEPTION_NONE = 0,
    LIBRAW_EXCEPTION_ALLOC = 1,
    LIBRAW_EXCEPTION_DECODE_RAW = 2,
    LIBRAW_EXCEPTION_IO_EOF = 4,
    LIBRAW_EXCEPTION_IO_CORRUPT = 5,
    LIBRAW_EXCEPTION_TOOBIG = 11
};

struct crx_data_header_t
{
    int32_t version;
    int32_t f_width;
    int32_t f_height;
    int32_t tileWidth;
    int32_t tileHeight;
    int32_t nBits;
    int32_t nPlanes;
    int32_t cfaLayout;
    int32_t encType;
    int32_t imageLevels;
    int32_t hasTileCols;
    int32_t hasTileRows;
    int32_t mdatHdrSize;
    int32_t medianBits;
    int32_t MediaSize;
    INT64 MediaOffset;
    uint32_t MediaType;
};

// Zero-copy in-memory datastream for lock-free multi-threaded CRX decoding
class LibRaw_abstract_datastream
{
public:
    const uint8_t *buf_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;

    LibRaw_abstract_datastream() = default;
    LibRaw_abstract_datastream(const uint8_t *buf, size_t size)
        : buf_(buf), size_(size), pos_(0) {}

    inline void lock() {}
    inline void unlock() {}

    inline int seek(INT64 offset, int whence)
    {
        INT64 target = 0;
        if (whence == SEEK_SET) target = offset;
        else if (whence == SEEK_CUR) target = (INT64)pos_ + offset;
        else if (whence == SEEK_END) target = (INT64)size_ + offset;
        if (target < 0) target = 0;
        if ((size_t)target > size_) target = (INT64)size_;
        pos_ = (size_t)target;
        return 0;
    }

    inline int read(void *dst, size_t elem_size, size_t count)
    {
        size_t bytes = elem_size * count;
        if (pos_ >= size_) return 0;
        size_t avail = size_ - pos_;
        if (bytes > avail) bytes = avail;
        if (bytes > 0)
        {
            memcpy(dst, buf_ + pos_, bytes);
            pos_ += bytes;
        }
        return (int)(bytes / (elem_size ? elem_size : 1));
    }
};

struct LibRaw
{
    struct
    {
        struct
        {
            LibRaw_abstract_datastream *input = nullptr;
        } internal_data;
        struct
        {
            int crx_track_selected = 0;
            crx_data_header_t crx_header[LIBRAW_CRXTRACKS_MAXCOUNT] = {};
            INT64 data_offset = 0;
            UINT64 data_size = 0;
        } unpacker_data;
    } libraw_internal_data;

    struct
    {
        struct
        {
            uint32_t maximum = 0;
        } color;
        struct
        {
            ushort raw_width = 0;
            ushort raw_height = 0;
        } sizes;
        struct
        {
            ushort *raw_image = nullptr;
        } rawdata;
        struct
        {
            unsigned max_raw_memory_mb = 2048;
        } rawparams;
    } imgdata;

    static inline unsigned sgetn(int n, const uchar *s)
    {
        unsigned result = 0;
        while (n-- > 0)
            result = (result << 8) | (*s++);
        return result;
    }

    inline void derror()
    {
        throw LIBRAW_EXCEPTION_IO_CORRUPT;
    }

    int crxDecodePlane(void *p, uint32_t planeNumber);
    void crxLoadDecodeLoop(void *img, int nPlanes);
    void crxConvertPlaneLineDf(void *p, int imageRow);
    void crxLoadFinalizeLoopE3(void *p, int planeHeight);
    void crxLoadRaw();
    int crxParseImageHeader(uchar *cmp1TagData, int nTrack, INT64 size);
};

// Public C++ helper implemented at the bottom of crx_decoder.cpp
struct CrxDecodedMeta
{
    int width = 0;
    int height = 0;
    int bits = 0;
    int cfaLayout = 0;
    int encType = 0;
};

bool decode_crx_frame(
    const uint8_t *cmp1_data, size_t cmp1_len,
    const uint8_t *mdat_data, size_t mdat_len,
    std::vector<uint16_t> &out_raw,
    CrxDecodedMeta &meta,
    std::string &err_out);
