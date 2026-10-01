#include <QColor>
#include <math.h>
#include <string.h>
#include <vector>
#include "vfilters.h"

// Linear->sRGB conversion table
static unsigned char linear_to_srgb[256];
// sRGB->Linear conversion table
static unsigned char srgb_to_linear[256];

// helper for clamping values
static inline float clampf(float x, float lo, float hi) {
	return fminf(fmaxf(x, lo), hi);
}

#define RING_FRAMES 5
float last_gamma = 0;
uint32_t *ring_base = NULL;
static int ring_head = 0; // last frame idx
static int ring_wid = 0;
static int ring_hei = 0;

// Adaptive modes mix a pixel only after its pattern has held this many frames
// in a row: a moving picture repeats A,B,A by chance, but not for three periods.
#define AF_CONFIRM_2C	4
#define AF_CONFIRM_3C	4
#define AF_RUN_MAX	15
// per pixel: frames the 2C pattern has held (low nibble), the 3C one (high)
static std::vector<unsigned char> af_runs;

static inline unsigned char run_step(unsigned char run, bool hit) {
	return hit ? (run < AF_RUN_MAX ? run + 1 : AF_RUN_MAX) : 0;
}

// A scrolling GigaScreen picture is the same page shifted between t-2 and t, and
// the other page at t is t-1 shifted by half that. Rows are searched in bands,
// so a static picture and a scroller, or two scroll layers, each get their own.
#define AF_BAND		16	// rows in a band
#define AF_SHIFT_X	16	// search range, pixels (2 a dot)
#define AF_SHIFT_Y	8	// search range, rows
typedef struct {
	int dx, dy;		// t-2 moved by this is t; 0,0 when the band does not scroll
} afShift;
static std::vector<afShift> af_bands;

// Picks the band's shift from a sample of its rows. Only a whole-dot half shift
// can be followed, and only a clear winner over no shift at all is taken.
static afShift af_find_shift(const uint32_t* cur, int cstride, const uint32_t* old, int wid, int hei, int y0) {
	afShift res = {0, 0};
	int y1 = y0 + AF_BAND;
	if (y1 > hei) y1 = hei;
	int xa = AF_SHIFT_X, xb = wid - AF_SHIFT_X;
	if (xb <= xa) return res;
	int best = -1, bdx = 0, bdy = 0, cost0 = 0;
	for (int dy = -AF_SHIFT_Y; dy <= AF_SHIFT_Y; dy++) {
		for (int dx = -AF_SHIFT_X; dx <= AF_SHIFT_X; dx += 2) {
			if (dx && dy) continue;			// along one axis at a time
			int cost = 0;
			for (int y = y0 + 1; y < y1; y += 3) {	// odd step: rows of both parities
				int yo = y - dy;
				if ((yo < 0) || (yo >= hei)) continue;
				const uint32_t* pc = cur + y * cstride;
				const uint32_t* po = old + yo * wid - dx;
				for (int x = xa; x < xb; x += 2)
					cost += (pc[x] != po[x]);
			}
			if (!dx && !dy) {
				cost0 = cost;
			} else if ((best < 0) || (cost < best)) {
				best = cost;
				bdx = dx;
				bdy = dy;
			}
		}
	}
	// a band that changed little, or that no shift explains, keeps no shift
	if ((cost0 < 16) || (best < 0) || (best * 4 >= cost0)) return res;
	if ((bdx & 3) || (bdy & 1)) return res;		// half the shift is not a whole dot / row
	res.dx = bdx;
	res.dy = bdy;
	return res;
}

static void ring_rotate(void) {
	ring_head = (ring_head + RING_FRAMES - 1) % RING_FRAMES;
}

static uint32_t *ring_get_frame(int i, int size) { // i = 0..4, 0 = last, 1 = last-1, ...
	return ring_base + ((ring_head + i) % RING_FRAMES) * size;
}

static void rebuild_gamma_lut(float gamma) {

	gamma = fmaxf(1.0, gamma);	// gamma <= 1 == linear blending

	const float igamma = 1.0f / gamma;
	const float maxvalue = (float)(256 - 1);

	// generating sRGB colorspace conversion tables
	for (int i = 0; i < 256; i++) {
		const float component = (float)i / maxvalue;

		// building Linear->sRGB conversion table
		float v_fwd = component <= 0.0031308f ? (12.92f * component) * maxvalue
											: (1.055f * powf(component, igamma) - 0.055f) * maxvalue;
		linear_to_srgb[i] = (unsigned char)(clampf(v_fwd, 0.0f, maxvalue) + 0.5f);

		// building sRGB->Linear conversion table
		float v_rev = component <= 0.04045f ? (component / 12.92f) * maxvalue
											: powf((component + 0.055f) / 1.055f, gamma) * maxvalue;
		srgb_to_linear[i] = (unsigned char)(clampf(v_rev, 0.0f, maxvalue) + 0.5f);
	}
	last_gamma = gamma;
}

// - Blending functions --------------------------------------------------------
// Gigascreen blending via LUTs
static uint32_t blend_2c(uint32_t p0, uint32_t p1, float ratio) {
	const double ratio_rev = 1.0 - ratio;

	// Extract RGB pixel components for current frame (RGBA packed format)
	unsigned char frame0_r = srgb_to_linear[p0 & 0xFF];
	unsigned char frame0_b = srgb_to_linear[(p0 >> 16) & 0xFF];
	unsigned char frame0_g = srgb_to_linear[(p0 >> 8) & 0xFF];

	// Extract RGB pixel components for previous frame (RGBA packed format)
	unsigned char frame1_r = srgb_to_linear[p1 & 0xFF];
	unsigned char frame1_g = srgb_to_linear[(p1 >> 8) & 0xFF];
	unsigned char frame1_b = srgb_to_linear[(p1 >> 16) & 0xFF];

	// Look up precomputed blended components in encoded (8-bit) space.
	// Alpha forced opaque: the source pixels always are, but a bare RGB OR leaves it at 0.
	return 	linear_to_srgb[int(frame1_r * ratio + frame0_r * ratio_rev)] |
			linear_to_srgb[int(frame1_g * ratio + frame0_g * ratio_rev)] << 8 |
			linear_to_srgb[int(frame1_b * ratio + frame0_b * ratio_rev)] << 16 |
			0xFF000000;
}

// 3-Color blending in linear light using LUTs
static uint32_t blend_3c(uint32_t p0, uint32_t p1, uint32_t p2, float ratio) {
	const double ratio_rev = 1.0 - ratio;

	// Decode RGB components from RGBA encoded space to linear colorspace (sRGB -> linear)
	unsigned char frame0_r = srgb_to_linear[p0 & 0xFF];
	unsigned char frame0_b = srgb_to_linear[(p0 >> 16) & 0xFF];
	unsigned char frame0_g = srgb_to_linear[(p0 >> 8) & 0xFF];

	unsigned char frame1_r = srgb_to_linear[p1 & 0xFF];
	unsigned char frame1_g = srgb_to_linear[(p1 >> 8) & 0xFF];
	unsigned char frame1_b = srgb_to_linear[(p1 >> 16) & 0xFF];

	unsigned char frame2_r = srgb_to_linear[p2 & 0xFF];
	unsigned char frame2_g = srgb_to_linear[(p2 >> 8) & 0xFF];
	unsigned char frame2_b = srgb_to_linear[(p2 >> 16) & 0xFF];

	// Encode averaged linear components back to RGBA encoded space (linear -> sRGB).
	// Alpha forced opaque: the source pixels always are, but a bare RGB OR leaves it at 0.
	return 	linear_to_srgb[int((frame0_r + frame1_r + frame2_r) / 3 * ratio + frame0_r * ratio_rev)] |
			linear_to_srgb[int((frame0_g + frame1_g + frame2_g) / 3 * ratio + frame0_g * ratio_rev)] << 8 |
			linear_to_srgb[int((frame0_b + frame1_b + frame2_b) / 3 * ratio + frame0_b * ratio_rev)] << 16 |
			0xFF000000;
}

// Check if RGBA pixel has more than one color component
static bool rgb_has_multi_component(uint32_t c) {

	// Components
	uint32_t r = c & 0x000000FF; // Red
	uint32_t g = c & 0x0000FF00; // Green
	uint32_t b = c & 0x00FF0000; // Blue

	// More than one non-zero component?
	return ((r != 0) + (g != 0) + (b != 0)) > 1;
}

// blend src into dst with weight `mass` in linear space (sRGB-correct),
// then store original dst back to src for the next frame.
// sRGB colorspace in Gigascreen reference: https://hype.retroscene.org/blog/graphics/808.html
// dst is the top left corner of the shown frame inside the whole raster, so
// only that part is mixed - the ring keeps frames of exactly that size, and
// nothing is spent on the raster around it.
// reset: the frame before this one was not mixed (fast mode, rewind, mixing
// off), so the history does not lead up to it.
void scrMix(unsigned char* src, unsigned char* dst, int wid, int hei, int stride, double ratio, float gamma, int mode, int reset) {
	const double ratio_x2 = ratio * 2.0;
	int size = wid * hei;			// pixels in one frame of the ring

	// Init ring buffer pointer if not set yet
	if (ring_base == NULL) { ring_base = (uint32_t *)src; }
	// Rebuild gamma LUTs if Gamma value has changed
	if (last_gamma != gamma) { rebuild_gamma_lut(gamma); }

	// a history of another size or from before a gap would mix in a picture that
	// is not there: start it again from this frame, shown as it is
	if (reset || (wid != ring_wid) || (hei != ring_hei)) {
		ring_wid = wid;
		ring_hei = hei;
		af_runs.assign(size, 0);
		for (int i = 0; i < RING_FRAMES; i++) {
			uint32_t *pf = ring_get_frame(i, size);
			for (int y = 0; y < hei; y++)
				memcpy(pf + y * wid, dst + y * stride, wid * 4);
		}
		return;
	}

	// screen is in GL_RGBA 32-bit format: Red,Green,Blue,Alpha
	uint32_t *p0 = reinterpret_cast<uint32_t*>(dst);
	unsigned char *pr = af_runs.data();
	uint32_t *p1 = ring_get_frame(0, size);
	uint32_t *p2 = ring_get_frame(1, size);
	uint32_t *p3 = ring_get_frame(2, size);
	uint32_t *p4 = ring_get_frame(3, size);
	uint32_t *p5 = ring_get_frame(4, size);
	const uint32_t *f1 = p1;		// t-1 and t-2 from their first pixel, for the shifted reads
	const uint32_t *f2 = p2;
	int rest = wid;				// pixels left in the current line
	const int skip = stride - wid * 4;	// from a line's last pixel to the next line's first
	int x = 0, y = 0;
	const afShift* band = NULL;
	bool adaptive = (mode == AF_2C_ADAPTIVE) || (mode == AF_3C_ADAPTIVE);
	if (adaptive) {
		af_bands.resize((hei + AF_BAND - 1) / AF_BAND);
		for (size_t b = 0; b < af_bands.size(); b++)
			af_bands[b] = af_find_shift(p0, stride / 4, f2, wid, hei, b * AF_BAND);
		band = af_bands.data();
	}
	ring_rotate();

	while (size > 0) {
		const uint32_t c0 = *p0; // current pixel color
		const uint32_t c1 = *p1;
		const uint32_t c2 = *p2;
		const uint32_t c3 = *p3;
		const uint32_t c4 = *p4;
		const uint32_t c5 = *p5;
		uint32_t output_color = c0;
		bool multi_components;
		bool aba, per3;
		unsigned char run2, run3;
		uint32_t mate = c1;		// what c0 is mixed with in 2C
		if (adaptive) {
			const afShift& s = band[y / AF_BAND];
			bool moved = false;		// t-1 shifted is this pixel: plain motion
			aba = false;
			if (s.dx || s.dy) {
				int x2 = x - s.dx, y2 = y - s.dy;
				int xh = x - s.dx / 2, yh = y - s.dy / 2;
				if ((x2 >= 0) && (x2 < wid) && (y2 >= 0) && (y2 < hei)) {
					uint32_t m = f1[yh * wid + xh];
					moved = (c0 == m);
					if ((c0 == f2[y2 * wid + x2]) && !moved) {
						aba = true;
						mate = m;
					}
				}
			}
			if (!aba && !moved) aba = (c0 == c2 && c0 != c1);
		}

		switch (mode) {
		// 2C+3C (adaptive)
		case AF_3C_ADAPTIVE:
			// skip static pixels
			if (c0 == c1 && c0 == c2 && !aba) {
				*pr = 0;
				break;
			}

			// 3Color simple check
			multi_components =	rgb_has_multi_component(c0) ||
								rgb_has_multi_component(c1) ||
								rgb_has_multi_component(c2);
			// static RGB-image check
			per3 = !multi_components && c0 == c3 && c1 == c4 && c2 == c5;
			run2 = run_step(*pr & 0x0F, aba);
			run3 = run_step(*pr >> 4, per3);
			*pr = run2 | (run3 << 4);
			if (per3 && run3 >= AF_CONFIRM_3C) {
				output_color = blend_3c(c0, c1, c2, ratio_x2);
			} else if (aba && run2 >= AF_CONFIRM_2C) {
				// fallback to 2C blending
				output_color = blend_2c(c0, mate, ratio);
			}
			break;

		// 2C only (adaptive)
		case AF_2C_ADAPTIVE:
			run2 = run_step(*pr & 0x0F, aba);
			*pr = run2;
			if (aba && run2 >= AF_CONFIRM_2C)
				output_color = blend_2c(c0, mate, ratio);
			break;

		// 2C only (fullscreen)
		case AF_2C_FULL:
			// blend only on changed pixel
			if (c0 != c1)
				output_color = blend_2c(c0, c1, ratio);
			break;

		// 3C only (fullscreen)
		case AF_3C_FULL:
			// skip static pixels
			if (c0 == c1 && c0 == c2)
				break;

			output_color = blend_3c(c0, c1, c2, ratio_x2);
			break;

		default:
			break;
		}

		*p5++ = c0;				// replace most expired buffer with fresh data
		*p0++ = output_color;	// update current screen buffer with calculated color
		p1++;
		p2++;
		p3++;
		p4++;
		pr++;
		size--;
		x++;
		if (--rest == 0) {			// next line of the frame
			rest = wid;
			x = 0;
			y++;
			p0 = reinterpret_cast<uint32_t*>(reinterpret_cast<unsigned char*>(p0) + skip);
		}
	}
}
