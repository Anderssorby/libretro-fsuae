#include "sysconfig.h"
#include "sysdeps.h"

#include <ctype.h>
#include <assert.h>

#include "options.h"
#include "xwin.h"
#include "custom.h"
#include "drawing.h"
#include "memory.h"
#include "specialmonitors.h"
#include "debug.h"
#include "zfile.h"

static bool automatic;
static int monitor;

extern unsigned int bplcon0;
extern uae_u8 **row_map_genlock;

static uae_u8 graffiti_palette[256 * 4];

STATIC_INLINE bool is_transparent(uae_u8 v)
{
	return v == 0;
}

STATIC_INLINE uae_u8 FVR(struct vidbuffer *src, uae_u8 *dataline)
{
	if (src->pixbytes == 4)
		return dataline[2];
	else
		return ((((uae_u16*)dataline)[0] >> 11) & 31) << 3;
}
STATIC_INLINE uae_u8 FVG(struct vidbuffer *src, uae_u8 *dataline)
{
	if (src->pixbytes == 4)
		return dataline[1];
	else
		return ((((uae_u16*)dataline)[0] >> 5) & 63) << 2;
}
STATIC_INLINE uae_u8 FVB(struct vidbuffer *src, uae_u8 *dataline)
{
	if (src->pixbytes == 4)
		return dataline[0];
	else
		return ((((uae_u16*)dataline)[0] >> 0) & 31) << 2;
}

STATIC_INLINE bool FR(struct vidbuffer *src, uae_u8 *dataline)
{
	if (src->pixbytes == 4)
		return (dataline[2] & 0x80) != 0;
	else
		return ((dataline[1] >> 7) & 1) != 0;
}
STATIC_INLINE bool FG(struct vidbuffer *src, uae_u8 *dataline)
{
	if (src->pixbytes == 4)
		return (dataline[1] & 0x80) != 0;
	else
		return ((dataline[1] >> 2) & 1) != 0;
}
STATIC_INLINE bool FB(struct vidbuffer *src, uae_u8 *dataline)
{
	if (src->pixbytes == 4)
		return (dataline[0] & 0x80) != 0;
	else
		return ((dataline[0] >> 4) & 1) != 0;
}
STATIC_INLINE bool FI(struct vidbuffer *src, uae_u8 *dataline)
{
	if (src->pixbytes == 4)
		return (dataline[0] & 0x10) != 0;
	else
		return ((dataline[0] >> 1) & 1) != 0;
}

STATIC_INLINE uae_u8 FIRGB(struct vidbuffer *src, uae_u8 *dataline)
{
	uae_u8 v = 0;
#if 1
	if (FI(src, dataline))
		v |= 1;
	if (FR(src, dataline))
		v |= 8;
	if (FG(src, dataline))
		v |= 4;
	if (FB(src, dataline))
		v |= 2;
#else
	if (FI(src, dataline))
		v |= 1 << scramble[scramble_counter * 4 + 0];
	if (FR(src, dataline))
		v |= 1 << scramble[scramble_counter * 4 + 1];
	if (FG(src, dataline))
		v |= 1 << scramble[scramble_counter * 4 + 2];
	if (FB(src, dataline))
		v |= 1 << scramble[scramble_counter * 4 + 3];
#endif
	return v;
}

STATIC_INLINE uae_u8 DCTV_FIRBG(struct vidbuffer *src, uae_u8 *dataline)
{
	uae_u8 v = 0;
	if (FI(src, dataline))
		v |= 0x40;
	if (FR(src, dataline))
		v |= 0x10;
	if (FB(src, dataline))
		v |= 0x04;
	if (FG(src, dataline))
		v |= 0x01;
	return v;
}

STATIC_INLINE void PRGB(struct vidbuffer *dst, uae_u8 *dataline, uae_u8 r, uae_u8 g, uae_u8 b)
{
	if (dst->pixbytes == 4) {
		dataline[0] = b;
		dataline[1] = g;
		dataline[2] = r;
	} else {
		r >>= 3;
		g >>= 2;
		b >>= 3;
		((uae_u16*)dataline)[0] = (r << 11) | (g << 5) | b;
	}
}

STATIC_INLINE void PUT_PRGB(uae_u8 *d, uae_u8 *d2, struct vidbuffer *dst, uae_u8 r, uae_u8 g, uae_u8 b, int xadd, int doublelines, bool hdouble)
{
	if (hdouble)
		PRGB(dst, d - dst->pixbytes, r, g, b);
	PRGB(dst, d, r, g, b);
	if (xadd >= 2) {
		PRGB(dst, d + 1 * dst->pixbytes, r, g, b);
		if (hdouble)
			PRGB(dst, d + 2 * dst->pixbytes, r, g, b);
	}
	if (doublelines) {
		if (hdouble)
			PRGB(dst, d2 - dst->pixbytes, r, g, b);
		PRGB(dst, d2, r, g, b);
		if (xadd >= 2) {
			PRGB(dst, d2 + 1 * dst->pixbytes, r, g, b);
			if (hdouble)
				PRGB(dst, d2 + 2 * dst->pixbytes, r, g, b);
		}
	}
}

STATIC_INLINE void PUT_AMIGARGB(uae_u8 *d, uae_u8 *s, uae_u8 *d2, uae_u8 *s2, struct vidbuffer *dst, int xadd, int doublelines, bool hdouble)
{
	if (dst->pixbytes == 4) {
		if (hdouble)
			((uae_u32*)d)[-1] = ((uae_u32*)s)[-1];
		((uae_u32*)d)[0] = ((uae_u32*)s)[0];
	} else {
		if (hdouble)
			((uae_u16*)d)[-1] = ((uae_u16*)s)[-1];
		((uae_u16*)d)[0] = ((uae_u16*)s)[0];
	}
	if (doublelines) {
		if (dst->pixbytes == 4) {
			if (hdouble)
				((uae_u32*)d2)[-1] = ((uae_u32*)s2)[-1];
			((uae_u32*)d2)[0] = ((uae_u32*)s2)[0];
		} else {
			if (hdouble)
				((uae_u16*)d2)[-1] = ((uae_u16*)s2)[-1];
			((uae_u16*)d2)[0] = ((uae_u16*)s2)[0];
		}
	}
}

static void clearmonitor(struct vidbuffer *dst)
{
	uae_u8 *p = dst->bufmem;
	for (int y = 0; y < dst->height_allocated; y++) {
		memset(p, 0, dst->width_allocated * dst->pixbytes);
		p += dst->rowbytes;
	}
}

static void blank_generic(struct vidbuffer *src, struct vidbuffer *dst, int oddlines)
{
	int y, vdbl;
	int ystart, yend, isntsc;

	isntsc = (beamcon0 & 0x20) ? 0 : 1;
	if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
		isntsc = currprefs.ntscmode ? 1 : 0;

	vdbl = gfxvidinfo.ychange;

	ystart = isntsc ? VBLANK_ENDLINE_NTSC : VBLANK_ENDLINE_PAL;
	yend = isntsc ? MAXVPOS_NTSC : MAXVPOS_PAL;

	for (y = ystart; y < yend; y++) {
		int yoff = (((y * 2 + oddlines) - src->yoffset) / vdbl);
		if (yoff < 0)
			continue;
		if (yoff >= src->inheight)
			continue;
		uae_u8 *dstline = dst->bufmem + (((y * 2 + oddlines) - dst->yoffset) / vdbl) * dst->rowbytes;
		memset(dstline, 0, dst->inwidth * dst->pixbytes);
	}
}

static uae_s8 dctv_chroma[1600];
static uae_u8 dctv_luma[1600];

static const uae_u16 dctv_tables[] = {
	0xF9AF, 0xF9C9, 0xF9E2, 0xF9FC, 0xFA15, 0xFA2F, 0xFA48, 0xFA62,
	0xFA7B, 0xFA95, 0xFAAE, 0xFAC8, 0xFAE1, 0xFAFB, 0xFB14, 0xFB2E,
	0xFB47, 0xFB61, 0xFB7A, 0xFB94, 0xFBAD, 0xFBC7, 0xFBE0, 0xFBFA,
	0xFC13, 0xFC2D, 0xFC46, 0xFC60, 0xFC79, 0xFC93, 0xFCAC, 0xFCC6,
	0xFCDF, 0xFCF9, 0xFD12, 0xFD2C, 0xFD45, 0xFD5F, 0xFD78, 0xFD92,
	0xFDAB, 0xFDC5, 0xFDDE, 0xFDF8, 0xFE11, 0xFE2B, 0xFE44, 0xFE5E,
	0xFE77, 0xFE91, 0xFEAA, 0xFEC4, 0xFEDD, 0xFEF7, 0xFF10, 0xFF2A,
	0xFF43, 0xFF5D, 0xFF76, 0xFF90, 0xFFA9, 0xFFC3, 0xFFDC, 0xFFF6,
	0x000F, 0x0029, 0x0042, 0x005C, 0x0075, 0x008F, 0x00A8, 0x00C2,
	0x00DB, 0x00F5, 0x010E, 0x0128, 0x0141, 0x015B, 0x0174, 0x018E,
	0x01A7, 0x01C1, 0x01DA, 0x01F4, 0x020D, 0x0227, 0x0240, 0x025A,
	0x0273, 0x028D, 0x02A6, 0x02C0, 0x02D9, 0x02F3, 0x030C, 0x0326,
	0x033F, 0x0359, 0x0372, 0x038C, 0x03A5, 0x03BF, 0x03D8, 0x03F2,
	0x040B, 0x0425, 0x043E, 0x0458, 0x0471, 0x048B, 0x04A4, 0x04BE,
	0x04D7, 0x04F1, 0x050A, 0x0524, 0x053D, 0x0557, 0x0570, 0x058A,
	0x05A3, 0x05BD, 0x05D6, 0x05F0, 0x0609, 0x0623, 0x063C, 0x0656,
	0x066F, 0x0689, 0x06A2, 0x06BC, 0x06D5, 0x06EF, 0x0708, 0x0722,
	0x073B, 0x0755, 0x076E, 0x0788, 0x07A1, 0x07BB, 0x07D4, 0x07EE,
	0x0807, 0x0821, 0x083A, 0x0854, 0x086D, 0x0887, 0x08A0, 0x08BA,
	0x08D3, 0x08ED, 0x0906, 0x0920, 0x0939, 0x0953, 0x096C, 0x0986,
	0x099F, 0x09B9, 0x09D2, 0x09EC, 0x0A05, 0x0A1F, 0x0A38, 0x0A52,
	0x0A6B, 0x0A85, 0x0A9E, 0x0AB8, 0x0AD1, 0x0AEB, 0x0B04, 0x0B1E,
	0x0B37, 0x0B51, 0x0B6A, 0x0B84, 0x0B9D, 0x0BB7, 0x0BD0, 0x0BEA,
	0x0C03, 0x0C1D, 0x0C36, 0x0C50, 0x0C69, 0x0C83, 0x0C9C, 0x0CB6,
	0x0CCF, 0x0CE9, 0x0D02, 0x0D1C, 0x0D35, 0x0D4F, 0x0D68, 0x0D82,
	0x0D9B, 0x0DB5, 0x0DCE, 0x0DE8, 0x0E01, 0x0E1B, 0x0E34, 0x0E4E,
	0x0E67, 0x0E81, 0x0E9A, 0x0EB4, 0x0ECD, 0x0EE7, 0x0F00, 0x0F1A,
	0x0F33, 0x0F4D, 0x0F66, 0x0F80, 0x0F99, 0x0FB3, 0x0FCC, 0x0FE6,
	0x0FFF, 0x1019, 0x1032, 0x104C, 0x1065, 0x107F, 0x1098, 0x10B2,
	0x10CB, 0x10E5, 0x10FE, 0x1118, 0x1131, 0x114B, 0x1164, 0x117E,
	0x1197, 0x11B1, 0x11CA, 0x11E4, 0x11FD, 0x1217, 0x1230, 0x124A,
	0x1263, 0x127D, 0x1296, 0x12B0, 0x12C9, 0x12E3, 0x12FC, 0x1316
};

STATIC_INLINE int minmax(int v, int min, int max)
{
	if (v < min)
		v = min;
	if (v > max)
		v = max;
	return v;
}

static bool dctv(struct vidbuffer *src, struct vidbuffer *dst, bool doublelines, int oddlines)
{
	int y, x, vdbl, hdbl;
	int ystart, yend, isntsc;
	int xadd;

	isntsc = (beamcon0 & 0x20) ? 0 : 1;
	if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
		isntsc = currprefs.ntscmode ? 1 : 0;

	vdbl = gfxvidinfo.ychange;
	hdbl = gfxvidinfo.xchange;

	xadd = ((1 << 1) / hdbl) * src->pixbytes;

	ystart = isntsc ? VBLANK_ENDLINE_NTSC : VBLANK_ENDLINE_PAL;
	yend = isntsc ? MAXVPOS_NTSC : MAXVPOS_PAL;

	oddlines = 1;

	memset(dctv_luma, 0x40, sizeof dctv_luma);

	int signature_detected = -1;
	int signature_x = -1;
	int linenr = 0;
	uae_u8 r, g, b;
	for (y = ystart; y < yend; y++) {
		int yoff = (((y * 2 + oddlines) - src->yoffset) / vdbl);
		if (yoff < 0)
			continue;
		if (yoff >= src->inheight)
			continue;
		uae_u8 *line = src->bufmem + yoff * src->rowbytes;
		uae_u8 *dstline = dst->bufmem + (((y * 2 + oddlines) - dst->yoffset) / vdbl) * dst->rowbytes;

#if 0
		if (y < 60) {
			write_log(_T("%d:\n"), y);
			for (x = 22; x < 300; x += 1) {
				uae_u8 *s = line + ((x << 1) / hdbl) * src->pixbytes;
				write_log(_T("%01x"), FIRGB(src, s));
			}
			write_log(_T("*\n"));
			for (x = 21; x < 300; x += 1) {
				uae_u8 *s = line + ((x << 1) / hdbl) * src->pixbytes;
				write_log(_T("%01x"), FIRGB(src, s));
			}
			write_log(_T("\n"));
		}
#endif

		int signature_cnt = -1;
		bool signature_line = false;
		int firstnz = -1;
		bool sign = false;
		int oddeven = 0;
		uae_u8 prev = 0;
		uae_u8 vals[3] = { 0x40, 0x40, 0x40 };
		int chrsum = 0;
		uae_s8 *chrbuf = dctv_chroma + 10;
		uae_u8 *lumabuf = dctv_luma + 10;
		int zigzagoffset = 0;
		int prevtval = 0;
		for (x = 0; x < src->inwidth; x++) {
			uae_u8 *s = line + ((x << 1) / hdbl) * src->pixbytes;
			uae_u8 *d = dstline + ((x << 1) / hdbl) * dst->pixbytes + zigzagoffset;
			uae_u8 *s2 = s + src->rowbytes;
			uae_u8 *d2 = d + dst->rowbytes;
			uae_u8 newval = DCTV_FIRBG(src, s);
			uae_u8 val = prev | newval;

			if (firstnz < 0 && newval) {
				firstnz = 0;
				if (newval == 0x14) {
					zigzagoffset = -1 * dst->pixbytes;
					oddeven = -2;
				} else {
					zigzagoffset = 0;
					oddeven = -2;
				}
			}

			if (oddeven > 0) {
				if (!val)
					val = 64;
				int tval = minmax(val, 64, 224);
				if (tval != val)
					tval = prevtval;
				prevtval = tval;

				uae_s16 luma = (uae_s16)dctv_tables[tval];
				tval = minmax(luma / 16, 0, 255);
				uae_u8 v1 = (uae_u8)tval;

				r = b = g = v1;

				if (val < 64) {
					r = 0x80 + val * 2;
					b = g = 0;
				} else if (val > 224) {
					r = b = 0;
					g = 0x80 + (val - 224) * 2;
				}
				

				sign = !sign;

				PRGB(dst, d - dst->pixbytes, r, g, b);
				PRGB(dst, d, r, g, b);
				if (doublelines) {
					PRGB(dst, d2 - dst->pixbytes, r, g, b);
					PRGB(dst, d2, r, g, b);
				}
			}

			if (oddeven >= 0)
				oddeven = oddeven ? 0 : 1;
			else
				oddeven++;
			prev = newval << 1;
		}
	}
	dst->nativepositioning = true;
	return true;
}

static bool do_dctv(struct vidbuffer *src, struct vidbuffer *dst)
{
	bool v;
	if (interlace_seen) {
		if (currprefs.gfx_iscanlines) {
			v = dctv(src, dst, false, lof_store ? 0 : 1);
			if (v && currprefs.gfx_iscanlines > 1)
				blank_generic(src, dst, lof_store ? 1 : 0);
		} else {
			v = dctv(src, dst, false, 0);
			v |= dctv(src, dst, false, 1);
		}
	} else {
		v = dctv(src, dst, true, 0);
	}
	return v;
}

static uae_u8 *sm_frame_buffer;
#define SM_VRAM_WIDTH 1024
#define SM_VRAM_HEIGHT 800
#define SM_VRAM_BYTES 4
static int sm_configured;
static uae_u8 sm_acmemory[128];

static void sm_alloc_fb(void)
{
	if (!sm_frame_buffer) {
		sm_frame_buffer = xcalloc(uae_u8, SM_VRAM_WIDTH * SM_VRAM_HEIGHT * SM_VRAM_BYTES);
	}
}
static void sm_free(void)
{
	xfree(sm_frame_buffer);
	sm_frame_buffer = NULL;
	sm_configured = 0;
}

#define FC24_MAXHEIGHT 482
static uae_u8 fc24_mode, fc24_cr0, fc24_cr1;
static uae_u16 fc24_hpos, fc24_vpos, fc24_width;
static int fc24_offset;

STATIC_INLINE uae_u8 MAKEFCOVERLAY(uae_u8 v)
{
	v &= 3;
	v |= v << 2;
	v |= v << 4;
	v |= v << 6;
	return v;
}

static bool firecracker24(struct vidbuffer *src, struct vidbuffer *dst, bool doublelines, int oddlines)
{
	int y, x, vdbl, hdbl;
	int fc24_y, fc24_x, fc24_dx, fc24_xadd, fc24_xmult, fc24_xoffset;
	int ystart, yend, isntsc;
	int xadd, xaddfc;
	int bufferoffset;

	// FC disabled and Amiga enabled?
	if (!(fc24_cr1 & 1) && !(fc24_cr0 & 1))
		return false;

	isntsc = (beamcon0 & 0x20) ? 0 : 1;
	if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
		isntsc = currprefs.ntscmode ? 1 : 0;

	vdbl = gfxvidinfo.ychange;
	hdbl = gfxvidinfo.xchange; // 4=lores,2=hires,1=shres

	xaddfc = (1 << 1) / hdbl; // 0=lores,1=hires,2=shres
	xadd = xaddfc * src->pixbytes;
	

	ystart = isntsc ? VBLANK_ENDLINE_NTSC : VBLANK_ENDLINE_PAL;
	yend = isntsc ? MAXVPOS_NTSC : MAXVPOS_PAL;

	switch (fc24_width)
	{
		case 384:
		fc24_xmult = 0;
		break;
		case 512:
		fc24_xmult = 1;
		break;
		case 768:
		fc24_xmult = 1;
		break;
		case 1024:
		fc24_xmult = 2;
		break;
		default:
		return false;
	}
	
	if (fc24_xmult >= xaddfc) {
		fc24_xadd = fc24_xmult - xaddfc;
		fc24_dx = 0;
	} else {
		fc24_xadd = 0;
		fc24_dx = xaddfc - fc24_xmult;
	}

	fc24_xoffset = ((src->inwidth - ((fc24_width << fc24_dx) >> fc24_xadd)) / 2);
	fc24_xadd = 1 << fc24_xadd;

	bufferoffset = (fc24_cr0 & 2) ? 512 * SM_VRAM_BYTES: 0;

	fc24_y = 0;
	for (y = ystart; y < yend; y++) {
		int oddeven = 0;
		uae_u8 prev = 0;
		int yoff = (((y * 2 + oddlines) - src->yoffset) / vdbl);
		if (yoff < 0)
			continue;
		if (yoff >= src->inheight)
			continue;
		uae_u8 *line = src->bufmem + yoff * src->rowbytes;
		uae_u8 *line_genlock = row_map_genlock[yoff];
		uae_u8 *dstline = dst->bufmem + (((y * 2 + oddlines) - dst->yoffset) / vdbl) * dst->rowbytes;
		uae_u8 *vramline = sm_frame_buffer + (fc24_y + oddlines) * SM_VRAM_WIDTH * SM_VRAM_BYTES + bufferoffset;
		fc24_x = 0;
		for (x = 0; x < src->inwidth; x++) {
			uae_u8 r = 0, g = 0, b = 0;
			uae_u8 *s = line + ((x << 1) / hdbl) * src->pixbytes;
			uae_u8 *s_genlock = line_genlock + ((x << 1) / hdbl);
			uae_u8 *d = dstline + ((x << 1) / hdbl) * dst->pixbytes;
			int fc24_xx = (fc24_x >> fc24_dx) - fc24_xoffset;
			uae_u8 *vramptr = NULL;
			if (fc24_xx >= 0 && fc24_xx < fc24_width && fc24_y >= 0 && fc24_y < FC24_MAXHEIGHT) {
				vramptr = vramline + fc24_xx * SM_VRAM_BYTES;
				uae_u8 ax = vramptr[0];
				if (ax & 0x40) {
					r = MAKEFCOVERLAY(ax >> 4);
					g = MAKEFCOVERLAY(ax >> 2);
					b = MAKEFCOVERLAY(ax >> 0);
				} else {
					r = vramptr[1];
					g = vramptr[2];
					b = vramptr[3];
				}
			}
			if (!(fc24_cr0 & 1) && (!(fc24_cr1 & 1) || (!is_transparent(s_genlock[0])))) {
				uae_u8 *s2 = s + src->rowbytes;
				uae_u8 *d2 = d + dst->rowbytes;
				PUT_AMIGARGB(d, s, d2, s2, dst, xadd, doublelines, false);
			} else {
				PUT_PRGB(d, NULL, dst, r, g, b, 0, false, false);
				if (doublelines) {
					if (vramptr) {
						vramptr += SM_VRAM_WIDTH * SM_VRAM_BYTES;
						uae_u8 ax = vramptr[0];
						if (ax & 0x40) {
							r = MAKEFCOVERLAY(ax >> 4);
							g = MAKEFCOVERLAY(ax >> 2);
							b = MAKEFCOVERLAY(ax >> 0);
						} else {
							r = vramptr[1];
							g = vramptr[2];
							b = vramptr[3];
						}
					}
					PUT_PRGB(d + dst->rowbytes, NULL, dst, r, g, b, 0, false, false);
				}
			}
			fc24_x += fc24_xadd;
		}
		fc24_y += 2;
	}

	dst->nativepositioning = true;
	if (monitor != MONITOREMU_FIRECRACKER24) {
		monitor = MONITOREMU_FIRECRACKER24;
		write_log(_T("FireCracker mode\n"));
	}

	return true;
}

static void fc24_setoffset(void)
{
	fc24_vpos &= 511;
	fc24_hpos &= 1023;
	fc24_offset = fc24_vpos * SM_VRAM_WIDTH * SM_VRAM_BYTES + fc24_hpos * SM_VRAM_BYTES;
	sm_alloc_fb();
}

static void fc24_inc(uaecptr addr, bool write)
{
	addr &= 65535;
	if (addr >= 6) {
		fc24_hpos++;
		if (fc24_hpos >= 1024) {
			fc24_hpos = 0;
			fc24_vpos++;
			fc24_vpos &= 511;
		}
		fc24_setoffset();
	}
}

static void fc24_setmode(void)
{
	switch (fc24_cr0 >> 6)
	{
		case 0:
		fc24_width = 384;
		break;
		case 1:
		fc24_width = 512;
		break;
		case 2:
		fc24_width = 768;
		break;
		case 3:
		fc24_width = 1024;
		break;
	}
	sm_alloc_fb();
}

static void fc24_reset(void)
{
	if (currprefs.monitoremu != MONITOREMU_FIRECRACKER24)
		return;
	fc24_cr0 = 0;
	fc24_cr1 = 0;
	fc24_setmode();
	fc24_setoffset();
}

static void firecracker24_write_byte(uaecptr addr, uae_u8 v)
{
	addr &= 65535;
	switch (addr)
	{
		default:
		if (!sm_frame_buffer)
			return;
		sm_frame_buffer[fc24_offset + (addr & 3)] = v;
		break;
		case 10:
		fc24_cr0 = v;
		fc24_setmode();
		write_log(_T("FC24_CR0 = %02x\n"), fc24_cr0);
		break;
		case 11:
		fc24_cr1 = v;
		sm_alloc_fb();
		write_log(_T("FC24_CR1 = %02x\n"), fc24_cr1);
		break;
		case 12:
		fc24_vpos &= 0x00ff;
		fc24_vpos |= v << 8;
		fc24_setoffset();
		break;
		case 13:
		fc24_vpos &= 0xff00;
		fc24_vpos |= v;
		fc24_setoffset();
		//write_log(_T("V=%d "), fc24_vpos);
		break;
		case 14:
		fc24_hpos &= 0x00ff;
		fc24_hpos |= v << 8;
		fc24_setoffset();
		break;
		case 15:
		fc24_hpos &= 0xff00;
		fc24_hpos |= v;
		fc24_setoffset();
		//write_log(_T("H=%d "), fc24_hpos);
		break;
	}
}

static uae_u8 firecracker24_read_byte(uaecptr addr)
{
	uae_u8 v = 0;
	addr &= 65535;
	switch (addr)
	{
		default:
		if (!sm_frame_buffer)
			return v;
		v = sm_frame_buffer[fc24_offset + (addr & 3)];
		break;
		case 10:
		v = fc24_cr0;
		break;
		case 11:
		v = fc24_cr1;
		break;
		case 12:
		v = fc24_vpos >> 8;
		break;
		case 13:
		v = fc24_vpos >> 0;
		break;
		case 14:
		v = fc24_hpos >> 8;
		break;
		case 15:
		v = fc24_hpos >> 0;
		break;
	}
	return v;
}

static void firecracker24_write(uaecptr addr, uae_u32 v, int size)
{
	int offset = addr & 3;
	uaecptr oaddr = addr;
	int shift = 8 * (size - 1);
	while (size > 0 && addr < 10) {
		int off = fc24_offset + offset;
		if ((offset & 3) == 0) {
			if (!(fc24_cr1 & 0x80))
				sm_frame_buffer[off] = v >> shift;
		} else {
			sm_frame_buffer[off] = v >> shift;
		}
		offset++;
		size--;
		shift -= 8;
		addr++;
	}
	fc24_inc(oaddr, true);
}

static uae_u32 firecracker24_read(uaecptr addr, int size)
{
	uae_u32 v = 0;
	uaecptr oaddr = addr;
	int offset = addr & 3;
	while (size > 0 && addr < 10) {
		v <<= 8;
		v |= sm_frame_buffer[fc24_offset + offset];
		offset++;
		size--;
		addr++;
	}
	fc24_inc(oaddr, false);
	return v;
}

extern addrbank specialmonitors_bank;

static void REGPARAM2 sm_bput(uaecptr addr, uae_u32 b)
{
#ifdef JIT
	special_mem |= S_WRITE;
#endif
	b &= 0xff;
	addr &= 65535;
	if (!sm_configured) {
		switch (addr) {
			case 0x48:
			map_banks_z2(&specialmonitors_bank, expamem_z2_pointer >> 16, 65536 >> 16);
			sm_configured = 1;
			expamem_next(&specialmonitors_bank, NULL);
			break;
			case 0x4c:
			sm_configured = -1;
			expamem_shutup(&specialmonitors_bank);
			break;
		}
		return;
	}
	if (sm_configured > 0) {
		if (addr < 10) {
			firecracker24_write(addr, b, 1);
		} else {
			firecracker24_write_byte(addr, b);
		}
	}
}
static void REGPARAM2 sm_wput(uaecptr addr, uae_u32 b)
{
#ifdef JIT
	special_mem |= S_WRITE;
#endif
	addr &= 65535;
	if (addr < 10) {
		firecracker24_write(addr, b, 2);
	} else {
		firecracker24_write_byte(addr + 0, b >> 8);
		firecracker24_write_byte(addr + 1, b >> 0);
	}
}

static void REGPARAM2 sm_lput(uaecptr addr, uae_u32 b)
{
#ifdef JIT
	special_mem |= S_WRITE;
#endif
	addr &= 65535;
	if (addr < 10) {
		firecracker24_write(addr + 0, b >> 16, 2);
		firecracker24_write(addr + 2, b >>  0, 2);
	} else {
		firecracker24_write_byte(addr + 0, b >> 24);
		firecracker24_write_byte(addr + 1, b >> 16);
		firecracker24_write_byte(addr + 2, b >> 8);
		firecracker24_write_byte(addr + 3, b >> 0);
	}
}
static uae_u32 REGPARAM2 sm_bget(uaecptr addr)
{
	uae_u8 v = 0;
#ifdef JIT
	special_mem |= S_READ;
#endif
	addr &= 65535;
	if (!sm_configured) {
		if (addr >= sizeof sm_acmemory)
			return 0;
		return sm_acmemory[addr];
	}
	if (sm_configured > 0) {
		if (addr < 10) {
			v = firecracker24_read(addr, 1);
		} else {
			v = firecracker24_read_byte(addr);
		}
	}
	return v;
}
static uae_u32 REGPARAM2 sm_wget(uaecptr addr)
{
	uae_u16 v;
#ifdef JIT
	special_mem |= S_READ;
#endif
	addr &= 65535;
	if (addr < 10) {
		v = firecracker24_read(addr, 2);
	} else {
		v = firecracker24_read_byte(addr) << 8;
		v |= firecracker24_read_byte(addr + 1) << 0;
	}
	return v;
}
static uae_u32 REGPARAM2 sm_lget(uaecptr addr)
{
	uae_u32 v;
#ifdef JIT
	special_mem |= S_READ;
#endif
	addr &= 65535;
	if (addr < 10) {
		v  = firecracker24_read(addr + 0, 2) << 16;
		v |= firecracker24_read(addr + 2, 2) <<  0;
	} else {
		v = firecracker24_read_byte(addr) << 24;
		v |= firecracker24_read_byte(addr + 1) << 16;
		v |= firecracker24_read_byte(addr + 2) << 8;
		v |= firecracker24_read_byte(addr + 3) << 0;
	}
	return v;
}

addrbank specialmonitors_bank = {
	sm_lget, sm_wget, sm_bget,
	sm_lput, sm_wput, sm_bput,
	default_xlate, default_check, NULL, NULL, _T("DisplayAdapter"),
	dummy_lgeti, dummy_wgeti, ABFLAG_IO
};

static void ew(int addr, uae_u32 value)
{
	addr &= 0xffff;
	if (addr == 00 || addr == 02 || addr == 0x40 || addr == 0x42) {
		sm_acmemory[addr] = (value & 0xf0);
		sm_acmemory[addr + 2] = (value & 0x0f) << 4;
	} else {
		sm_acmemory[addr] = ~(value & 0xf0);
		sm_acmemory[addr + 2] = ~((value & 0x0f) << 4);
	}
}

static const uae_u8 firecracker24_autoconfig[16] = { 0xc1, 0, 0, 0, 2104 >> 8, 2104 & 255 };

addrbank *specialmonitor_autoconfig_init(int devnum)
{
	sm_configured = 0;
	memset(sm_acmemory, 0xff, sizeof sm_acmemory);
	for (int i = 0; i < 16; i++) {
		uae_u8 b = firecracker24_autoconfig[i];
		ew(i * 4, b);
	}
	return &specialmonitors_bank;
}

static bool do_firecracker24(struct vidbuffer *src, struct vidbuffer *dst)
{
	bool v;
	if (interlace_seen) {
		if (currprefs.gfx_iscanlines) {
			v = firecracker24(src, dst, false, lof_store ? 0 : 1);
			if (v && currprefs.gfx_iscanlines > 1)
				blank_generic(src, dst, lof_store ? 1 : 0);
		} else {
			v = firecracker24(src, dst, false, 0);
			v |= firecracker24(src, dst, false, 1);
		}
	} else {
		v = firecracker24(src, dst, true, 0);
	}
	return v;
}

static uae_u16 avideo_previous_fmode[2];
static int av24_offset[2];
static int av24_doublebuffer[2];
static int av24_writetovram[2];
static int av24_mode[2];
static int avideo_allowed;

static bool avideo(struct vidbuffer *src, struct vidbuffer *dst, bool doublelines, int oddlines, int lof)
{
	int y, x, vdbl, hdbl;
	int ystart, yend, isntsc;
	int xadd, xaddpix;
	int mode;
	int offset = -1;
	bool writetovram;
	bool av24;
	int doublebuffer = -1;
	uae_u16 fmode;
	
	fmode = avideo_previous_fmode[lof];

	if (currprefs.monitoremu == MONITOREMU_AUTO) {
		if (!avideo_allowed)
			return false;
		av24 = avideo_allowed == 24;
	} else {
		av24 = currprefs.monitoremu == MONITOREMU_AVIDEO24;
	}

	if (currprefs.chipset_mask & CSMASK_AGA)
		return false;

	if (av24) {
		writetovram = av24_writetovram[lof] != 0;
		mode = av24_mode[lof];
	} else {
		mode = fmode & 7;
		if (mode == 1)
			offset = 0;
		else if (mode == 3)
			offset = 1;
		else if (mode == 2)
			offset = 2;
		writetovram = offset >= 0;
	}

	if (!mode)
		return false;

	sm_alloc_fb();

	//write_log(_T("%04x %d %d %d\n"), avideo_previous_fmode[oddlines], mode, offset, writetovram);

	isntsc = (beamcon0 & 0x20) ? 0 : 1;
	if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
		isntsc = currprefs.ntscmode ? 1 : 0;

	vdbl = gfxvidinfo.ychange;
	hdbl = gfxvidinfo.xchange;

	xaddpix = (1 << 1) / hdbl;
	xadd = ((1 << 1) / hdbl) * src->pixbytes;

	ystart = isntsc ? VBLANK_ENDLINE_NTSC : VBLANK_ENDLINE_PAL;
	yend = isntsc ? MAXVPOS_NTSC : MAXVPOS_PAL;

	for (y = ystart; y < yend; y++) {
		int oddeven = 0;
		uae_u8 prev = 0;
		int yoff = (((y * 2 + oddlines) - src->yoffset) / vdbl);
		if (yoff < 0)
			continue;
		if (yoff >= src->inheight)
			continue;
		uae_u8 *line = src->bufmem + yoff * src->rowbytes;
		uae_u8 *dstline = dst->bufmem + (((y * 2 + oddlines) - dst->yoffset) / vdbl) * dst->rowbytes;
		uae_u8 *vramline = sm_frame_buffer + y * 2 * SM_VRAM_WIDTH * SM_VRAM_BYTES;

		if (av24) {
			vramline += av24_offset[lof];
		} else {
			if (fmode & 0x10)
				vramline += SM_VRAM_WIDTH * SM_VRAM_BYTES;
		}

		for (x = 0; x < src->inwidth; x++) {
			uae_u8 *s = line + ((x << 1) / hdbl) * src->pixbytes;
			uae_u8 *d = dstline + ((x << 1) / hdbl) * dst->pixbytes;
			uae_u8 *vramptr = vramline + ((x << 1) / hdbl) * SM_VRAM_BYTES;
			uae_u8 *s2 = s + src->rowbytes;
			uae_u8 *d2 = d + dst->rowbytes;
			if (writetovram) {
				uae_u8 val[3];
				val[0] = FVR(src, s) >> 4;
				val[1] = FVG(src, s) >> 4;
				val[2] = FVB(src, s) >> 4;
				if (av24) {
					uae_u8 v;

					/*
					R3 = 20 08 08 08
					R2 = 20 01 01 01
					R1 = 10 02 02 02
					R0 = 08 04 04 04

					G3 = 20 04 04 04
					G2 = 10 08 08 08
					G1 = 10 01 01 01
					G0 = 08 02 02 02

					B3 = 20 02 02 02
					B2 = 10 04 04 04
					B1 = 08 08 08 08
					B0 = 08 01 01 01
					*/

					uae_u8 rval = vramptr[0];
					uae_u8 gval = vramptr[1];
					uae_u8 bval = vramptr[2];

					/* This probably should use lookup tables.. */

					if (fmode & 0x01) { // Red (low)

						v = (((val[0] >> 2) & 1) << 0);
						rval &= ~(0x01);
						rval |= v;

						v = (((val[1] >> 1) & 1) << 0);
						gval &= ~(0x01);
						gval |= v;

						v = (((val[2] >> 3) & 1) << 1) | (((val[2] >> 0) & 1) << 0);
						bval &= ~(0x02 | 0x01);
						bval |= v;
					}

					if (fmode & 0x02) { // Green (low)

						v = (((val[0] >> 1) & 1) << 1);
						rval &= ~(0x02);
						rval |= v;

						v = (((val[1] >> 0) & 1) << 1) | (((val[1] >> 3) & 1) << 2);
						gval &= ~(0x02 | 0x04);
						gval |= v;

						v = (((val[2] >> 2) & 1) << 2);
						bval &= ~(0x04);
						bval |= v;
					}

					if (fmode & 0x04) { // Blue (low)

						v = (((val[0] >> 0) & 1) << 2) | (((val[0] >> 3) & 1) << 3);
						rval &= ~(0x04 | 0x08);
						rval |= v;

						v = (((val[1] >> 2) & 1) << 3);
						gval &= ~(0x08);
						gval |= v;

						v = (((val[2] >> 1) & 1) << 3);
						bval &= ~(0x08);
						bval |= v;
					}

					if (fmode & 0x08) { // Red (high)
						
						v = (((val[0] >> 2) & 1) << 4);
						rval &= ~(0x10);
						rval |= v;
						
						v = (((val[1] >> 1) & 1) << 4);
						gval &= ~(0x10);
						gval |= v;

						v = (((val[2] >> 3) & 1) << 5) | (((val[2] >> 0) & 1) << 4);
						bval &= ~(0x20 | 0x10);
						bval |= v;
					}

					if (fmode & 0x10) { // Green (high)

						v = (((val[0] >> 1) & 1) << 5);
						rval &= ~(0x20);
						rval |= v;

						v = (((val[1] >> 0) & 1) << 5) | (((val[1] >> 3) & 1) << 6);
						gval &= ~(0x20 | 0x40);
						gval |= v;

						v = (((val[2] >> 2) & 1) << 6);
						bval &= ~(0x40);
						bval |= v;
					}

					if (fmode & 0x20) { // Blue (high)

						v = (((val[0] >> 0) & 1) << 6) | (((val[0] >> 3) & 1) << 7);
						rval &= ~(0x40 | 0x80);
						rval |= v;

						v = (((val[1] >> 2) & 1) << 7);
						gval &= ~(0x80);
						gval |= v;

						v = (((val[2] >> 1) & 1) << 7);
						bval &= ~(0x80);
						bval |= v;
					}

					if (av24_doublebuffer[lof] == 0 && (fmode & (0x08 | 0x10 | 0x20))) {
						rval = (rval & 0xf0) | (rval >> 4);
						gval = (gval & 0xf0) | (gval >> 4);
						bval = (bval & 0xf0) | (bval >> 4);
					}

					vramptr[0] = rval;
					vramptr[1] = gval;
					vramptr[2] = bval;

				} else {

					uae_u8 v = val[offset];
					vramptr[offset] = (v << 4) | (v & 15);

				}
			}
			if (writetovram || mode == 7 || (mode == 6 && FVR(src, s) == 0 && FVG(src, s) == 0 && FVB(src, s) == 0)) {
				uae_u8 r, g, b;
				r = vramptr[0];
				g = vramptr[1];
				b = vramptr[2];
				if (av24_doublebuffer[lof] == 1) {
					r = (r << 4) | (r & 0x0f);
					g = (g << 4) | (g & 0x0f);
					b = (b << 4) | (b & 0x0f);
				} else if (av24_doublebuffer[lof] == 2) {
					r = (r >> 4) | (r & 0xf0);
					g = (g >> 4) | (g & 0xf0);
					b = (b >> 4) | (b & 0xf0);
				}
				PUT_PRGB(d, d2, dst, r, g, b, xaddpix, doublelines, false);
			} else {
				PUT_AMIGARGB(d, s, d2, s2, dst, xaddpix, doublelines, false);
			}
		}
	}

	dst->nativepositioning = true;
	if (monitor != MONITOREMU_AVIDEO12 && monitor != MONITOREMU_AVIDEO24) {
		monitor = av24 ? MONITOREMU_AVIDEO24 : MONITOREMU_AVIDEO12;
		write_log (_T("AVIDEO%d mode\n"), av24 ? 24 : 12);
	}

	return true;
}

void specialmonitor_store_fmode(int vpos, int hpos, uae_u16 fmode)
{
	int lof = lof_store ? 0 : 1;
	if (vpos < 0) {
		for (int i = 0; i < 2; i++) {
			avideo_previous_fmode[i] = 0;
			av24_offset[i] = 0;
			av24_doublebuffer[i] = 0;
			av24_mode[i] = 0;
			av24_writetovram[i] = 0;
		}
		avideo_allowed = 0;
		return;
	}
	if (currprefs.monitoremu == MONITOREMU_AUTO) {
		if ((fmode & 0x0080))
			avideo_allowed = 24;
		if ((fmode & 0x00f0) < 0x40 && !avideo_allowed)
			avideo_allowed = 12;
		if (!avideo_allowed)
			return;
	}
	//write_log(_T("%04x\n"), fmode);

	if (fmode == 0x91) {
		av24_offset[lof] = SM_VRAM_WIDTH * SM_VRAM_BYTES;
	}
	if (fmode == 0x92) {
		av24_offset[lof] = 0;
	}

	if (fmode & 0x8000) {
		av24_doublebuffer[lof] = (fmode & 0x4000) ? 1 : 2;
		av24_mode[lof] = 6;
	}

	if ((fmode & 0xc0) == 0x40) {
		av24_writetovram[lof] = (fmode & 0x3f) != 0;
	}

	if (fmode == 0x80) {
		av24_mode[lof] = 0;
	}

	if (fmode == 0x13) {
		av24_mode[lof] = 6;
		av24_doublebuffer[lof] = 0;
	}

	avideo_previous_fmode[lof] = fmode;
}

static bool do_avideo(struct vidbuffer *src, struct vidbuffer *dst)
{
	bool v;
	int lof = lof_store ? 0 : 1;
	if (interlace_seen) {
		if (currprefs.gfx_iscanlines) {
			v = avideo(src, dst, false, lof, lof);
			if (v && currprefs.gfx_iscanlines > 1)
				blank_generic(src, dst, !lof);
		} else {
			v = avideo(src, dst, false, 0, 0);
			v |= avideo(src, dst, false, 1, 1);
		}
	} else {
		v = avideo(src, dst, true, 0, lof);
	}
	return v;
}


static bool videodac18(struct vidbuffer *src, struct vidbuffer *dst, bool doublelines, int oddlines)
{
	int y, x, vdbl, hdbl;
	int ystart, yend, isntsc;
	int xadd, xaddpix;
	uae_u16 hsstrt, hsstop, vsstrt, vsstop;
	int xstart, xstop;

	if ((beamcon0 & (0x80 | 0x100 | 0x200 | 0x10)) != 0x300)
		return false;
	getsyncregisters(&hsstrt, &hsstop, &vsstrt, &vsstop);

	if (hsstop >= (maxhpos & ~1))
		hsstrt = 0;
	xstart = ((hsstrt * 2) << RES_MAX) - src->xoffset;
	xstop = ((hsstop * 2) << RES_MAX) - src->xoffset;

	isntsc = (beamcon0 & 0x20) ? 0 : 1;
	if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
		isntsc = currprefs.ntscmode ? 1 : 0;

	vdbl = gfxvidinfo.ychange;
	hdbl = gfxvidinfo.xchange;

	xaddpix = (1 << 1) / hdbl;
	xadd = ((1 << 1) / hdbl) * src->pixbytes;

	ystart = isntsc ? VBLANK_ENDLINE_NTSC : VBLANK_ENDLINE_PAL;
	yend = isntsc ? MAXVPOS_NTSC : MAXVPOS_PAL;

	uae_u8 r = 0, g = 0, b = 0;
	for (y = ystart; y < yend; y++) {
		int oddeven = 0;
		uae_u8 prev = 0;
		int yoff = (((y * 2 + oddlines) - src->yoffset) / vdbl);
		if (yoff < 0)
			continue;
		if (yoff >= src->inheight)
			continue;
		uae_u8 *line = src->bufmem + yoff * src->rowbytes;
		uae_u8 *dstline = dst->bufmem + (((y * 2 + oddlines) - dst->yoffset) / vdbl) * dst->rowbytes;
		r = g = b = 0;
		for (x = 0; x < src->inwidth; x++) {
			uae_u8 *s = line + ((x << 1) / hdbl) * src->pixbytes;
			uae_u8 *d = dstline + ((x << 1) / hdbl) * dst->pixbytes;
			uae_u8 *s2 = s + src->rowbytes;
			uae_u8 *d2 = d + dst->rowbytes;
			uae_u8 newval = FIRGB(src, s);
			uae_u8 val = prev | (newval << 4);
			if (oddeven) {
				int mode = val >> 6;
				int data = (val & 63) << 2;
				if (mode == 0) {
					r = data;
					g = data;
					b = data;
				} else if (mode == 1) {
					b = data;
				} else if (mode == 2) {
					r = data;
				} else {
					g = data;
				}
				if (y >= vsstrt && y < vsstop && x >= xstart && y < xstop) {
					PUT_PRGB(d, d2, dst, r, g, b, xaddpix, doublelines, true);
				} else {
					PUT_AMIGARGB(d, s, d2, s2, dst, xaddpix, doublelines, true);
				}
			}
			oddeven = oddeven ? 0 : 1;
			prev = val >> 4;
		}
	}

	dst->nativepositioning = true;
	if (monitor != MONITOREMU_VIDEODAC18) {
		monitor = MONITOREMU_VIDEODAC18;
		write_log (_T("Video DAC 18 mode\n"));
	}

	return true;
}

static bool do_videodac18(struct vidbuffer *src, struct vidbuffer *dst)
{
	bool v;
	if (interlace_seen) {
		if (currprefs.gfx_iscanlines) {
			v = videodac18(src, dst, false, lof_store ? 0 : 1);
			if (v && currprefs.gfx_iscanlines > 1)
				blank_generic(src, dst, lof_store ? 1 : 0);
		} else {
			v = videodac18(src, dst, false, 0);
			v |= videodac18(src, dst, false, 1);
		}
	} else {
		v = videodac18(src, dst, true, 0);
	}
	return v;
}

static const uae_u8 ham_e_magic_cookie[] = { 0xa2, 0xf5, 0x84, 0xdc, 0x6d, 0xb0, 0x7f  };
static const uae_u8 ham_e_magic_cookie_reg = 0x14;
static const uae_u8 ham_e_magic_cookie_ham = 0x18;

static bool ham_e(struct vidbuffer *src, struct vidbuffer *dst, bool doublelines, int oddlines)
{
	int y, x, vdbl, hdbl;
	int ystart, yend, isntsc;
	int xadd, xaddpix;
	bool hameplus = currprefs.monitoremu == MONITOREMU_HAM_E_PLUS;

	isntsc = (beamcon0 & 0x20) ? 0 : 1;
	if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
		isntsc = currprefs.ntscmode ? 1 : 0;

	vdbl = gfxvidinfo.ychange;
	hdbl = gfxvidinfo.xchange;

	xaddpix = (1 << 1) / hdbl;
	xadd = ((1 << 1) / hdbl) * src->pixbytes;

	ystart = isntsc ? VBLANK_ENDLINE_NTSC : VBLANK_ENDLINE_PAL;
	yend = isntsc ? MAXVPOS_NTSC : MAXVPOS_PAL;

	uae_u8 r, g, b;
	/* or is an alternative operator and cannot be used as an identifier */
	uae_u8 or_, og, ob;
	int pcnt = 0;
	int bank = 0;
	int mode_active = 0;
	bool prevzeroline = false;
	int was_active = 0;
	bool cookie_line = false;
	int cookiestartx = 10000;
	for (y = ystart; y < yend; y++) {
		int yoff = (((y * 2 + oddlines) - src->yoffset) / vdbl);
		if (yoff < 0)
			continue;
		if (yoff >= src->inheight)
			continue;
		uae_u8 *line = src->bufmem + yoff * src->rowbytes;
		uae_u8 *line_genlock = row_map_genlock[yoff];
		uae_u8 *dstline = dst->bufmem + (((y * 2 + oddlines) - dst->yoffset) / vdbl) * dst->rowbytes;

		bool getpalette = false;
		uae_u8 prev = 0;
		bool zeroline = true;
		int oddeven = 0;
		for (x = 0; x < src->inwidth; x++) {
			uae_u8 *s = line + ((x << 1) / hdbl) * src->pixbytes;
			uae_u8 *s_genlock = line_genlock + ((x << 1) / hdbl);
			uae_u8 *d = dstline + ((x << 1) / hdbl) * dst->pixbytes;
			uae_u8 *s2 = s + src->rowbytes;
			uae_u8 *d2 = d + dst->rowbytes;
			uae_u8 newval = FIRGB(src, s);
			uae_u8 val = prev | newval;

			if (s_genlock[0])
				zeroline = false;

			if (val == ham_e_magic_cookie[0] && x + sizeof ham_e_magic_cookie + 1 < src->inwidth) {
				int i;
				for (i = 1; i <= sizeof ham_e_magic_cookie; i++) {
					uae_u8 val2 = (FIRGB(src, s + (i * 2 - 1) * xadd) << 4) | FIRGB(src, s + (i * 2 + 0) * xadd);
					if (i < sizeof ham_e_magic_cookie) {
						if (val2 != ham_e_magic_cookie[i])
							break;
					} else if (val2 == ham_e_magic_cookie_reg || val2 == ham_e_magic_cookie_ham) {
						mode_active = val2;
						getpalette = true;
						prevzeroline = false;
						cookiestartx = x - 1;
						x += i * 2;
						oddeven = 0;
						cookie_line = true;
					}
				}
				if (i == sizeof ham_e_magic_cookie + 1)
					continue;
			}

			if (!cookie_line && x == cookiestartx)
				oddeven = 0;

			if (oddeven) {
				if (getpalette) {
					graffiti_palette[pcnt] = val;
					pcnt++;
					if ((pcnt & 3) == 3)
						pcnt++;
					// 64 colors/line
					if ((pcnt & ((4 * 64) - 1)) == 0)
						getpalette = false;
					pcnt &= (4 * 256) - 1;
				}
				if (mode_active) {
					if (cookie_line || x < cookiestartx) {
						r = g = b = 0;
						or_ = og = ob = 0;
					} else {
						if (mode_active == ham_e_magic_cookie_reg) {
							uae_u8 *pal = &graffiti_palette[val * 4];
							r = pal[0];
							g = pal[1];
							b = pal[2];
						} else if (mode_active == ham_e_magic_cookie_ham) {
							int mode = val >> 6;
							int color = val & 63;
							if (mode == 0 && color <= 59) {
								uae_u8 *pal = &graffiti_palette[(bank + color) * 4];
								r = pal[0];
								g = pal[1];
								b = pal[2];
							} else if (mode == 0) {
								bank = (color & 3) * 64;
							} else if (mode == 1) {
								b = color << 2;
							} else if (mode == 2) {
								r = color << 2;
							} else if (mode == 3) {
								g = color << 2;
							}
						}
					}

					if (hameplus) {
						uae_u8 ar, ag, ab;

						ar = (r + or_) / 2;
						ag = (g + og) / 2;
						ab = (b + ob) / 2;

						if (xaddpix >= 2) {
							PRGB(dst, d - dst->pixbytes, ar, ag, ab);
							PRGB(dst, d, ar, ag, ab);
							PRGB(dst, d + 1 * dst->pixbytes, r, g, b);
							PRGB(dst, d + 2 * dst->pixbytes, r, g, b);
							if (doublelines) {
								PRGB(dst, d2 - dst->pixbytes, ar, ag, ab);
								PRGB(dst, d2, ar, ag, ab);
								PRGB(dst, d2 + 1 * dst->pixbytes, r, g, b);
								PRGB(dst, d2 + 2 * dst->pixbytes, r, g, b);
							}
						} else {
							PRGB(dst, d - dst->pixbytes, ar, ag, ab);
							PRGB(dst, d, r, g, b);
							if (doublelines) {
								PRGB(dst, d2 - dst->pixbytes, ar, ag, ab);
								PRGB(dst, d2, r, g, b);
							}
						}
						or_ = r;
						og = g;
						ob = b;
					} else {
						PUT_PRGB(d, d2, dst, r, g, b, xaddpix, doublelines, true);
					}
				} else {
					PUT_AMIGARGB(d, s, d2, s2, dst, xaddpix, doublelines, true);
				}
			}

			oddeven = oddeven ? 0 : 1;
			prev = val << 4;
		}

		if (cookie_line) {
			// Erase magic cookie. I assume real HAM-E would erase it
			// because not erasing it would look really ugly.
			memset(dstline, 0, dst->outwidth * dst->pixbytes);
			if (doublelines)
				memset(dstline + dst->rowbytes, 0, dst->outwidth * dst->pixbytes);
		}

		cookie_line = false;
		if (mode_active)
			was_active = mode_active;
		if (zeroline) {
			if (prevzeroline) {
				mode_active = 0;
				pcnt = 0;
				cookiestartx = 10000;
			}
			prevzeroline = true;
		} else {
			prevzeroline = false;
		}

	}

	if (was_active) {
		dst->nativepositioning = true;
		if (monitor != MONITOREMU_HAM_E) {
			monitor = MONITOREMU_HAM_E;
			write_log (_T("HAM-E mode, %s\n"), was_active == ham_e_magic_cookie_reg ? _T("REG") : _T("HAM"));
		}
	}

	return was_active != 0;
}

static bool do_hame(struct vidbuffer *src, struct vidbuffer *dst)
{
	bool v;
	if (interlace_seen) {
		if (currprefs.gfx_iscanlines) {
			v = ham_e(src, dst, false, lof_store ? 0 : 1);
			if (v && currprefs.gfx_iscanlines > 1)
				blank_generic(src, dst, lof_store ? 1 : 0);
		} else {
			v = ham_e(src, dst, false, 0);
			v |= ham_e(src, dst, false, 1);
		}
	} else {
		v = ham_e(src, dst, true, 0);
	}
	return v;
}

static bool graffiti(struct vidbuffer *src, struct vidbuffer *dst)
{
	int y, x;
	int ystart, yend, isntsc;
	int xstart, xend;
	uae_u8 *srcbuf, *srcend;
	uae_u8 *dstbuf;
	bool command, hires, found;
	int xadd, xpixadd, extrapix;
	int waitline = 0, dbl;
	uae_u8 read_mask = 0xff, color = 0, color2 = 0;

	if (!(bplcon0 & 0x0100)) // GAUD
		return false;

	command = true;
	found = false;
	isntsc = (beamcon0 & 0x20) ? 0 : 1;
	if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
		isntsc = currprefs.ntscmode ? 1 : 0;

	dbl = gfxvidinfo.ychange == 1 ? 2 : 1;

	ystart = isntsc ? VBLANK_ENDLINE_NTSC : VBLANK_ENDLINE_PAL;
	yend = isntsc ? MAXVPOS_NTSC : MAXVPOS_PAL;
	if (src->yoffset >= (ystart << VRES_MAX))
		ystart = src->yoffset >> VRES_MAX;

	xadd = gfxvidinfo.xchange == 1 ? src->pixbytes * 2 : src->pixbytes;
	xpixadd = gfxvidinfo.xchange == 1 ? 4 : 2;

	xstart = 0x1c * 2 + 1;
	xend = 0xf0 * 2 + 1;
	if (!(currprefs.chipset_mask & CSMASK_AGA)) {
		xstart++;
		xend++;
	}

	srcbuf = src->bufmem + (((ystart << VRES_MAX) - src->yoffset) / gfxvidinfo.ychange) * src->rowbytes + (((xstart << RES_MAX) - src->xoffset) / gfxvidinfo.xchange) * src->pixbytes;
	srcend = src->bufmem + (((yend << VRES_MAX) - src->yoffset) / gfxvidinfo.ychange) * src->rowbytes;
	extrapix = 0;

	dstbuf = dst->bufmem + (((ystart << VRES_MAX) - src->yoffset) / gfxvidinfo.ychange) * dst->rowbytes + (((xstart << RES_MAX) - src->xoffset) / gfxvidinfo.xchange) * dst->pixbytes;

	y = 0;
	while (srcend > srcbuf && dst->bufmemend > dstbuf) {
		uae_u8 *srcp = srcbuf + extrapix;
		uae_u8 *dstp = dstbuf;

		x = xstart;
		while (x < xend) {
			
			uae_u8 mask = 0x80;
			uae_u8 chunky[4] = { 0, 0, 0, 0 };
			while (mask) {
				if (FR(src, srcp)) // R
					chunky[3] |= mask;
				if (FG(src, srcp)) // G
					chunky[2] |= mask;
				if (FB(src, srcp)) // B
					chunky[1] |= mask;
				if (FI(src, srcp)) // I
					chunky[0] |= mask;
				srcp += xadd;
				mask >>= 1;
			}

			if (command) {
				if (chunky[0] || chunky[1] || chunky[2] || chunky[3] || found) {
					for (int pix = 0; pix < 2; pix++) {
						uae_u8 cmd = chunky[pix * 2 + 0];
						uae_u8 parm = chunky[pix * 2 + 1];

						if (automatic && cmd >= 0x40)
							return false;
						if (cmd != 0)
							found = true;
						if (cmd & 8) {
							command = false;
							dbl = 1;
							waitline = 2;
							if (0 && (cmd & 16)) {
								hires = true;
								xadd /= 2;
								xpixadd /= 2;
								extrapix = -4 * src->pixbytes;
							} else {
								hires = false;
							}
							if (xpixadd == 0) // shres needed
								return false;
							if (monitor != MONITOREMU_GRAFFITI)
								clearmonitor(dst);
						} else if (cmd & 4) {
							if ((cmd & 3) == 1) {
								read_mask = parm;
							} else if ((cmd & 3) == 2) {
								graffiti_palette[color * 4 + color2] = (parm << 2) | (parm & 3);
								color2++;
								if (color2 == 3) {
									color2 = 0;
									color++;
								}
							} else if ((cmd & 3) == 0) {
								color = parm;
								color2 = 0;
							}
						}
					}
				}

				memset(dstp, 0, dst->pixbytes * 4 * 2);
				dstp += dst->pixbytes * 4 * 2;

			} else if (waitline) {
			
				memset(dstp, 0, dst->pixbytes * 4 * 2);
				dstp += dst->pixbytes * 4 * 2;
			
			} else {

				for (int pix = 0; pix < 4; pix++) {
					uae_u8 r, g, b, c;
					
					c = chunky[pix] & read_mask;
					r = graffiti_palette[c * 4 + 0];
					g = graffiti_palette[c * 4 + 1];
					b = graffiti_palette[c * 4 + 2];
					PRGB(dst, dstp, r, g, b);
					dstp += dst->pixbytes;
					PRGB(dst, dstp, r, g, b);
					dstp += dst->pixbytes;
					
					if (gfxvidinfo.xchange == 1 && !hires) {
						PRGB(dst, dstp, r, g, b);
						dstp += dst->pixbytes;
						PRGB(dst, dstp, r, g, b);
						dstp += dst->pixbytes;
					}
				}

			}

			x += xpixadd;
		}

		y++;
		srcbuf += src->rowbytes * dbl;
		dstbuf += dst->rowbytes * dbl;
		if (waitline > 0)
			waitline--;
	}

	dst->nativepositioning = true;

	if (monitor != MONITOREMU_GRAFFITI) {
		monitor = MONITOREMU_GRAFFITI;
		write_log (_T("GRAFFITI %s mode\n"), hires ? _T("hires") : _T("lores"));
	}

	return true;
}

/* A2024 information comes from US patent 4851826 */

static bool a2024(struct vidbuffer *src, struct vidbuffer *dst)
{
	int y;
	uae_u8 *srcbuf, *dstbuf;
	uae_u8 *dataline;
	int px, py, doff, pxcnt, dbl;
	int panel_width, panel_width_draw, panel_height, srcxoffset;
	bool f64, interlace, expand, wpb, less16;
	uae_u8 enp, dpl;
	bool hires, ntsc, found;
	int idline;
	int total_width, total_height;
	
	dbl = gfxvidinfo.ychange == 1 ? 2 : 1;
	doff = (128 * 2 / gfxvidinfo.xchange) * src->pixbytes;
	found = false;

	for (idline = 21; idline <= 29; idline += 8) {
		if (src->yoffset > (idline << VRES_MAX))
			continue;
		// min 178 max 234
		dataline = src->bufmem + (((idline << VRES_MAX) - src->yoffset) / gfxvidinfo.ychange) * src->rowbytes + (((200 << RES_MAX) - src->xoffset) / gfxvidinfo.xchange) * src->pixbytes;

#if 0
		write_log (_T("%02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x\n"),
			dataline[0 * doff + 0], dataline[0 * doff + 1], dataline[0 * doff + 2],
			dataline[1 * doff + 0], dataline[1 * doff + 1], dataline[1 * doff + 2],
			dataline[2 * doff + 0], dataline[2 * doff + 1], dataline[2 * doff + 2],
			dataline[3 * doff + 0], dataline[3 * doff + 1], dataline[3 * doff + 2]);
#endif

		if (FB(src, &dataline[0 * doff]))			// 0:B = 0
			continue;
		if (!FI(src, &dataline[0 * doff]))			// 0:I = 1
			continue;
		if (FI(src, &dataline[2 * doff]))			// 2:I = 0
			continue;
		if (!FI(src, &dataline[3 * doff]))			// 3:I = 1
			continue;

		ntsc = idline < 26;
		found = true;
		break;
	}

	if (!found)
		return false;

	px = py = 0;
	if (FB(src, &dataline[1 * doff])) // 1:B FN2
		px |= 2;
	if (FG(src, &dataline[1 * doff])) // 1:G FN1
		px |= 1;
	if (FR(src, &dataline[1 * doff])) // 1:R FN0
		py |= 1;

	f64 = FR(src, &dataline[0 * doff]) != 0;		// 0:R
	interlace = FG(src, &dataline[0 * doff]) != 0;	// 0:G (*Always zero)
	expand = FI(src, &dataline[1 * doff]) != 0;		// 1:I (*Always set)
	enp = FR(src, &dataline[2 * doff]) ? 1 : 0;		// 2:R (*ENP=3)
	enp |= FG(src, &dataline[2 * doff]) ? 2 : 0;	// 2:G
	wpb = FB(src, &dataline[2 * doff]) != 0;		// 2:B (*Always zero)
	dpl = FR(src, &dataline[3 * doff]) ? 1 : 0;		// 3:R (*DPL=3)
	dpl |= FG(src, &dataline[3 * doff]) ? 2 : 0;	// 3:G
	less16 = FB(src, &dataline[3 * doff]) != 0;		// 3:B

	/* (*) = AOS A2024 driver static bits. Not yet implemented in emulation. */

	if (f64) {
		panel_width = 336;
		panel_width_draw = px == 2 ? 352 : 336;
		pxcnt = 3;
		hires = false;
		srcxoffset = 113;
		if (px > 2)
			return false;
		total_width = 336 + 336 + 352;
	} else {
		panel_width = 512;
		panel_width_draw = 512;
		pxcnt = 2;
		hires = true;
		srcxoffset = 129;
		if (px > 1)
			return false;
		total_width = 512 + 512;
	}
	panel_height = ntsc ? 400 : 512;

	if (monitor != MONITOREMU_A2024) {
		clearmonitor(dst);
	}

#if 0
	write_log (_T("0 = F6-4:%d INTERLACE:%d\n"), f64, interlace);
	write_log (_T("1 = FN:%d EXPAND:%d\n"), py + px *2, expand);
	write_log (_T("2 = ENP:%d WPB=%d\n"), enp, wpb);
	write_log (_T("3 = DPL:%d LESS16=%d\n"), dpl, less16);
#endif
#if 0
	write_log (_T("%02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x %dx%d\n"),
		dataline[0 * doff + 0], dataline[0 * doff + 1], dataline[0 * doff + 2],
		dataline[1 * doff + 0], dataline[1 * doff + 1], dataline[1 * doff + 2],
		dataline[2 * doff + 0], dataline[2 * doff + 1], dataline[2 * doff + 2],
		dataline[3 * doff + 0], dataline[3 * doff + 1], dataline[3 * doff + 2],
		px, py);
#endif

	if (less16) {
		total_width -= 16;
		if (px == pxcnt - 1)
			panel_width_draw -= 16;
	}
	total_height = panel_height * dbl;
	
	srcbuf = src->bufmem + (((44 << VRES_MAX) - src->yoffset) / gfxvidinfo.ychange) * src->rowbytes + (((srcxoffset << RES_MAX) - src->xoffset) / gfxvidinfo.xchange) * src->pixbytes;
	dstbuf = dst->bufmem + py * (panel_height / gfxvidinfo.ychange) * dst->rowbytes + px * ((panel_width * 2) / gfxvidinfo.xchange) * dst->pixbytes;

	for (y = 0; y < (panel_height / (dbl == 1 ? 1 : 2)) / gfxvidinfo.ychange; y++) {
#if 0
		memcpy (dstbuf, srcbuf, ((panel_width * 2) / gfxvidinfo.xchange) * dst->pixbytes);
#else
		uae_u8 *srcp = srcbuf;
		uae_u8 *dstp1 = dstbuf;
		uae_u8 *dstp2 = dstbuf + dst->rowbytes;
		int x;
		for (x = 0; x < (panel_width_draw * 2) / gfxvidinfo.xchange; x++) {
			uae_u8 c1 = 0, c2 = 0;
			if (FR(src, srcp)) // R
				c1 |= 2;
			if (FG(src, srcp)) // G
				c2 |= 2;
			if (FB(src, srcp)) // B
				c1 |= 1;
			if (FI(src, srcp)) // I
				c2 |= 1;
			if (dpl == 0) {
				c1 = c2 = 0;
			} else if (dpl == 1) {
				c1 &= 1;
				c1 |= c1 << 1;
				c2 &= 1;
				c2 |= c2 << 1;
			} else if (dpl == 2) {
				c1 &= 2;
				c1 |= c1 >> 1;
				c2 &= 2;
				c2 |= c2 >> 1;
			}
			if (dbl == 1) {
				c1 = (c1 + c2 + 1) / 2;
				c1 = (c1 << 6) | (c1 << 4) | (c1 << 2) | (c1 << 0);
				PRGB(dst, dstp1, c1, c1, c1);
			} else {
				c1 = (c1 << 6) | (c1 << 4) | (c1 << 2) | (c1 << 0);
				c2 = (c2 << 6) | (c2 << 4) | (c2 << 2) | (c2 << 0);
				PRGB(dst, dstp1, c1, c1, c1);
				PRGB(dst, dstp2, c2, c2, c2);
				dstp2 += dst->pixbytes;
			}
			srcp += src->pixbytes;
			if (!hires)
				srcp += src->pixbytes;
			dstp1 += dst->pixbytes;
		}
#endif
		srcbuf += src->rowbytes * dbl;
		dstbuf += dst->rowbytes * dbl;
	}

	total_width /= 2;
	total_width <<= currprefs.gfx_resolution;

	dst->outwidth = total_width;
	dst->outheight = total_height;
	dst->inwidth = total_width;
	dst->inheight = total_height;
	dst->inwidth2 = total_width;
	dst->inheight2 = total_height;
	dst->nativepositioning = false;

	if (monitor != MONITOREMU_A2024) {
		monitor = MONITOREMU_A2024;
		write_log (_T("A2024 %dHz %s mode\n"), hires ? 10 : 15, ntsc ? _T("NTSC") : _T("PAL"));
	}

	return true;
}

static bool emulate_specialmonitors2(struct vidbuffer *src, struct vidbuffer *dst)
{
	automatic = false;
	if (currprefs.monitoremu == MONITOREMU_AUTO) {
		automatic = true;
		bool v = a2024(src, dst);
		if (!v)
			v = graffiti(src, dst);
		if (!v)
			v = do_videodac18(src, dst);
		if (!v) {
			if (avideo_allowed)
				v = do_avideo(src, dst);
		}
		return v;
	} else if (currprefs.monitoremu == MONITOREMU_A2024) {
		return a2024(src, dst);
	} else if (currprefs.monitoremu == MONITOREMU_GRAFFITI) {
		return graffiti(src, dst);
	} else if (currprefs.monitoremu == MONITOREMU_DCTV) {
		return do_dctv(src, dst);
	} else if (currprefs.monitoremu == MONITOREMU_HAM_E || currprefs.monitoremu == MONITOREMU_HAM_E_PLUS) {
		return do_hame(src, dst);
	} else if (currprefs.monitoremu == MONITOREMU_VIDEODAC18) {
		return do_videodac18(src, dst);
	} else if (currprefs.monitoremu == MONITOREMU_AVIDEO12 || currprefs.monitoremu == MONITOREMU_AVIDEO24) {
		avideo_allowed = -1;
		return do_avideo(src, dst);
	} else if (currprefs.monitoremu == MONITOREMU_FIRECRACKER24) {
		return do_firecracker24(src, dst);
	}
	return false;
}


bool emulate_specialmonitors(struct vidbuffer *src, struct vidbuffer *dst)
{
	if (!emulate_specialmonitors2(src, dst)) {
		if (monitor) {
			clearmonitor(dst);
			monitor = 0;
			write_log (_T("Native mode\n"));
		}
		return false;
	}
	return true;
}

void specialmonitor_reset(void)
{
	if (!currprefs.monitoremu)
		return;
	specialmonitor_store_fmode(-1, -1, 0);
	fc24_reset();
}

bool specialmonitor_need_genlock(void)
{
	switch (currprefs.monitoremu)
	{
		case MONITOREMU_FIRECRACKER24:
		case MONITOREMU_HAM_E:
		case MONITOREMU_HAM_E_PLUS:
		return true;
	}
	if (currprefs.genlock_image && currprefs.genlock)
		return true;
	return false;
}

static uae_u8 *genlock_image;
static int genlock_image_width, genlock_image_height, genlock_image_pitch;
static uae_u8 noise_buffer[1024];
static uae_u32 noise_seed, noise_add, noise_index;

static uae_u32 quickrand(void)
{
	noise_seed = (noise_seed >> 1) ^ (0 - (noise_seed & 1) & 0xd0000001);
	return noise_seed;
}

static void init_noise(void)
{
	noise_seed++;
	for (int i = 0; i < sizeof noise_buffer; i++) {
		noise_buffer[i] = quickrand();
	}
}

static uae_u8 get_noise(void)
{
	noise_index += noise_add;
	noise_index &= 1023;
	return noise_buffer[noise_index];
}

#include "png.h"

struct png_cb
{
	uae_u8 *ptr;
	int size;
};

#ifdef FSUAE
// FIXME
static void readcallback(png_structp png_ptr, png_bytep out, png_size_t count)
#else
static void __cdecl readcallback(png_structp png_ptr, png_bytep out, png_size_t count)
#endif
{
	png_voidp io_ptr = png_get_io_ptr(png_ptr);

	if (!io_ptr)
		return;
	struct png_cb *cb = (struct png_cb*)io_ptr;
	if (count > cb->size)
		count = cb->size;
	memcpy(out, cb->ptr, count);
	cb->ptr += count;
	cb->size -= count;
}

static void load_genlock_image(void)
{
	extern unsigned char test_card_png[];
	extern unsigned int test_card_png_len;
	uae_u8 *b = test_card_png;
	uae_u8 *bfree = NULL;
	int file_size = test_card_png_len;
	png_structp png_ptr;
	png_infop info_ptr;
	png_uint_32 width, height;
	int depth, color_type;
	struct png_cb cb;
    png_bytepp row_pp;
    png_size_t cols;

	xfree(genlock_image);
	genlock_image = NULL;

	if (currprefs.genlock_image == 3) {
		int size;
		uae_u8 *bb = zfile_load_file(currprefs.genlock_image_file, &size);
		if (bb) {
			file_size = size;
			b = bb;
			bfree = bb;
		}
	}

	if (!png_check_sig(b, 8))
		goto end;

	png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
	if (!png_ptr)
		goto end;
	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr) {
		png_destroy_read_struct(&png_ptr, 0, 0);
		goto end;
	}
	cb.ptr = b;
	cb.size = file_size;
	png_set_read_fn(png_ptr, &cb, readcallback);

	png_read_info(png_ptr, info_ptr);

	png_get_IHDR(png_ptr, info_ptr, &width, &height, &depth, &color_type, 0, 0, 0);

	if (color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_expand(png_ptr);
	if (color_type == PNG_COLOR_TYPE_GRAY && depth < 8)
		png_set_expand(png_ptr);

	if (depth > 8)
		png_set_strip_16(png_ptr);
	if (depth < 8)
		png_set_packing(png_ptr);
	if (!(color_type & PNG_COLOR_MASK_ALPHA))
		png_set_add_alpha(png_ptr, 0, PNG_FILLER_AFTER);

    cols = png_get_rowbytes(png_ptr, info_ptr);

	genlock_image_pitch = width * 4;
	genlock_image_width = width;
	genlock_image_height = height;

    row_pp = new png_bytep[height];
	
	genlock_image = xcalloc(uae_u8, width * height * 4);
	
	for (int i = 0; i < height; i++) {
		row_pp[i] = (png_bytep) &genlock_image[i * genlock_image_pitch];
	}

	png_read_image(png_ptr, row_pp);
	png_read_end(png_ptr, info_ptr);

	png_destroy_read_struct(&png_ptr, &info_ptr, 0);

	delete[] row_pp;
end:
	xfree(bfree);
}

static bool do_genlock(struct vidbuffer *src, struct vidbuffer *dst, bool doublelines, int oddlines)
{
	int y, x, vdbl, hdbl;
	int ystart, yend, isntsc;
	int gl_vdbl_l, gl_vdbl_r;
	int gl_hdbl_l, gl_hdbl_r, gl_hdbl;
	int gl_hcenter, gl_vcenter;
	int mix1 = 0, mix2 = 0;

	isntsc = (beamcon0 & 0x20) ? 0 : 1;
	if (!(currprefs.chipset_mask & CSMASK_ECS_AGNUS))
		isntsc = currprefs.ntscmode ? 1 : 0;

	if (!genlock_image && currprefs.genlock_image == 2) {
		load_genlock_image();
	}
	if (genlock_image && currprefs.genlock_image != 2) {
		xfree(genlock_image);
		genlock_image = NULL;
	}

	if (gfxvidinfo.xchange == 1)
		hdbl = 0;
	else if (gfxvidinfo.xchange == 2)
		hdbl = 1;
	else
		hdbl = 2;

	gl_hdbl_l = gl_hdbl_r = 0;
	if (genlock_image_width < 600) {
		gl_hdbl = 0;
	} else if (genlock_image_width < 1000) {
		gl_hdbl = 1;
	} else {
		gl_hdbl = 2;
	}
	if (hdbl >= gl_hdbl) {
		gl_hdbl_l = hdbl - gl_hdbl;
	} else {
		gl_hdbl_r = gl_hdbl - hdbl;
	}

	if (gfxvidinfo.ychange == 1)
		vdbl = 0;
	else
		vdbl = 1;

	gl_vdbl_l = gl_vdbl_r = 0;

	gl_hcenter = (genlock_image_width - ((src->inwidth << hdbl) >> gl_hdbl)) / 2;

	ystart = isntsc ? VBLANK_ENDLINE_NTSC : VBLANK_ENDLINE_PAL;
	yend = isntsc ? MAXVPOS_NTSC : MAXVPOS_PAL;

	gl_vcenter = (((genlock_image_height << gl_vdbl_l) >> gl_vdbl_r) - (((yend - ystart) * 2))) / 2;

	init_noise();

	if(currprefs.genlock_mix) {
		mix1 = 256 - currprefs.genlock_mix;
		mix2 = currprefs.genlock_mix;
	}

	uae_u8 r = 0, g = 0, b = 0;
	for (y = ystart; y < yend; y++) {
		int yoff = (((y * 2 + oddlines) - src->yoffset) >> vdbl);
		if (yoff < 0)
			continue;
		if (yoff >= src->inheight)
			continue;

		uae_u8 *line = src->bufmem + yoff * src->rowbytes;
		uae_u8 *dstline = dst->bufmem + (((y * 2 + oddlines) - dst->yoffset) >> vdbl) * dst->rowbytes;
		uae_u8 *line_genlock = row_map_genlock[yoff];
		int gy = ((((y * 2 + oddlines) - dst->yoffset) << gl_vdbl_l) >> gl_vdbl_r) + gl_vcenter;
		uae_u8 *image_genlock = genlock_image + gy * genlock_image_pitch;
		r = g = b = 0;
		noise_add = (quickrand() & 15) | 1;
		for (x = 0; x < src->inwidth; x++) {
			uae_u8 *s = line + x * src->pixbytes;
			uae_u8 *d = dstline + x * dst->pixbytes;
			uae_u8 *s_genlock = line_genlock + x;
			uae_u8 *s2 = s + src->rowbytes;
			uae_u8 *d2 = d + dst->rowbytes;

			if (is_transparent(*s_genlock)) {
				if (genlock_image) {
					int gx = (((x + gl_hcenter) << gl_hdbl_l) >> gl_hdbl_r);
					if (gx >= 0 && gx < genlock_image_width && gy >= 0 && gy < genlock_image_height) {
						uae_u8 *s_genlock_image = image_genlock + gx * 4;
						r = s_genlock_image[0];
						g = s_genlock_image[1];
						b = s_genlock_image[2];
					} else {
						r = g = b = 0;
					}
				} else {
					r = g = b = get_noise();
				}
				if (mix2) {
					r = (mix1 * r + mix2 * FVR(src, s)) / 256;
					g = (mix1 * g + mix2 * FVG(src, s)) / 256;
					b = (mix1 * b + mix2 * FVB(src, s)) / 256;
				}
				PUT_PRGB(d, d2, dst, r, g, b, 0, doublelines, false);
			} else {
				PUT_AMIGARGB(d, s, d2, s2, dst, 0, doublelines, false);
			}
		}
	}

	dst->nativepositioning = true;
	return true;
}

bool emulate_genlock(struct vidbuffer *src, struct vidbuffer *dst)
{
	bool v;
	if (interlace_seen) {
		if (currprefs.gfx_iscanlines) {
			v = do_genlock(src, dst, false, lof_store ? 0 : 1);
			if (v && currprefs.gfx_iscanlines > 1)
				blank_generic(src, dst, lof_store ? 1 : 0);
		} else {
			v = do_genlock(src, dst, false, 0);
			v |= do_genlock(src, dst, false, 1);
		}
	} else {
		if (currprefs.gfx_pscanlines) {
			v = do_genlock(src, dst, false, lof_store ? 0 : 1);
			if (v && currprefs.gfx_pscanlines > 1)
				blank_generic(src, dst, lof_store ? 1 : 0);
		} else {
			v = do_genlock(src, dst, true, 0);
		}
	}
	return v;
}
