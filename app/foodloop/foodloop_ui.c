/*
 * FoodLoop's on-device interface for the ESP32-S3-EYE 240x240 LCD.
 *
 * The renderer uses the NuttX LCD character device directly so it remains
 * lightweight and leaves the camera and network memory budget available for
 * an explicit scan.
 */

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/lcd/lcd_dev.h>

#include "foodloop_ui.h"

#define FOODLOOP_LCD_DEVICE "/dev/lcd0"
#define FOODLOOP_WIDTH      240
#define FOODLOOP_HEIGHT     240
#define FOODLOOP_PREVIEW_X  32
#define FOODLOOP_PREVIEW_Y  50
#define FOODLOOP_PREVIEW_WIDTH  176
#define FOODLOOP_PREVIEW_HEIGHT 132

#define RGB565(r, g, b) \
  ((uint16_t)((((uint16_t)(r) & 0xf8) << 8) | \
              (((uint16_t)(g) & 0xfc) << 3) | \
              ((uint16_t)(b) >> 3)))

#define COLOR_BACKGROUND ((uint16_t)0x0000)
#define COLOR_SURFACE    RGB565(15, 25, 20)
#define COLOR_INK        RGB565(235, 247, 239)
#define COLOR_MUTED      RGB565(128, 153, 139)
#define COLOR_GREEN      RGB565(72, 211, 158)
#define COLOR_GREEN_SOFT RGB565(20, 58, 42)
#define COLOR_ORANGE     RGB565(249, 143, 92)
#define COLOR_YELLOW     RGB565(245, 202, 80)
#define COLOR_RED        RGB565(238, 86, 75)

struct foodloop_canvas_s
{
  uint16_t *pixels;
};

/* 5x7 uppercase display font. Bit zero is the top pixel of a column. */

static const char g_charset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-:/.";

static const uint8_t g_font[][5] =
{
  {0x00, 0x00, 0x00, 0x00, 0x00}, /* Space */
  {0x7e, 0x11, 0x11, 0x11, 0x7e}, /* A */
  {0x7f, 0x49, 0x49, 0x49, 0x36}, /* B */
  {0x3e, 0x41, 0x41, 0x41, 0x22}, /* C */
  {0x7f, 0x41, 0x41, 0x22, 0x1c}, /* D */
  {0x7f, 0x49, 0x49, 0x49, 0x41}, /* E */
  {0x7f, 0x09, 0x09, 0x09, 0x01}, /* F */
  {0x3e, 0x41, 0x49, 0x49, 0x7a}, /* G */
  {0x7f, 0x08, 0x08, 0x08, 0x7f}, /* H */
  {0x00, 0x41, 0x7f, 0x41, 0x00}, /* I */
  {0x20, 0x40, 0x41, 0x3f, 0x01}, /* J */
  {0x7f, 0x08, 0x14, 0x22, 0x41}, /* K */
  {0x7f, 0x40, 0x40, 0x40, 0x40}, /* L */
  {0x7f, 0x02, 0x0c, 0x02, 0x7f}, /* M */
  {0x7f, 0x04, 0x08, 0x10, 0x7f}, /* N */
  {0x3e, 0x41, 0x41, 0x41, 0x3e}, /* O */
  {0x7f, 0x09, 0x09, 0x09, 0x06}, /* P */
  {0x3e, 0x41, 0x51, 0x21, 0x5e}, /* Q */
  {0x7f, 0x09, 0x19, 0x29, 0x46}, /* R */
  {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
  {0x01, 0x01, 0x7f, 0x01, 0x01}, /* T */
  {0x3f, 0x40, 0x40, 0x40, 0x3f}, /* U */
  {0x1f, 0x20, 0x40, 0x20, 0x1f}, /* V */
  {0x7f, 0x20, 0x18, 0x20, 0x7f}, /* W */
  {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
  {0x03, 0x04, 0x78, 0x04, 0x03}, /* Y */
  {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
  {0x3e, 0x51, 0x49, 0x45, 0x3e}, /* 0 */
  {0x00, 0x42, 0x7f, 0x40, 0x00}, /* 1 */
  {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
  {0x21, 0x41, 0x45, 0x4b, 0x31}, /* 3 */
  {0x18, 0x14, 0x12, 0x7f, 0x10}, /* 4 */
  {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
  {0x3c, 0x4a, 0x49, 0x49, 0x30}, /* 6 */
  {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
  {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
  {0x06, 0x49, 0x49, 0x29, 0x1e}, /* 9 */
  {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
  {0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
  {0x40, 0x20, 0x10, 0x08, 0x04}, /* / */
  {0x00, 0x60, 0x60, 0x00, 0x00}  /* . */
};

static void foodloop_ui_pixel(struct foodloop_canvas_s *canvas, int x, int y,
                              uint16_t color)
{
  if (x >= 0 && x < FOODLOOP_WIDTH && y >= 0 && y < FOODLOOP_HEIGHT)
    {
      canvas->pixels[y * FOODLOOP_WIDTH + x] = color;
    }
}

static void foodloop_ui_fill(struct foodloop_canvas_s *canvas, uint16_t color)
{
  int index;

  for (index = 0; index < FOODLOOP_WIDTH * FOODLOOP_HEIGHT; index++)
    {
      canvas->pixels[index] = color;
    }
}

static void foodloop_ui_rect(struct foodloop_canvas_s *canvas, int x, int y,
                             int width, int height, uint16_t color)
{
  int end_x = x + width;
  int end_y = y + height;
  int row;
  int col;

  if (x < 0)
    {
      x = 0;
    }

  if (y < 0)
    {
      y = 0;
    }

  if (end_x > FOODLOOP_WIDTH)
    {
      end_x = FOODLOOP_WIDTH;
    }

  if (end_y > FOODLOOP_HEIGHT)
    {
      end_y = FOODLOOP_HEIGHT;
    }

  for (row = y; row < end_y; row++)
    {
      for (col = x; col < end_x; col++)
        {
          canvas->pixels[row * FOODLOOP_WIDTH + col] = color;
        }
    }
}

static void foodloop_ui_round_rect(struct foodloop_canvas_s *canvas, int x,
                                   int y, int width, int height, int radius,
                                   uint16_t color)
{
  int row;
  int col;
  int end_x = x + width;
  int end_y = y + height;

  for (row = y; row < end_y; row++)
    {
      for (col = x; col < end_x; col++)
        {
          int dx = 0;
          int dy = 0;

          if (col < x + radius)
            {
              dx = x + radius - col;
            }
          else if (col >= end_x - radius)
            {
              dx = col - (end_x - radius - 1);
            }

          if (row < y + radius)
            {
              dy = y + radius - row;
            }
          else if (row >= end_y - radius)
            {
              dy = row - (end_y - radius - 1);
            }

          if (dx == 0 || dy == 0 || dx * dx + dy * dy <= radius * radius)
            {
              foodloop_ui_pixel(canvas, col, row, color);
            }
        }
    }
}

static void foodloop_ui_circle(struct foodloop_canvas_s *canvas, int center_x,
                               int center_y, int radius, uint16_t color)
{
  int row;
  int col;
  int radius_squared = radius * radius;

  for (row = center_y - radius; row <= center_y + radius; row++)
    {
      for (col = center_x - radius; col <= center_x + radius; col++)
        {
          int dx = col - center_x;
          int dy = row - center_y;

          if (dx * dx + dy * dy <= radius_squared)
            {
              foodloop_ui_pixel(canvas, col, row, color);
            }
        }
    }
}

static const uint8_t *foodloop_ui_glyph(char character)
{
  const char *found = strchr(g_charset, character);

  if (found == NULL)
    {
      found = g_charset;
    }

  return g_font[found - g_charset];
}

static int foodloop_ui_text_width(const char *text, int scale)
{
  return (int)strlen(text) * 6 * scale - scale;
}

static void foodloop_ui_text(struct foodloop_canvas_s *canvas, int x, int y,
                             const char *text, int scale, uint16_t color)
{
  int character_index;

  for (character_index = 0; text[character_index] != '\0'; character_index++)
    {
      const uint8_t *glyph = foodloop_ui_glyph(text[character_index]);
      int column;
      int row;

      for (column = 0; column < 5; column++)
        {
          for (row = 0; row < 7; row++)
            {
              if ((glyph[column] & (1 << row)) != 0)
                {
                  foodloop_ui_rect(canvas, x + column * scale,
                                   y + row * scale, scale, scale, color);
                }
            }
        }

      x += 6 * scale;
    }
}

static void foodloop_ui_center_text(struct foodloop_canvas_s *canvas, int y,
                                    const char *text, int scale,
                                    uint16_t color)
{
  foodloop_ui_text(canvas, (FOODLOOP_WIDTH - foodloop_ui_text_width(text,
                    scale)) / 2, y, text, scale, color);
}

static void foodloop_ui_camera_icon(struct foodloop_canvas_s *canvas, int x,
                                    int y, uint16_t outline,
                                    uint16_t lens)
{
  foodloop_ui_round_rect(canvas, x, y + 9, 64, 44, 9, outline);
  foodloop_ui_round_rect(canvas, x + 18, y, 28, 14, 4, outline);
  foodloop_ui_circle(canvas, x + 32, y + 31, 13, lens);
  foodloop_ui_circle(canvas, x + 32, y + 31, 7, outline);
}

static void foodloop_ui_check(struct foodloop_canvas_s *canvas, int x, int y,
                              uint16_t color)
{
  int offset;

  for (offset = 0; offset < 18; offset++)
    {
      foodloop_ui_rect(canvas, x + offset, y + 10 + offset / 2, 5, 5,
                       color);
    }

  for (offset = 0; offset < 30; offset++)
    {
      foodloop_ui_rect(canvas, x + 15 + offset, y + 19 - offset, 5, 5,
                       color);
    }
}

static void foodloop_ui_header(struct foodloop_canvas_s *canvas)
{
  foodloop_ui_text(canvas, 18, 19, "FOODLOOP", 2, COLOR_GREEN);
  foodloop_ui_rect(canvas, 18, 39, 30, 2, COLOR_GREEN);
  foodloop_ui_text(canvas, 159, 23, "PRIVATE", 1, COLOR_MUTED);
  foodloop_ui_circle(canvas, 215, 27, 4, COLOR_GREEN);
}

static void foodloop_ui_footer(struct foodloop_canvas_s *canvas,
                               const char *label, uint16_t color)
{
  foodloop_ui_round_rect(canvas, 20, 203, 200, 24, 12, color);
  foodloop_ui_center_text(canvas, 208, label, 2, COLOR_INK);
}

static void foodloop_ui_draw_home(struct foodloop_canvas_s *canvas,
                                  bool uses_mimo)
{
  foodloop_ui_fill(canvas, COLOR_BACKGROUND);
  foodloop_ui_header(canvas);
  foodloop_ui_round_rect(canvas, 20, 63, 200, 121, 24, COLOR_SURFACE);
  foodloop_ui_circle(canvas, 120, 111, 44, COLOR_GREEN_SOFT);
  foodloop_ui_camera_icon(canvas, 88, 82, COLOR_GREEN, COLOR_INK);
  foodloop_ui_center_text(canvas, 149, "PREVIEW FOOD", 2, COLOR_INK);
  foodloop_ui_center_text(canvas, 171,
                          uses_mimo ? "PRIVATE PREVIEW" : "ONE PHOTO ONLY",
                          1, COLOR_MUTED);
  foodloop_ui_footer(canvas, "BOOT TO PREVIEW", COLOR_GREEN_SOFT);
}

static void foodloop_ui_draw_scanning(struct foodloop_canvas_s *canvas)
{
  foodloop_ui_fill(canvas, COLOR_BACKGROUND);
  foodloop_ui_header(canvas);
  foodloop_ui_circle(canvas, 120, 113, 48, COLOR_GREEN_SOFT);
  foodloop_ui_circle(canvas, 120, 113, 39, COLOR_SURFACE);
  foodloop_ui_camera_icon(canvas, 88, 84, COLOR_GREEN, COLOR_ORANGE);
  foodloop_ui_center_text(canvas, 174, "CAPTURING", 2, COLOR_INK);
  foodloop_ui_center_text(canvas, 198, "ONE PHOTO", 1, COLOR_MUTED);
}

static void foodloop_ui_draw_analyzing(struct foodloop_canvas_s *canvas)
{
  foodloop_ui_fill(canvas, COLOR_BACKGROUND);
  foodloop_ui_header(canvas);
  foodloop_ui_circle(canvas, 120, 113, 48, COLOR_GREEN_SOFT);
  foodloop_ui_circle(canvas, 120, 113, 39, COLOR_SURFACE);
  foodloop_ui_center_text(canvas, 99, "MIMO", 3, COLOR_YELLOW);
  foodloop_ui_center_text(canvas, 174, "ANALYZING", 2, COLOR_INK);
  foodloop_ui_center_text(canvas, 198, "BUILDING DRAFT", 1, COLOR_MUTED);
}

static void foodloop_ui_draw_preview_frame(struct foodloop_canvas_s *canvas)
{
  foodloop_ui_fill(canvas, COLOR_BACKGROUND);
  foodloop_ui_header(canvas);
  foodloop_ui_rect(canvas, FOODLOOP_PREVIEW_X - 4,
                   FOODLOOP_PREVIEW_Y - 4,
                   FOODLOOP_PREVIEW_WIDTH + 8,
                   FOODLOOP_PREVIEW_HEIGHT + 8, COLOR_GREEN_SOFT);
  foodloop_ui_footer(canvas, "BOOT TO ANALYZE", COLOR_GREEN_SOFT);
}

static void foodloop_ui_draw_captured(struct foodloop_canvas_s *canvas)
{
  foodloop_ui_fill(canvas, COLOR_BACKGROUND);
  foodloop_ui_header(canvas);
  foodloop_ui_circle(canvas, 120, 113, 48, COLOR_GREEN_SOFT);
  foodloop_ui_check(canvas, 92, 92, COLOR_GREEN);
  foodloop_ui_center_text(canvas, 174, "PHOTO READY", 2, COLOR_INK);
  foodloop_ui_footer(canvas, "BOOT TO RESCAN", COLOR_GREEN_SOFT);
}

static void foodloop_ui_draw_draft(struct foodloop_canvas_s *canvas)
{
  foodloop_ui_fill(canvas, COLOR_BACKGROUND);
  foodloop_ui_header(canvas);
  foodloop_ui_circle(canvas, 120, 113, 48, COLOR_GREEN_SOFT);
  foodloop_ui_check(canvas, 92, 92, COLOR_GREEN);
  foodloop_ui_center_text(canvas, 174, "DRAFT READY", 2, COLOR_INK);
  foodloop_ui_center_text(canvas, 198, "NOT SAVED YET", 1, COLOR_MUTED);
  foodloop_ui_footer(canvas, "BOOT TO CONFIRM", COLOR_GREEN_SOFT);
}

static void foodloop_ui_draw_confirmed(struct foodloop_canvas_s *canvas)
{
  foodloop_ui_fill(canvas, COLOR_BACKGROUND);
  foodloop_ui_header(canvas);
  foodloop_ui_circle(canvas, 120, 113, 48, COLOR_GREEN_SOFT);
  foodloop_ui_check(canvas, 92, 92, COLOR_GREEN);
  foodloop_ui_center_text(canvas, 174, "RECORD SAVED", 2, COLOR_INK);
  foodloop_ui_center_text(canvas, 198, "CONFIRMED", 1, COLOR_MUTED);
  foodloop_ui_footer(canvas, "BOOT TO RESCAN", COLOR_GREEN_SOFT);
}

static void foodloop_ui_draw_error(struct foodloop_canvas_s *canvas)
{
  foodloop_ui_fill(canvas, COLOR_BACKGROUND);
  foodloop_ui_header(canvas);
  foodloop_ui_circle(canvas, 120, 113, 48, COLOR_RED);
  foodloop_ui_text(canvas, 111, 86, "!", 5, COLOR_INK);
  foodloop_ui_center_text(canvas, 174, "SCAN FAILED", 2, COLOR_INK);
  foodloop_ui_footer(canvas, "BOOT TO RETRY", COLOR_SURFACE);
}

static void foodloop_ui_draw(struct foodloop_canvas_s *canvas,
                             enum foodloop_ui_state_e state,
                             bool uses_mimo)
{
  switch (state)
    {
      case FOODLOOP_UI_SCANNING:
        foodloop_ui_draw_scanning(canvas);
        break;

      case FOODLOOP_UI_ANALYZING:
        foodloop_ui_draw_analyzing(canvas);
        break;

      case FOODLOOP_UI_PREVIEW:
        foodloop_ui_draw_preview_frame(canvas);
        break;

      case FOODLOOP_UI_CAPTURED:
        foodloop_ui_draw_captured(canvas);
        break;

      case FOODLOOP_UI_DRAFT_READY:
        foodloop_ui_draw_draft(canvas);
        break;

      case FOODLOOP_UI_CONFIRMED:
        foodloop_ui_draw_confirmed(canvas);
        break;

      case FOODLOOP_UI_ERROR:
        foodloop_ui_draw_error(canvas);
        break;

      case FOODLOOP_UI_HOME:
      default:
        foodloop_ui_draw_home(canvas, uses_mimo);
        break;
    }
}

static void foodloop_ui_draw_demo_badge(struct foodloop_canvas_s *canvas)
{
  foodloop_ui_round_rect(canvas, 174, 5, 52, 14, 7, COLOR_ORANGE);
  foodloop_ui_text(canvas, 182, 9, "DEMO", 1, COLOR_BACKGROUND);
}

static int foodloop_ui_present(enum foodloop_ui_state_e state, bool uses_mimo,
                               bool demo)
{
  struct foodloop_canvas_s canvas;
  struct lcddev_area_s area;
  int fd;
  int ret;

  canvas.pixels = malloc(FOODLOOP_WIDTH * FOODLOOP_HEIGHT *
                         sizeof(*canvas.pixels));
  if (canvas.pixels == NULL)
    {
      return -ENOMEM;
    }

  foodloop_ui_draw(&canvas, state, uses_mimo);
  if (demo)
    {
      foodloop_ui_draw_demo_badge(&canvas);
    }

  fd = open(FOODLOOP_LCD_DEVICE, O_RDWR);
  if (fd < 0)
    {
      ret = -errno;
      goto cleanup;
    }

  ioctl(fd, LCDDEVIO_SETPOWER, 1);
  memset(&area, 0, sizeof(area));
  area.row_start = 0;
  area.row_end = FOODLOOP_HEIGHT - 1;
  area.col_start = 0;
  area.col_end = FOODLOOP_WIDTH - 1;
  area.stride = FOODLOOP_WIDTH * sizeof(*canvas.pixels);
  area.data = (uint8_t *)canvas.pixels;

  ret = ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)(uintptr_t)&area);
  if (ret < 0)
    {
      ret = -errno;
    }

  close(fd);

cleanup:
  free(canvas.pixels);
  return ret;
}

int foodloop_ui_show(enum foodloop_ui_state_e state, bool uses_mimo)
{
  return foodloop_ui_present(state, uses_mimo, false);
}

int foodloop_ui_show_demo(enum foodloop_ui_state_e state)
{
  return foodloop_ui_present(state, true, true);
}

int foodloop_ui_show_preview(uint16_t *frame, int width, int height)
{
  struct lcddev_area_s area;
  int fd;
  int ret;
  int x;
  int y;

  if (frame == NULL || width <= 0 || height <= 0)
    {
      return -EINVAL;
    }

  /* Draw the stable chrome first, then upload a small photo region. Keeping
   * the thumbnail in the capture buffer avoids allocating a second full LCD
   * canvas while the camera frame is still resident.
   */

  ret = foodloop_ui_show(FOODLOOP_UI_PREVIEW, true);
  if (ret < 0)
    {
      return ret;
    }

  for (y = 0; y < FOODLOOP_PREVIEW_HEIGHT; y++)
    {
      int source_y = y * height / FOODLOOP_PREVIEW_HEIGHT;

      for (x = 0; x < FOODLOOP_PREVIEW_WIDTH; x++)
        {
          int source_x = x * width / FOODLOOP_PREVIEW_WIDTH;
          frame[y * FOODLOOP_PREVIEW_WIDTH + x] =
              frame[source_y * width + source_x];
        }
    }

  fd = open(FOODLOOP_LCD_DEVICE, O_RDWR);
  if (fd < 0)
    {
      return -errno;
    }

  ioctl(fd, LCDDEVIO_SETPOWER, 1);
  memset(&area, 0, sizeof(area));
  area.row_start = FOODLOOP_PREVIEW_Y;
  area.row_end = FOODLOOP_PREVIEW_Y + FOODLOOP_PREVIEW_HEIGHT - 1;
  area.col_start = FOODLOOP_PREVIEW_X;
  area.col_end = FOODLOOP_PREVIEW_X + FOODLOOP_PREVIEW_WIDTH - 1;
  area.stride = FOODLOOP_PREVIEW_WIDTH * sizeof(*frame);
  area.data = (uint8_t *)frame;

  ret = ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)(uintptr_t)&area);
  if (ret < 0)
    {
      ret = -errno;
    }

  close(fd);
  return ret;
}

/****************************************************************************
 * Full-screen viewfinder: scale a 320x240 RGB565 frame into the LCD canvas
 * directly and write the whole 240x240 area in one PUTAREA call.  This
 * avoids the row-by-row emulated path that made the preview stutter.
 *
 * The canvas and LCD handle are kept across calls (per-frame malloc/free
 * and open/close of a 115 KB buffer and a device fd were the main
 * frame-rate killers).  Only one live viewfinder runs at a time, so the
 * cached state is safe.
 ****************************************************************************/

static FAR uint16_t *g_live_canvas;
static int g_live_lcd_fd = -1;
static int g_live_sx[FOODLOOP_WIDTH];
static bool g_live_sx_ready;

int foodloop_ui_show_live_frame(uint16_t *frame, int width, int height)
{
  struct lcddev_area_s area;
  int fd = -1;
  int ret;
  int x;
  int y;

  if (frame == NULL || width <= 0 || height <= 0)
    {
      return -EINVAL;
    }

  /* Cache the canvas buffer across frames. */

  if (g_live_canvas == NULL)
    {
      g_live_canvas = malloc(FOODLOOP_WIDTH * FOODLOOP_HEIGHT *
                             sizeof(*g_live_canvas));
      if (g_live_canvas == NULL)
        {
          return -ENOMEM;
        }
    }

  /* Precompute the horizontal source mapping once (240/320 = 3/4).  The
   * vertical mapping is the identity for 240-high input, so it needs no
   * table or per-pixel division. */

  if (!g_live_sx_ready || width != 320)
    {
      for (x = 0; x < FOODLOOP_WIDTH; x++)
        {
          g_live_sx[x] = x * width / FOODLOOP_WIDTH;
        }

      g_live_sx_ready = true;
    }

  for (y = 0; y < FOODLOOP_HEIGHT; y++)
    {
      int source_y = y * height / FOODLOOP_HEIGHT;
      FAR uint16_t *dst = &g_live_canvas[y * FOODLOOP_WIDTH];

      for (x = 0; x < FOODLOOP_WIDTH; x++)
        {
          /* Camera frame is little-endian RGB565; the ST7789 is driven
           * MSB-first (CONFIG_LCD_ST7789_DATA_ENDIAN_LITTLE is not set),
           * so swap each pixel before writing it to the LCD.
           */

          uint16_t pixel = frame[source_y * width + g_live_sx[x]];
          dst[x] = (uint16_t)((pixel >> 8) | (pixel << 8));
        }
    }

  /* Reuse the LCD handle across frames. */

  if (g_live_lcd_fd < 0)
    {
      g_live_lcd_fd = open(FOODLOOP_LCD_DEVICE, O_RDWR);
      if (g_live_lcd_fd < 0)
        {
          ret = -errno;
          goto cleanup;
        }
    }

  fd = g_live_lcd_fd;
  ioctl(fd, LCDDEVIO_SETPOWER, 1);
  memset(&area, 0, sizeof(area));
  area.row_start = 0;
  area.row_end = FOODLOOP_HEIGHT - 1;
  area.col_start = 0;
  area.col_end = FOODLOOP_WIDTH - 1;
  area.stride = FOODLOOP_WIDTH * sizeof(*g_live_canvas);
  area.data = (uint8_t *)g_live_canvas;

  ret = ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)(uintptr_t)&area);
  if (ret < 0)
    {
      ret = -errno;
    }

  /* Keep the LCD handle and canvas cached for the next frame. */

  return ret;

cleanup:
  /* Only reached when the cached LCD handle could not be opened; reset the
   * cached canvas so the next call retries cleanly. */

  free(g_live_canvas);
  g_live_canvas = NULL;
  return ret;
}

/****************************************************************************
 * Draft result screen: lists up to FOODLOOP_DRAFT_LINES food items with
 * name, expiry date and storage.  Long names are truncated; the list is
 * drawn over the photo so a scan result is readable right away.
 ****************************************************************************/

#define FOODLOOP_DRAFT_LINES 5
#define FOODLOOP_DRAFT_ROW_H 26

static void foodloop_ui_draw_draft_result(struct foodloop_canvas_s *canvas,
                                          FAR const char *const *lines,
                                          int line_count)
{
  int row;

  if (line_count <= 0)
    {
      foodloop_ui_draw_draft(canvas);
      return;
    }

  foodloop_ui_fill(canvas, COLOR_BACKGROUND);
  foodloop_ui_header(canvas);
  foodloop_ui_center_text(canvas, 50, "SCAN RESULT", 2, COLOR_GREEN);
  foodloop_ui_rect(canvas, 60, 74, 120, 2, COLOR_GREEN);

  for (row = 0; row < line_count && row < FOODLOOP_DRAFT_LINES; row++)
    {
      foodloop_ui_text(canvas, 14, 88 + row * FOODLOOP_DRAFT_ROW_H,
                       lines[row], 1, COLOR_INK);
    }
}

int foodloop_ui_show_draft_result(FAR const char *const *lines,
                                  int line_count)
{
  struct foodloop_canvas_s canvas;
  struct lcddev_area_s area;
  int fd;
  int ret;

  canvas.pixels = malloc(FOODLOOP_WIDTH * FOODLOOP_HEIGHT *
                         sizeof(*canvas.pixels));
  if (canvas.pixels == NULL)
    {
      return -ENOMEM;
    }

  foodloop_ui_draw_draft_result(&canvas, lines, line_count);

  fd = open(FOODLOOP_LCD_DEVICE, O_RDWR);
  if (fd < 0)
    {
      ret = -errno;
      goto cleanup;
    }

  ioctl(fd, LCDDEVIO_SETPOWER, 1);
  memset(&area, 0, sizeof(area));
  area.row_start = 0;
  area.row_end = FOODLOOP_HEIGHT - 1;
  area.col_start = 0;
  area.col_end = FOODLOOP_WIDTH - 1;
  area.stride = FOODLOOP_WIDTH * sizeof(*canvas.pixels);
  area.data = (uint8_t *)canvas.pixels;

  ret = ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)(uintptr_t)&area);
  if (ret < 0)
    {
      ret = -errno;
    }

  close(fd);

cleanup:
  free(canvas.pixels);
  return ret;
}

/****************************************************************************
 * Local record list screen: a caller-supplied title over the same row
 * layout as the draft result, for "foodloop list".
 ****************************************************************************/

static void foodloop_ui_draw_line_list(struct foodloop_canvas_s *canvas,
                                       FAR const char *title,
                                       FAR const char *const *lines,
                                       int line_count)
{
  int row;

  foodloop_ui_fill(canvas, COLOR_BACKGROUND);
  foodloop_ui_header(canvas);
  foodloop_ui_center_text(canvas, 50, title, 2, COLOR_GREEN);
  foodloop_ui_rect(canvas, 60, 74, 120, 2, COLOR_GREEN);

  for (row = 0; row < line_count && row < FOODLOOP_DRAFT_LINES; row++)
    {
      foodloop_ui_text(canvas, 14, 88 + row * FOODLOOP_DRAFT_ROW_H,
                       lines[row], 1, COLOR_INK);
    }
}

int foodloop_ui_show_list_result(FAR const char *title,
                                 FAR const char *const *lines,
                                 int line_count)
{
  struct foodloop_canvas_s canvas;
  struct lcddev_area_s area;
  int fd;
  int ret;

  canvas.pixels = malloc(FOODLOOP_WIDTH * FOODLOOP_HEIGHT *
                         sizeof(*canvas.pixels));
  if (canvas.pixels == NULL)
    {
      return -ENOMEM;
    }

  foodloop_ui_draw_line_list(&canvas, title, lines, line_count);

  fd = open(FOODLOOP_LCD_DEVICE, O_RDWR);
  if (fd < 0)
    {
      ret = -errno;
      goto cleanup;
    }

  ioctl(fd, LCDDEVIO_SETPOWER, 1);
  memset(&area, 0, sizeof(area));
  area.row_start = 0;
  area.row_end = FOODLOOP_HEIGHT - 1;
  area.col_start = 0;
  area.col_end = FOODLOOP_WIDTH - 1;
  area.stride = FOODLOOP_WIDTH * sizeof(*canvas.pixels);
  area.data = (uint8_t *)canvas.pixels;

  ret = ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)(uintptr_t)&area);
  if (ret < 0)
    {
      ret = -errno;
    }

  close(fd);

cleanup:
  free(canvas.pixels);
  return ret;
}
