#ifndef LZH_COMPRESS_H
#define LZH_COMPRESS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Clean-room LZH compressor compatible with ar002/lh5 (public domain format).
// Produces output decompressible by unlzh.c / LZH_decompress.
//
// source:  uncompressed data
// dest:    buffer for compressed data (must be at least size + 65536)
// size:    number of bytes in source
// ultra:   reserved flag (stored in header byte)
// Returns: number of bytes written to dest
uint32_t a2m_lzh_compress(const uint8_t *source, uint8_t *dest, uint32_t size,
                          bool ultra);

// CRC-32 using polynomial 0xEDB88320 (compatible with AT2's Update32).
// init should be 0xFFFFFFFF for the first call.
uint32_t a2m_crc32(const void *buf, uint32_t len, uint32_t crc);

// Initialize CRC table (called automatically on first use).
void a2m_crc32_init(void);

#ifdef __cplusplus
}
#endif

#endif // LZH_COMPRESS_H
