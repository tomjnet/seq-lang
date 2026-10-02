/* Bar chart helper: draws into a palette canvas and writes it as a PNG. */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "seq_internal.h"
#include "seq_runtime.h"

enum {
  kWidth = 800,
  kHeight = 480,
  kPlotLeft = 90,
  kPlotRight = 770,
  kPlotTop = 64,
  kPlotBottom = 400,
  kMaxBars = 32,
  kTickCount = 5
};

enum {
  kColorBackground = 0,
  kColorText = 1,
  kColorAxis = 2,
  kColorGrid = 3,
  kColorFirstBar = 4,
  kBarColors = 6,
  kColorCount = kColorFirstBar + kBarColors
};

static const uint8_t kPalette[kColorCount * 3] = {
    0xFF, 0xFF, 0xFF, /* background */
    0x22, 0x22, 0x22, /* text */
    0x44, 0x44, 0x44, /* axis */
    0xDD, 0xDD, 0xDD, /* grid */
    0x4C, 0x78, 0xA8, 0xF5, 0x85, 0x18, 0x54, 0xA2, 0x4B,
    0xE4, 0x57, 0x56, 0x72, 0xB7, 0xB2, 0xB2, 0x79, 0xA2,
};

static void fill_rect(uint8_t* canvas, int x0, int y0, int x1, int y1,
                      uint8_t color) {
  int y;

  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > kWidth) x1 = kWidth;
  if (y1 > kHeight) y1 = kHeight;
  if (x0 >= x1) return;
  for (y = y0; y < y1; ++y) {
    memset(canvas + (size_t)y * kWidth + x0, color, (size_t)(x1 - x0));
  }
}

/* Number of glyphs drawn for a string: one per ASCII byte or UTF-8 lead
 * byte. Non-ASCII characters are drawn as '?'. */
static int text_glyphs(const char* text) {
  int count = 0;

  for (; *text != '\0'; ++text) {
    if (((unsigned char)*text & 0xC0) != 0x80) {
      ++count;
    }
  }
  return count;
}

static int text_width(int glyphs, int scale) {
  return glyphs > 0 ? glyphs * (SEQ_FONT_WIDTH + 1) * scale - scale : 0;
}

/* Draws at most max_glyphs glyphs with the top-left corner at (x, y). */
static void draw_text(uint8_t* canvas, int x, int y, const char* text,
                      int max_glyphs, int scale, uint8_t color) {
  int drawn = 0;

  for (; *text != '\0' && drawn < max_glyphs; ++text) {
    unsigned char c = (unsigned char)*text;
    const uint8_t* rows;
    int row;

    if ((c & 0xC0) == 0x80) {
      continue; /* continuation byte of a character already drawn as '?' */
    }
    rows = seq_font_glyph(c);
    for (row = 0; row < SEQ_FONT_HEIGHT; ++row) {
      int column;
      for (column = 0; column < SEQ_FONT_WIDTH; ++column) {
        if (rows[row] & (1U << (SEQ_FONT_WIDTH - 1 - column))) {
          int px = x + column * scale;
          int py = y + row * scale;
          fill_rect(canvas, px, py, px + scale, py + scale, color);
        }
      }
    }
    x += (SEQ_FONT_WIDTH + 1) * scale;
    ++drawn;
  }
}

/* Draws text centered on center_x, shrinking and then truncating it to fit
 * max_width pixels. */
static void draw_centered(uint8_t* canvas, int center_x, int y,
                          const char* text, int max_width, int scale,
                          uint8_t color) {
  int glyphs = text_glyphs(text);

  while (scale > 1 && text_width(glyphs, scale) > max_width) {
    --scale;
  }
  while (glyphs > 1 && text_width(glyphs, scale) > max_width) {
    --glyphs;
  }
  draw_text(canvas, center_x - text_width(glyphs, scale) / 2, y, text, glyphs,
            scale, color);
}

/* Rounds a positive tick spacing up to 1, 2, or 5 times a power of ten. */
static double nice_step(double raw) {
  double magnitude = pow(10.0, floor(log10(raw)));
  double fraction = raw / magnitude;

  if (fraction <= 1.0) return magnitude;
  if (fraction <= 2.0) return 2.0 * magnitude;
  if (fraction <= 5.0) return 5.0 * magnitude;
  return 10.0 * magnitude;
}

int seq_chart_bar_png(const char* relpath, const char* title,
                      const char* const* labels, const double* values,
                      int count) {
  uint8_t* canvas;
  FILE* out;
  double low = 0.0;
  double high = 0.0;
  double step;
  double axis_low;
  double axis_high;
  int plot_height = kPlotBottom - kPlotTop;
  int plot_width = kPlotRight - kPlotLeft;
  int slot;
  int bar_width;
  int zero_y;
  int result;
  int i;

  if (relpath == NULL || labels == NULL || values == NULL || count < 1 ||
      count > kMaxBars) {
    return 1;
  }
  for (i = 0; i < count; ++i) {
    if (!isfinite(values[i])) {
      return 1;
    }
    if (values[i] < low) low = values[i];
    if (values[i] > high) high = values[i];
  }
  if (high - low <= 0.0) {
    high = low + 1.0;
  }
  step = nice_step((high - low) / kTickCount);
  axis_low = floor(low / step) * step;
  axis_high = ceil(high / step) * step;
  if (axis_high - axis_low <= 0.0) {
    axis_high = axis_low + step;
  }

  canvas = (uint8_t*)malloc((size_t)kWidth * kHeight);
  if (canvas == NULL) {
    return 1;
  }
  memset(canvas, kColorBackground, (size_t)kWidth * kHeight);

#define SEQ_Y(v) \
  (kPlotBottom - \
   (int)lround(((v) - axis_low) / (axis_high - axis_low) * plot_height))

  if (title != NULL && title[0] != '\0') {
    draw_centered(canvas, kWidth / 2, 20, title, kWidth - 40, 3, kColorText);
  }

  /* Grid lines and value-axis labels. */
  {
    int ticks = (int)lround((axis_high - axis_low) / step);
    int t;
    for (t = 0; t <= ticks && t <= 4 * kTickCount; ++t) {
      double v = axis_low + t * step;
      char text[32];
      int y = SEQ_Y(v);
      int glyphs;

      if (fabs(v) < step * 1e-9) {
        v = 0.0;
      }
      fill_rect(canvas, kPlotLeft, y, kPlotRight, y + 1, kColorGrid);
      snprintf(text, sizeof(text), "%.6g", v);
      glyphs = text_glyphs(text);
      draw_text(canvas, kPlotLeft - 8 - text_width(glyphs, 1), y - 3, text,
                glyphs, 1, kColorText);
    }
  }

  slot = plot_width / count;
  bar_width = slot * 6 / 10;
  if (bar_width < 2) bar_width = 2;
  zero_y = SEQ_Y(0.0);

  for (i = 0; i < count; ++i) {
    int center = kPlotLeft + slot * i + slot / 2;
    int value_y = SEQ_Y(values[i]);
    int top = value_y < zero_y ? value_y : zero_y;
    int bottom = value_y < zero_y ? zero_y : value_y;
    char text[32];
    const char* label = labels[i] != NULL ? labels[i] : "";

    if (bottom == top) {
      bottom = top + 1;
    }
    fill_rect(canvas, center - bar_width / 2, top,
              center - bar_width / 2 + bar_width, bottom,
              (uint8_t)(kColorFirstBar + i % kBarColors));

    /* The value goes above a positive bar and below a negative one, unless
     * that would run into the category labels under the plot. */
    snprintf(text, sizeof(text), "%.6g", values[i]);
    {
      int value_label_y = values[i] >= 0.0 ? top - 18 : bottom + 4;
      if (value_label_y + 14 > kPlotBottom) {
        value_label_y = top - 18;
      }
      draw_centered(canvas, center, value_label_y, text, slot - 4, 2,
                    kColorText);
    }
    draw_centered(canvas, center, kPlotBottom + 12, label, slot - 4, 2,
                  kColorText);
  }

  /* Axes are drawn last so they stay on top of the bars. */
  fill_rect(canvas, kPlotLeft, zero_y, kPlotRight, zero_y + 2, kColorAxis);
  fill_rect(canvas, kPlotLeft - 1, kPlotTop, kPlotLeft + 1, kPlotBottom + 1,
            kColorAxis);

#undef SEQ_Y

  out = seq_output_open(relpath, "wb");
  if (out == NULL) {
    free(canvas);
    return 1;
  }
  result = seq_png_write_indexed(out, kWidth, kHeight, canvas, kPalette,
                                 kColorCount);
  if (fclose(out) != 0) {
    result = 1;
  }
  free(canvas);
  return result != 0;
}
