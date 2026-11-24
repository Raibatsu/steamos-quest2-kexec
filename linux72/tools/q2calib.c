// q2calib: static stereo calibration image for the Quest 2 panel (fbdev). Run with SteamVR / q2-desktop stopped.
// Draws the same crosshair, rings and frame in each eye, centred on the lens centre shifted by /var/lib/q2/eyeshift
// (px, + = pictures toward the nose), and the current value as big digits. Redraws whenever the file changes.
// Panel geometry as in stereo_test.c: 1920x3664, eye 0 (left) = physical y [1832..3663], eye 1 (right) = y [0..1831],
// logical lx in [0,1831] left->right across the eye, ly in [0,1919] top->bottom; px = ly, py = y_base + 1831 - lx.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/kd.h>

#define FB_W 1920
#define FB_H 3664
#define EYE_W 1832
#define EYE_H 1920
#define WHITE 0x00ffffffu
#define GREY  0x00606060u
#define GREEN 0x0000ff40u

static uint32_t *fb, *front;
static int stride_px = FB_W, ttyfd = -1, running = 1;

static void px(int eye, int lx, int ly, uint32_t c)
{
	if ((unsigned)lx >= EYE_W || (unsigned)ly >= EYE_H)
		return;
	fb[((eye == 0 ? 1832 : 0) + (EYE_W - 1 - lx)) * stride_px + ly] = c;
}

static void rect(int eye, int x0, int y0, int w, int h, uint32_t c)
{
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			px(eye, x, y, c);
}

// 5x7 font: digits 0-9, '+', '-'
static const uint8_t font[12][7] = {
	{0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}, {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}, {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f},
	{0x1f,0x02,0x04,0x02,0x01,0x11,0x0e}, {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}, {0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e},
	{0x06,0x08,0x10,0x1e,0x11,0x11,0x0e}, {0x1f,0x01,0x02,0x04,0x08,0x08,0x08}, {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},
	{0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c}, {0x00,0x04,0x04,0x1f,0x04,0x04,0x00}, {0x00,0x00,0x00,0x1f,0x00,0x00,0x00},
};

static void glyph(int eye, int gi, int x, int y, int s, uint32_t c)
{
	for (int r = 0; r < 7; r++)
		for (int b = 0; b < 5; b++)
			if (font[gi][r] & (1 << (4 - b)))
				rect(eye, x + b * s, y + r * s, s, s, c);
}

static void number(int eye, int cx, int y, int v, int s, uint32_t c)
{
	char t[16];
	snprintf(t, sizeof(t), "%+d", v);
	int n = strlen(t), w = n * 6 * s - s;
	int x = cx - w / 2;
	for (int i = 0; i < n; i++, x += 6 * s)
		glyph(eye, t[i] == '+' ? 10 : t[i] == '-' ? 11 : t[i] - '0', x, y, s, c);
}

static void ring(int eye, int cx, int cy, int r, uint32_t c)
{
	for (int y = cy - r - 2; y <= cy + r + 2; y++)
		for (int x = cx - r - 2; x <= cx + r + 2; x++) {
			float d = sqrtf((float)((x - cx) * (x - cx) + (y - cy) * (y - cy)));
			if (d >= r - 1.5f && d <= r + 1.5f)
				px(eye, x, y, c);
		}
}

static void draw(int shift)
{
	memset(fb, 0, (size_t)FB_H * stride_px * 4);  // fb = back buffer here
	for (int eye = 0; eye < 2; eye++) {
		int cx = EYE_W / 2 + (eye == 0 ? shift : -shift), cy = EYE_H / 2;
		// frame at the eye region edge (shows the lens aperture placement)
		rect(eye, 40, 40, EYE_W - 80, 6, GREY);
		rect(eye, 40, EYE_H - 46, EYE_W - 80, 6, GREY);
		rect(eye, 40, 40, 6, EYE_H - 80, GREY);
		rect(eye, EYE_W - 46, 40, 6, EYE_H - 80, GREY);
		for (int r = 100; r <= 800; r += 100)
			ring(eye, cx, cy, r, r % 200 ? GREY : WHITE);
		rect(eye, cx - 2, 0, 5, EYE_H, WHITE);          // vertical line: must fuse between the eyes
		rect(eye, 0, cy - 2, EYE_W, 5, WHITE);          // horizontal line
		for (int d = -800; d <= 800; d += 50) {          // tick marks every 50 px
			int len = (d % 200 == 0) ? 40 : 20;
			rect(eye, cx + d - 1, cy - len, 3, 2 * len, WHITE);
			rect(eye, cx - len, cy + d - 1, 2 * len, 3, WHITE);
		}
		// ruler: absolute position across the eye image, label n = x / 100 (0 = left edge of the eye image, 18 = right edge).
		// It does NOT move with the shift: the outermost number you can still see on each side is the visible extent.
		for (int n = 0; n <= 18; n++) {
			int x = n * 100 + (n == 18 ? 31 : 0), ry = cy - 360;
			rect(eye, x - 2, ry, 5, 90, n % 2 ? GREY : WHITE);
			char t[8]; snprintf(t, sizeof(t), "%d", n);
			int w = strlen(t) * 36 - 6, gx = x - w / 2;
			for (int i = 0; t[i]; i++, gx += 36) glyph(eye, t[i] - '0', gx, ry - 70, 6, n % 2 ? GREY : WHITE);
		}
		rect(eye, cx - 230, cy + 70, 460, 130, 0);           // black box so the digits stay readable over the rings
		number(eye, cx, cy + 90, shift, 12, GREEN);          // the current shift, in px
	}
}

static void stop(int s) { running = 0; }

int main(void)
{
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) { perror("/dev/fb0"); return 1; }
	struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
	ioctl(fd, FBIOGET_VSCREENINFO, &v); ioctl(fd, FBIOGET_FSCREENINFO, &f);
	if (v.bits_per_pixel != 32 || v.xres != FB_W || v.yres != FB_H) {
		fprintf(stderr, "unexpected fb %ux%u %ubpp\n", v.xres, v.yres, v.bits_per_pixel);
		return 1;
	}
	stride_px = f.line_length / 4;
	front = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (front == MAP_FAILED) { perror("mmap"); return 1; }
	fb = malloc(f.smem_len);
	ttyfd = open("/dev/tty0", O_RDWR);
	if (ttyfd >= 0) ioctl(ttyfd, KDSETMODE, KD_GRAPHICS);   // stop fbcon drawing over us
	signal(SIGTERM, stop); signal(SIGINT, stop);

	int last = 1 << 30;
	while (running) {
		int shift = 0;
		FILE *p = fopen("/var/lib/q2/eyeshift", "r");
		if (p) { if (fscanf(p, "%d", &shift) != 1) shift = 0; fclose(p); }
		if (shift != last) { draw(shift); memcpy(front, fb, (size_t)FB_H * stride_px * 4); last = shift; fprintf(stderr, "shift %d px\n", shift); }
		struct timespec ts = {0, 80 * 1000 * 1000};
		nanosleep(&ts, NULL);
	}
	if (ttyfd >= 0) ioctl(ttyfd, KDSETMODE, KD_TEXT);
	return 0;
}
