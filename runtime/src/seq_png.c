/* Minimal PNG encoder for 8-bit palette images.
 *
 * The image data is compressed with a single fixed-Huffman deflate block
 * whose only matches are runs of a repeated byte (distance 1). Charts are
 * mostly flat color, so this small encoder already compresses them well.
 * Written for this project; no third-party code. */

#include <stdlib.h>
#include <string.h>

#include "seq_internal.h"

typedef struct SeqBuffer {
  uint8_t* data;
  size_t size;
  size_t capacity;
  uint32_t bit_accumulator;
  int bit_count;
  int failed;
} SeqBuffer;

static void buffer_push(SeqBuffer* b, uint8_t byte) {
  if (b->failed) {
    return;
  }
  if (b->size == b->capacity) {
    size_t capacity = b->capacity == 0 ? 4096 : b->capacity * 2;
    uint8_t* data = (uint8_t*)realloc(b->data, capacity);
    if (data == NULL) {
      b->failed = 1;
      return;
    }
    b->data = data;
    b->capacity = capacity;
  }
  b->data[b->size++] = byte;
}

/* Deflate packs bits starting at the least significant bit of each byte. */
static void put_bits(SeqBuffer* b, uint32_t value, int count) {
  b->bit_accumulator |= value << b->bit_count;
  b->bit_count += count;
  while (b->bit_count >= 8) {
    buffer_push(b, (uint8_t)(b->bit_accumulator & 0xFF));
    b->bit_accumulator >>= 8;
    b->bit_count -= 8;
  }
}

/* Huffman codes are packed most significant bit first. */
static void put_code(SeqBuffer* b, uint32_t code, int length) {
  uint32_t reversed = 0;
  int i;

  for (i = 0; i < length; ++i) {
    reversed = (reversed << 1) | ((code >> i) & 1U);
  }
  put_bits(b, reversed, length);
}

/* Emits a literal/length symbol (0..287) with the fixed Huffman code. */
static void put_symbol(SeqBuffer* b, int symbol) {
  if (symbol <= 143) {
    put_code(b, 0x30U + (uint32_t)symbol, 8);
  } else if (symbol <= 255) {
    put_code(b, 0x190U + (uint32_t)(symbol - 144), 9);
  } else if (symbol <= 279) {
    put_code(b, (uint32_t)(symbol - 256), 7);
  } else {
    put_code(b, 0xC0U + (uint32_t)(symbol - 280), 8);
  }
}

/* Emits a match of `length` (3..258) bytes at distance 1. */
static void put_run(SeqBuffer* b, int length) {
  static const int kBase[29] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const int kExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  int index = 28;

  while (kBase[index] > length) {
    --index;
  }
  put_symbol(b, 257 + index);
  if (kExtra[index] > 0) {
    put_bits(b, (uint32_t)(length - kBase[index]), kExtra[index]);
  }
  put_code(b, 0, 5); /* distance code 0: distance 1, no extra bits */
}

static void deflate_fixed(SeqBuffer* b, const uint8_t* data, size_t size) {
  size_t i = 0;

  put_bits(b, 1, 1); /* final block */
  put_bits(b, 1, 2); /* fixed Huffman codes */
  while (i < size) {
    size_t run = 0;
    if (i > 0) {
      while (i + run < size && run < 258 && data[i + run] == data[i - 1]) {
        ++run;
      }
    }
    if (run >= 3) {
      put_run(b, (int)run);
      i += run;
    } else {
      put_symbol(b, data[i]);
      ++i;
    }
  }
  put_symbol(b, 256); /* end of block */
  if (b->bit_count > 0) {
    put_bits(b, 0, 8 - b->bit_count);
  }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t size) {
  static uint32_t table[256];
  static int table_ready = 0;
  size_t i;

  if (!table_ready) {
    uint32_t n;
    for (n = 0; n < 256; ++n) {
      uint32_t c = n;
      int k;
      for (k = 0; k < 8; ++k) {
        c = (c & 1U) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
      }
      table[n] = c;
    }
    table_ready = 1;
  }
  for (i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

static uint32_t adler32(const uint8_t* data, size_t size) {
  uint32_t a = 1;
  uint32_t b = 0;
  size_t i;

  for (i = 0; i < size; ++i) {
    a = (a + data[i]) % 65521U;
    b = (b + a) % 65521U;
  }
  return (b << 16) | a;
}

static void store_be32(uint8_t* out, uint32_t value) {
  out[0] = (uint8_t)(value >> 24);
  out[1] = (uint8_t)(value >> 16);
  out[2] = (uint8_t)(value >> 8);
  out[3] = (uint8_t)value;
}

static int write_chunk(FILE* out, const char* type, const uint8_t* data,
                       size_t size) {
  uint8_t header[8];
  uint8_t trailer[4];
  uint32_t crc;

  store_be32(header, (uint32_t)size);
  memcpy(header + 4, type, 4);
  crc = crc32_update(0xFFFFFFFFU, header + 4, 4);
  crc = crc32_update(crc, data, size);
  store_be32(trailer, crc ^ 0xFFFFFFFFU);
  if (fwrite(header, 1, 8, out) != 8) {
    return -1;
  }
  if (size > 0 && fwrite(data, 1, size, out) != size) {
    return -1;
  }
  if (fwrite(trailer, 1, 4, out) != 4) {
    return -1;
  }
  return 0;
}

int seq_png_write_indexed(FILE* out, int width, int height,
                          const uint8_t* pixels, const uint8_t* palette,
                          int color_count) {
  static const uint8_t kSignature[8] = {0x89, 'P',  'N',  'G',
                                        '\r', '\n', 0x1A, '\n'};
  uint8_t header[13];
  uint8_t checksum[4];
  uint8_t* raw;
  size_t raw_size;
  SeqBuffer zlib;
  int y;
  int result = -1;

  if (out == NULL || pixels == NULL || palette == NULL || width <= 0 ||
      height <= 0 || width > 8192 || height > 8192 || color_count < 1 ||
      color_count > 256) {
    return -1;
  }

  /* Each scanline is prefixed with filter type 0 (none). */
  raw_size = ((size_t)width + 1) * (size_t)height;
  raw = (uint8_t*)malloc(raw_size);
  if (raw == NULL) {
    return -1;
  }
  for (y = 0; y < height; ++y) {
    uint8_t* row = raw + (size_t)y * ((size_t)width + 1);
    row[0] = 0;
    memcpy(row + 1, pixels + (size_t)y * (size_t)width, (size_t)width);
  }

  memset(&zlib, 0, sizeof(zlib));
  buffer_push(&zlib, 0x78); /* deflate, 32 KiB window */
  buffer_push(&zlib, 0x01);
  deflate_fixed(&zlib, raw, raw_size);
  store_be32(checksum, adler32(raw, raw_size));
  buffer_push(&zlib, checksum[0]);
  buffer_push(&zlib, checksum[1]);
  buffer_push(&zlib, checksum[2]);
  buffer_push(&zlib, checksum[3]);
  free(raw);

  if (!zlib.failed) {
    store_be32(header, (uint32_t)width);
    store_be32(header + 4, (uint32_t)height);
    header[8] = 8;  /* bit depth */
    header[9] = 3;  /* color type: palette */
    header[10] = 0; /* compression */
    header[11] = 0; /* filter */
    header[12] = 0; /* no interlace */
    if (fwrite(kSignature, 1, 8, out) == 8 &&
        write_chunk(out, "IHDR", header, sizeof(header)) == 0 &&
        write_chunk(out, "PLTE", palette, (size_t)color_count * 3) == 0 &&
        write_chunk(out, "IDAT", zlib.data, zlib.size) == 0 &&
        write_chunk(out, "IEND", NULL, 0) == 0) {
      result = 0;
    }
  }
  free(zlib.data);
  return result;
}
