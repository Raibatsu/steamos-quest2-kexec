#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#define FB_W 1920
#define FB_H 3664
#define EYE_W 1832
#define EYE_H 1920

/*
 * Quest 2 JDI 1920x3664 portrait panel (rot=270):
 *  - eye 0 (Left Eye):  physical y in [0..1831]
 *  - eye 1 (Right Eye): physical y in [1832..3663]
 *  - Logical upright eye coordinates:
 *      lx in [0..1831] (left to right across eye)
 *      ly in [0..1919] (top to bottom in user's field of view)
 *    maps to:
 *      px = ly
 *      py = (eye ? 1832 : 0) + (1831 - lx)
 */
static inline void put_eye_px(uint32_t *fb, int stride_px, int eye, int lx, int ly, uint32_t color)
{
    if ((unsigned)lx >= EYE_W || (unsigned)ly >= EYE_H)
        return;
    int px = ly;
    int py = (eye ? 1832 : 0) + (EYE_W - 1 - lx);
    fb[py * stride_px + px] = color;
}

static void fill_eye_rect(uint32_t *fb, int stride_px, int eye,
                          int x0, int y0, int w, int h, uint32_t color)
{
    for (int dy = 0; dy < h; dy++) {
        int ly = y0 + dy;
        if ((unsigned)ly >= EYE_H) continue;
        for (int dx = 0; dx < w; dx++) {
            int lx = x0 + dx;
            if ((unsigned)lx >= EYE_W) continue;
            put_eye_px(fb, stride_px, eye, lx, ly, color);
        }
    }
}

/* Simple 5x7 bitmap font for uppercase letters, digits, space, '-', '.', ':' */
static const uint8_t *glyph5x7(char c)
{
    static const struct { char c; uint8_t rows[7]; } font[] = {
        {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
        {'-', {0x00,0x00,0x00,0x1f,0x00,0x00,0x00}},
        {'.', {0x00,0x00,0x00,0x00,0x00,0x0c,0x0c}},
        {':', {0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00}},
        {'0', {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}},
        {'1', {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}},
        {'2', {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}},
        {'3', {0x1f,0x02,0x04,0x02,0x01,0x11,0x0e}},
        {'4', {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}},
        {'5', {0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e}},
        {'6', {0x06,0x08,0x10,0x1e,0x11,0x11,0x0e}},
        {'7', {0x1f,0x01,0x02,0x04,0x08,0x08,0x08}},
        {'8', {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}},
        {'9', {0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c}},
        {'A', {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}},
        {'B', {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}},
        {'C', {0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}},
        {'D', {0x1c,0x12,0x11,0x11,0x11,0x12,0x1c}},
        {'E', {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}},
        {'F', {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}},
        {'G', {0x0e,0x11,0x10,0x17,0x11,0x11,0x0f}},
        {'H', {0x11,0x11,0x11,0x1f,0x11,0x11,0x11}},
        {'I', {0x0e,0x04,0x04,0x04,0x04,0x04,0x0e}},
        {'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0c}},
        {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
        {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1f}},
        {'M', {0x11,0x1b,0x15,0x15,0x11,0x11,0x11}},
        {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
        {'O', {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}},
        {'P', {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}},
        {'Q', {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}},
        {'R', {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}},
        {'S', {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}},
        {'T', {0x1f,0x04,0x04,0x04,0x04,0x04,0x04}},
        {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0e}},
        {'V', {0x11,0x11,0x11,0x11,0x11,0x0a,0x04}},
        {'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0a}},
        {'X', {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}},
        {'Y', {0x11,0x11,0x0a,0x04,0x04,0x04,0x04}},
        {'Z', {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}},
    };
    for (size_t i = 0; i < sizeof(font)/sizeof(font[0]); i++) {
        if (font[i].c == c)
            return font[i].rows;
    }
    return font[0].rows;
}

static void draw_eye_text_centered(uint32_t *fb, int stride_px, int eye,
                                   int cx, int y, const char *str, int scale, uint32_t color)
{
    int len = (int)strlen(str);
    int total_w = len * 6 * scale - scale;
    int x0 = cx - total_w / 2;
    for (int i = 0; i < len; i++) {
        const uint8_t *rows = glyph5x7(str[i]);
        int gx = x0 + i * 6 * scale;
        for (int r = 0; r < 7; r++) {
            for (int col = 0; col < 5; col++) {
                if (rows[r] & (1 << (4 - col))) {
                    fill_eye_rect(fb, stride_px, eye,
                                  gx + col * scale, y + r * scale,
                                  scale, scale, color);
                }
            }
        }
    }
}

static void draw_eye_circle(uint32_t *fb, int stride_px, int eye,
                            int cx, int cy, int r_in, int r_out, uint32_t color)
{
    int r2_in = r_in * r_in;
    int r2_out = r_out * r_out;
    for (int dy = -r_out; dy <= r_out; dy++) {
        for (int dx = -r_out; dx <= r_out; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 >= r2_in && d2 <= r2_out)
                put_eye_px(fb, stride_px, eye, cx + dx, cy + dy, color);
        }
    }
}

int main(void)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/fb0");
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("ioctl fb0");
        return 1;
    }

    size_t len = finfo.smem_len;
    uint32_t *fb = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        perror("mmap fb0");
        return 1;
    }

    int stride_px = finfo.line_length / 4;
    int cx = EYE_W / 2; /* 916 */
    int cy = EYE_H / 2; /* 960 */

    /* Render both eyes side-by-side */
    for (int eye = 0; eye < 2; eye++) {
        /* 1. Sky + floor background with perspective grid */
        for (int ly = 0; ly < EYE_H; ly++) {
            for (int lx = 0; lx < EYE_W; lx++) {
                uint32_t c;
                if (ly < cy) {
                    /* Upper sky gradient (deep navy -> horizon blue) */
                    int t = (ly * 255) / cy;
                    uint32_t r = 10 + (t * 20) / 255;
                    uint32_t g = 24 + (t * 70) / 255;
                    uint32_t b = 64 + (t * 140) / 255;
                    c = 0xFF000000 | (r << 16) | (g << 8) | b;
                } else if (ly < cy + 4) {
                    /* Bright horizon line */
                    c = 0xFF00FFFF;
                } else {
                    /* Lower ground plane with stereo perspective grid */
                    int dy = ly - cy;
                    int grid_y = ((dy * 24) / (dy + 120)) % 2;
                    int dx = lx - cx;
                    int persp_x = (dx * 320) / (dy + 160);
                    int grid_x = ((persp_x + 4000) / 40) % 2;
                    if ((dy % 64) < 3 || ((persp_x + 4000) % 40) < 2) {
                        c = 0xFF3090B0;
                    } else if ((grid_x ^ grid_y) & 1) {
                        c = 0xFF142234;
                    } else {
                        c = 0xFF0C1522;
                    }
                }
                put_eye_px(fb, stride_px, eye, lx, ly, c);
            }
        }

        /* 2. Outer eye-viewport border frame (inset 120px so visible inside lens FOV) */
        int bx0 = 140, by0 = 160, bw = EYE_W - 280, bh = EYE_H - 320;
        uint32_t border_col = (eye == 0) ? 0xFF00E5FF : 0xFFFF40A0;
        fill_eye_rect(fb, stride_px, eye, bx0, by0, bw, 8, border_col);
        fill_eye_rect(fb, stride_px, eye, bx0, by0 + bh - 8, bw, 8, border_col);
        fill_eye_rect(fb, stride_px, eye, bx0, by0, 8, bh, border_col);
        fill_eye_rect(fb, stride_px, eye, bx0 + bw - 8, by0, 8, bh, border_col);

        /* 3. Zero-parallax convergence ring at optical center (916, 960) */
        draw_eye_circle(fb, stride_px, eye, cx, cy, 260, 270, 0xFFFFFFFF);
        draw_eye_circle(fb, stride_px, eye, cx, cy, 140, 146, 0xFF80E0FF);

        /* 4. Stereo-disparity floating 3D card & center target (pop-out parallax) */
        /* Left eye shifts right (+18px), Right eye shifts left (-18px) -> positive screen parallax (near depth) */
        int disp = (eye == 0) ? +18 : -18;
        fill_eye_rect(fb, stride_px, eye, cx + disp - 180, cy - 290, 360, 110, 0xFF102840);
        fill_eye_rect(fb, stride_px, eye, cx + disp - 180, cy - 290, 360, 6, 0xFF00FF80);
        fill_eye_rect(fb, stride_px, eye, cx + disp - 180, cy - 186, 360, 6, 0xFF00FF80);
        draw_eye_text_centered(fb, stride_px, eye, cx + disp, cy - 262, "LINUX 7.2", 8, 0xFF00FF80);

        /* Eye-specific banner so winking left/right immediately verifies stereo separation */
        if (eye == 0) {
            draw_eye_text_centered(fb, stride_px, eye, cx, cy - 110, "LEFT EYE", 9, 0xFF00E5FF);
        } else {
            draw_eye_text_centered(fb, stride_px, eye, cx, cy - 110, "RIGHT EYE", 9, 0xFFFF60B0);
        }

        /* Center stereo crosshair with near disparity */
        int disp2 = (eye == 0) ? +26 : -26;
        fill_eye_rect(fb, stride_px, eye, cx + disp2 - 45, cy - 4, 90, 8, 0xFFFFFF00);
        fill_eye_rect(fb, stride_px, eye, cx + disp2 - 4, cy - 45, 8, 90, 0xFFFFFF00);
        draw_eye_circle(fb, stride_px, eye, cx + disp2, cy, 55, 63, 0xFFFFFF00);

        /* Bottom status banner */
        draw_eye_text_centered(fb, stride_px, eye, cx, cy + 120, "QUEST 2 STEREO", 7, 0xFFFFFFFF);
        draw_eye_text_centered(fb, stride_px, eye, cx, cy + 195, "1832X1920 PER EYE", 6, 0xFFB0D8FF);
    }

    vinfo.yoffset = 0;
    ioctl(fd, FBIOPAN_DISPLAY, &vinfo);
    msync(fb, len, MS_SYNC);
    printf("STEREO_SBS_OK (%ux%u stride=%u)\n", vinfo.xres, vinfo.yres, finfo.line_length);
    munmap(fb, len);
    close(fd);
    return 0;
}
