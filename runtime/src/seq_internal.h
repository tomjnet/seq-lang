/* Internal declarations shared by the runtime sources. Not installed. */
#ifndef SEQ_INTERNAL_H_
#define SEQ_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Descriptor on which the driver reports step progress to the supervisor. */
#define SEQ_REPORT_FD 3
/* Descriptor on which the supervisor passes the input directory. */
#define SEQ_INPUT_FD 4

/* Returns nonzero if relpath is a nonempty relative path with no empty, ".",
 * or ".." component and no backslash. */
int seq_path_is_safe(const char* relpath);

/* Glyph for an ASCII character as seven rows of five bits (bit 4 is the
 * leftmost column). Characters outside 32..126 map to '?'. */
#define SEQ_FONT_WIDTH 5
#define SEQ_FONT_HEIGHT 7
const uint8_t* seq_font_glyph(unsigned char c);

/* Writes an 8-bit palette image as a PNG. pixels holds width * height
 * palette indexes; palette holds color_count RGB triples. Returns 0 on
 * success. */
int seq_png_write_indexed(FILE* out, int width, int height,
                          const uint8_t* pixels, const uint8_t* palette,
                          int color_count);

#endif /* SEQ_INTERNAL_H_ */
