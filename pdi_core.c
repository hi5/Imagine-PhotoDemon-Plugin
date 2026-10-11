/*
 * pdi_core - portable PhotoDemon Image (PDI) reader + layer flattener.
 *
 * File format reference: PhotoDemon (https://github.com/tannerhelland/PhotoDemon)
 *   Classes/pdPackageChunky.cls  - chunk container
 *   Modules/ImageLoader.bas      - LoadPDI_Normal(): chunk order / semantics
 *   Classes/pdLayer.cls          - layer header XML + affine transform order
 *
 * Layout recap:
 *   "PDPKCHNK" + 4-byte encoder id ("PDIF") followed by chunks:
 *   id[4], int32 size, then if size>0: comp[4] ("none"/"zstd"/"defl"/"zlib"),
 *   int32 uncompressedSize, int32 compressedSize, payload.
 *   IHDR (image header xml), then repeated LHDR (layer header xml) + LDAT
 *   (layer data), ..., PEND.  Raster layer data = premultiplied BGRA DIB bits
 *   (bottom-up rows).  Text layers store their text properties as xml.
 */
#include "pdi_core.h"

#include <stdlib.h>
#include <string.h>

#include "zstd.h"

/* ------------------------------------------------------------------------ */
/* small helpers                                                            */
/* ------------------------------------------------------------------------ */

#define PDI_MAX_DIM 65535
#define PDI_MAX_CHUNK_BYTES (1u << 30)

static int32_t rd32(const uint8_t *p)
{
	return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static double my_floor(double v)
{
	double t = (double)(long long)v;
	return (t > v) ? t - 1.0 : t;
}

/* sin/cos of an angle in degrees (no libm available in the plugin build). */
static void sincos_deg(double deg, double *s, double *c)
{
	const double pi = 3.14159265358979323846;
	double x, x2, sn, cs, t;
	int quadrant;

	/* reduce to [0,360) */
	deg -= 360.0 * my_floor(deg / 360.0);
	quadrant = (int)(deg / 90.0);
	if (quadrant > 3) quadrant = 3;
	x = (deg - 90.0 * quadrant) * pi / 180.0; /* [0, pi/2] */
	x2 = x * x;

	/* Taylor series, plenty of terms for [0, pi/2] */
	t = x; sn = x;
	t *= -x2 / (2 * 3);   sn += t;
	t *= -x2 / (4 * 5);   sn += t;
	t *= -x2 / (6 * 7);   sn += t;
	t *= -x2 / (8 * 9);   sn += t;
	t *= -x2 / (10 * 11); sn += t;
	t *= -x2 / (12 * 13); sn += t;
	t *= -x2 / (14 * 15); sn += t;

	t = 1.0; cs = 1.0;
	t *= -x2 / (1 * 2);   cs += t;
	t *= -x2 / (3 * 4);   cs += t;
	t *= -x2 / (5 * 6);   cs += t;
	t *= -x2 / (7 * 8);   cs += t;
	t *= -x2 / (9 * 10);  cs += t;
	t *= -x2 / (11 * 12); cs += t;
	t *= -x2 / (13 * 14); cs += t;

	switch (quadrant)
	{
	case 0: *s = sn;  *c = cs;  break;
	case 1: *s = cs;  *c = -sn; break;
	case 2: *s = -sn; *c = -cs; break;
	default: *s = -cs; *c = sn; break;
	}
}

static uint8_t mul255(unsigned a, unsigned b)
{
	unsigned t = a * b + 128u;
	return (uint8_t)((t + (t >> 8)) >> 8);
}

/* ------------------------------------------------------------------------ */
/* inflate (RFC1951) - only used for the rarely seen "defl"/"zlib" chunks   */
/* ------------------------------------------------------------------------ */

typedef struct Infl
{
	const uint8_t *in;
	size_t inLen, inPos;
	uint32_t bitBuf;
	int bitCnt;
	uint8_t *out;
	size_t outLen, outPos;
} Infl;

typedef struct Huff
{
	short count[16];
	short symbol[288];
} Huff;

static int infl_bits(Infl *s, int need)
{
	uint32_t v = s->bitBuf;
	while (s->bitCnt < need)
	{
		if (s->inPos >= s->inLen) return -1;
		v |= (uint32_t)s->in[s->inPos++] << s->bitCnt;
		s->bitCnt += 8;
	}
	s->bitBuf = v >> need;
	s->bitCnt -= need;
	return (int)(v & ((1u << need) - 1u));
}

static int infl_decode(Infl *s, const Huff *h)
{
	int code = 0, first = 0, index = 0, len;
	for (len = 1; len <= 15; len++)
	{
		int b = infl_bits(s, 1), cnt;
		if (b < 0) return -1;
		code |= b;
		cnt = h->count[len];
		if (code - cnt < first) return h->symbol[index + (code - first)];
		index += cnt;
		first += cnt;
		first <<= 1;
		code <<= 1;
	}
	return -2;
}

static int infl_construct(Huff *h, const short *length, int n)
{
	int symbol, len, left;
	short offs[16];
	for (len = 0; len <= 15; len++) h->count[len] = 0;
	for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
	if (h->count[0] == n) return 0;
	left = 1;
	for (len = 1; len <= 15; len++)
	{
		left <<= 1;
		left -= h->count[len];
		if (left < 0) return left;
	}
	offs[1] = 0;
	for (len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
	for (symbol = 0; symbol < n; symbol++)
		if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
	return left;
}

static int infl_codes(Infl *s, const Huff *lencode, const Huff *distcode)
{
	static const short lens[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
	static const short lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
	static const short dists[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
	static const short dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
	int symbol;
	do
	{
		symbol = infl_decode(s, lencode);
		if (symbol < 0) return -1;
		if (symbol < 256)
		{
			if (s->outPos >= s->outLen) return -2;
			s->out[s->outPos++] = (uint8_t)symbol;
		}
		else if (symbol > 256)
		{
			int len, dist, b;
			size_t i;
			symbol -= 257;
			if (symbol >= 29) return -3;
			b = infl_bits(s, lext[symbol]);
			if (b < 0) return -1;
			len = lens[symbol] + b;
			symbol = infl_decode(s, distcode);
			if (symbol < 0 || symbol >= 30) return -3;
			b = infl_bits(s, dext[symbol]);
			if (b < 0) return -1;
			dist = dists[symbol] + b;
			if ((size_t)dist > s->outPos) return -3;
			if (s->outPos + (size_t)len > s->outLen) return -2;
			for (i = 0; i < (size_t)len; i++)
			{
				s->out[s->outPos] = s->out[s->outPos - (size_t)dist];
				s->outPos++;
			}
		}
	} while (symbol != 256);
	return 0;
}

static int infl_run(Infl *s)
{
	int last;
	do
	{
		int type;
		last = infl_bits(s, 1);
		type = infl_bits(s, 2);
		if (last < 0 || type < 0) return -1;
		if (type == 0)
		{
			unsigned len, nlen;
			s->bitBuf = 0;
			s->bitCnt = 0;
			if (s->inPos + 4 > s->inLen) return -1;
			len = s->in[s->inPos] | (s->in[s->inPos + 1] << 8);
			nlen = s->in[s->inPos + 2] | (s->in[s->inPos + 3] << 8);
			s->inPos += 4;
			if (len != (~nlen & 0xFFFFu)) return -1;
			if (s->inPos + len > s->inLen || s->outPos + len > s->outLen) return -1;
			memcpy(s->out + s->outPos, s->in + s->inPos, len);
			s->inPos += len;
			s->outPos += len;
		}
		else if (type == 1 || type == 2)
		{
			Huff lencode, distcode;
			short lengths[320];
			int sym;
			if (type == 1)
			{
				for (sym = 0; sym < 144; sym++) lengths[sym] = 8;
				for (; sym < 256; sym++) lengths[sym] = 9;
				for (; sym < 280; sym++) lengths[sym] = 7;
				for (; sym < 288; sym++) lengths[sym] = 8;
				infl_construct(&lencode, lengths, 288);
				for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
				infl_construct(&distcode, lengths, 30);
			}
			else
			{
				static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
				int nlen = infl_bits(s, 5), ndist = infl_bits(s, 5), ncode = infl_bits(s, 4), idx;
				if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
				nlen += 257; ndist += 1; ncode += 4;
				if (nlen > 286 || ndist > 30) return -1;
				for (idx = 0; idx < ncode; idx++)
				{
					int b = infl_bits(s, 3);
					if (b < 0) return -1;
					lengths[order[idx]] = (short)b;
				}
				for (; idx < 19; idx++) lengths[order[idx]] = 0;
				if (infl_construct(&lencode, lengths, 19) != 0) return -1;
				idx = 0;
				while (idx < nlen + ndist)
				{
					int symbol = infl_decode(s, &lencode), len, rep;
					if (symbol < 0) return -1;
					if (symbol < 16)
					{
						lengths[idx++] = (short)symbol;
					}
					else
					{
						len = 0;
						if (symbol == 16)
						{
							if (idx == 0) return -1;
							len = lengths[idx - 1];
							rep = 3 + infl_bits(s, 2);
						}
						else if (symbol == 17) rep = 3 + infl_bits(s, 3);
						else rep = 11 + infl_bits(s, 7);
						if (rep < 3 || idx + rep > nlen + ndist) return -1;
						while (rep--) lengths[idx++] = (short)len;
					}
				}
				if (lengths[256] == 0) return -1;
				if (infl_construct(&lencode, lengths, nlen) < 0) return -1;
				if (infl_construct(&distcode, lengths + nlen, ndist) < 0) return -1;
			}
			if (infl_codes(s, &lencode, &distcode) != 0) return -1;
		}
		else return -1;
	} while (!last);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* chunk container                                                          */
/* ------------------------------------------------------------------------ */

typedef struct Reader
{
	const uint8_t *buf;
	size_t len, pos;
} Reader;

typedef struct Chunk
{
	const uint8_t *id;     /* 4 bytes */
	const uint8_t *data;   /* payload after the 8 byte id+size header */
	int32_t size;
} Chunk;

static int idEq(const uint8_t *id, const char *s)
{
	return id[0] == (uint8_t)s[0] && id[1] == (uint8_t)s[1] && id[2] == (uint8_t)s[2] && id[3] == (uint8_t)s[3];
}

static int header_ok(const uint8_t *buf, size_t len)
{
	return len >= 12 && memcmp(buf, "PDPKCHNK", 8) == 0 && memcmp(buf + 8, "PDIF", 4) == 0;
}

/* returns 1 and fills c, or 0 at PEND / end / truncated file */
static int next_chunk(Reader *r, Chunk *c)
{
	int32_t size;
	if (r->pos + 8 > r->len) return 0;
	size = rd32(r->buf + r->pos + 4);
	if (size < 0) return 0;
	if ((uint64_t)r->pos + 8u + (uint64_t)size > (uint64_t)r->len) return 0;
	c->id = r->buf + r->pos;
	c->data = r->buf + r->pos + 8;
	c->size = size;
	r->pos += 8 + (size_t)size;
	if (idEq(c->id, "PEND")) return 0;
	return 1;
}

/*
 * Returns a malloc'ed buffer holding the uncompressed chunk payload
 * (NUL terminated for convenience, the NUL is not counted in *outLen).
 * *outLen == 0 / NULL result with PDI_OK for an empty chunk.
 */
static int chunk_payload(const Chunk *c, uint8_t **out, size_t *outLen)
{
	int32_t usz, csz;
	uint8_t *dst;
	const uint8_t *src;

	*out = NULL;
	*outLen = 0;
	if (c->size == 0) return PDI_OK;
	if (c->size < 12) return PDI_ERR_FORMAT;

	usz = rd32(c->data + 4);
	csz = rd32(c->data + 8);
	if (usz <= 0 || csz <= 0 || (uint32_t)usz > PDI_MAX_CHUNK_BYTES || csz > c->size - 12) return PDI_ERR_FORMAT;
	src = c->data + 12;

	dst = (uint8_t *)malloc((size_t)usz + 1);
	if (!dst) return PDI_ERR_MEMORY;

	if (memcmp(c->data, "none", 4) == 0)
	{
		if (csz < usz) { free(dst); return PDI_ERR_FORMAT; }
		memcpy(dst, src, (size_t)usz);
	}
	else if (memcmp(c->data, "zstd", 4) == 0)
	{
		size_t r = ZSTD_decompress(dst, (size_t)usz, src, (size_t)csz);
		if (ZSTD_isError(r) || r != (size_t)usz) { free(dst); return PDI_ERR_FORMAT; }
	}
	else if (memcmp(c->data, "defl", 4) == 0 || memcmp(c->data, "zlib", 4) == 0)
	{
		Infl s;
		size_t skip = (c->data[0] == 'z') ? 2 : 0;
		memset(&s, 0, sizeof(s));
		if ((size_t)csz < skip) { free(dst); return PDI_ERR_FORMAT; }
		s.in = src + skip;
		s.inLen = (size_t)csz - skip;
		s.out = dst;
		s.outLen = (size_t)usz;
		if (infl_run(&s) != 0 || s.outPos != (size_t)usz) { free(dst); return PDI_ERR_FORMAT; }
	}
	else
	{
		free(dst);
		return PDI_ERR_FORMAT;
	}

	dst[usz] = 0;
	*out = dst;
	*outLen = (size_t)usz;
	return PDI_OK;
}

/* ------------------------------------------------------------------------ */
/* tiny xml-ish parameter parser: <name>value</name>                        */
/* ------------------------------------------------------------------------ */

static int xml_get(const char *xml, size_t len, const char *name, const char **val, size_t *vlen)
{
	size_t nl = strlen(name), i;
	if (len < nl + 2) return 0;
	for (i = 0; i + nl + 2 <= len; i++)
	{
		if (xml[i] == '<' && xml[i + 1 + nl] == '>' && memcmp(xml + i + 1, name, nl) == 0)
		{
			size_t start = i + nl + 2, j;
			/* find closing tag "</name>" */
			for (j = start; j + nl + 3 <= len; j++)
			{
				if (xml[j] == '<' && xml[j + 1] == '/' && xml[j + 2 + nl] == '>' && memcmp(xml + j + 2, name, nl) == 0)
				{
					*val = xml + start;
					*vlen = j - start;
					return 1;
				}
			}
			return 0;
		}
	}
	return 0;
}

static double parse_double(const char *s, size_t n)
{
	size_t i = 0;
	double v = 0.0, frac = 0.1, sign = 1.0;
	while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
	if (i < n && (s[i] == '-' || s[i] == '+')) { if (s[i] == '-') sign = -1.0; i++; }
	while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10.0 + (s[i] - '0'); i++; }
	if (i < n && (s[i] == '.' || s[i] == ','))
	{
		i++;
		while (i < n && s[i] >= '0' && s[i] <= '9') { v += (s[i] - '0') * frac; frac *= 0.1; i++; }
	}
	if (i < n && (s[i] == 'e' || s[i] == 'E' || s[i] == 'd' || s[i] == 'D'))
	{
		int esign = 1, e = 0;
		i++;
		if (i < n && (s[i] == '-' || s[i] == '+')) { if (s[i] == '-') esign = -1; i++; }
		while (i < n && s[i] >= '0' && s[i] <= '9') { e = e * 10 + (s[i] - '0'); if (e > 400) e = 400; i++; }
		while (e-- > 0) { if (esign > 0) v *= 10.0; else v *= 0.1; }
	}
	return sign * v;
}

static double xml_double(const char *xml, size_t len, const char *name, double def)
{
	const char *v; size_t vl;
	return xml_get(xml, len, name, &v, &vl) ? parse_double(v, vl) : def;
}

static int xml_int(const char *xml, size_t len, const char *name, int def)
{
	double d = xml_double(xml, len, name, (double)def);
	if (d > 2147483000.0) d = 2147483000.0;
	if (d < -2147483000.0) d = -2147483000.0;
	return (int)(d < 0 ? d - 0.5 : d + 0.5);
}

static int xml_bool(const char *xml, size_t len, const char *name, int def)
{
	const char *v; size_t vl;
	if (!xml_get(xml, len, name, &v, &vl)) return def;
	while (vl && (*v == ' ')) { v++; vl--; }
	if (!vl) return def;
	return (v[0] == 'T' || v[0] == 't' || v[0] == '1' || v[0] == '-');
}

/* 4 character code, padded with blanks (PhotoDemon pads short ids with spaces) */
static void xml_fourcc(const char *xml, size_t len, const char *name, char out[4])
{
	const char *v; size_t vl, i;
	out[0] = out[1] = out[2] = out[3] = ' ';
	if (!xml_get(xml, len, name, &v, &vl)) { out[0] = 0; return; }
	for (i = 0; i < 4 && i < vl; i++) out[i] = v[i];
}

/* Decodes XML entities into UTF-8.  Result is malloc'ed, NUL terminated. */
static char *xml_unescape(const char *s, size_t n)
{
	char *out = (char *)malloc(n + 8); /* decoded text is never longer than the escaped text */
	size_t i = 0, o = 0;
	if (!out) return NULL;
	while (i < n)
	{
		if (s[i] == '&')
		{
			size_t j = i + 1;
			while (j < n && j - i < 12 && s[j] != ';') j++;
			if (j < n && s[j] == ';')
			{
				size_t L = j - i - 1;
				const char *e = s + i + 1;
				unsigned cp = 0xFFFFFFFFu;
				if (L == 2 && !memcmp(e, "lt", 2)) cp = '<';
				else if (L == 2 && !memcmp(e, "gt", 2)) cp = '>';
				else if (L == 3 && !memcmp(e, "amp", 3)) cp = '&';
				else if (L == 4 && !memcmp(e, "quot", 4)) cp = '"';
				else if (L == 4 && !memcmp(e, "apos", 4)) cp = '\'';
				else if (L >= 2 && e[0] == '#')
				{
					size_t k = 1;
					unsigned v = 0;
					int hex = (e[1] == 'x' || e[1] == 'X');
					if (hex) k = 2;
					for (; k < L; k++)
					{
						char ch = e[k];
						if (ch >= '0' && ch <= '9') v = v * (hex ? 16 : 10) + (unsigned)(ch - '0');
						else if (hex && ch >= 'a' && ch <= 'f') v = v * 16 + (unsigned)(ch - 'a' + 10);
						else if (hex && ch >= 'A' && ch <= 'F') v = v * 16 + (unsigned)(ch - 'A' + 10);
						else { v = 0xFFFFFFFFu; break; }
					}
					cp = v;
				}
				if (cp != 0xFFFFFFFFu && cp < 0x110000u)
				{
					/* a decoded char needs at most 4 bytes, an entity at least 3 input bytes */
					if (cp < 0x80) out[o++] = (char)cp;
					else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
					else if (cp < 0x10000) { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
					else { out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
					i = j + 1;
					continue;
				}
			}
		}
		out[o++] = s[i++];
	}
	out[o] = 0;
	return out;
}

/* ------------------------------------------------------------------------ */
/* layer description                                                        */
/* ------------------------------------------------------------------------ */

enum
{
	LT_RASTER = 0, LT_TEXT_BASIC, LT_OTHER
};

enum
{
	BM_NORMAL = 0, BM_DARKEN, BM_MULTIPLY, BM_COLORBURN, BM_LINEARBURN, BM_LIGHTEN, BM_SCREEN,
	BM_COLORDODGE, BM_LINEARDODGE, BM_OVERLAY, BM_SOFTLIGHT, BM_HARDLIGHT, BM_VIVIDLIGHT,
	BM_LINEARLIGHT, BM_PINLIGHT, BM_HARDMIX, BM_DIFFERENCE, BM_EXCLUSION, BM_SUBTRACT, BM_DIVIDE,
	BM_HUE, BM_SATURATION, BM_COLOR, BM_LUMINOSITY, BM_GRAINEXTRACT, BM_GRAINMERGE,
	BM_ERASE, BM_BEHIND, BM_OVERWRITE
};

enum { AM_NORMAL = 0, AM_LOCKED, AM_INHERIT };

typedef struct LayerInfo
{
	int valid;
	int type;
	int visible;
	double opacity;       /* 0..100 */
	int blend;
	int alphaMode;
	int offX, offY;
	double sx, sy, shx, shy, angle, rcx, rcy;
	int nearest;
	int depth;            /* 24 or 32 */
	int w, h;
	int stride;
	int premult;
} LayerInfo;

static int blend_from_string(const char c[4])
{
	static const struct { const char *k; int m; } tbl[] = {
		{"norm", BM_NORMAL}, {"dark", BM_DARKEN}, {"mult", BM_MULTIPLY}, {"cbrn", BM_COLORBURN},
		{"lbrn", BM_LINEARBURN}, {"lght", BM_LIGHTEN}, {"scrn", BM_SCREEN}, {"cddg", BM_COLORDODGE},
		{"lddg", BM_LINEARDODGE}, {"ovrl", BM_OVERLAY}, {"sftl", BM_SOFTLIGHT}, {"hrdl", BM_HARDLIGHT},
		{"vvdl", BM_VIVIDLIGHT}, {"lnrl", BM_LINEARLIGHT}, {"pinl", BM_PINLIGHT}, {"hrdm", BM_HARDMIX},
		{"diff", BM_DIFFERENCE}, {"excl", BM_EXCLUSION}, {"subt", BM_SUBTRACT}, {"divd", BM_DIVIDE},
		{"hue ", BM_HUE}, {"satr", BM_SATURATION}, {"clr ", BM_COLOR}, {"lumn", BM_LUMINOSITY},
		{"gext", BM_GRAINEXTRACT}, {"gmrg", BM_GRAINMERGE}, {"eras", BM_ERASE}, {"bhnd", BM_BEHIND},
		{"copy", BM_OVERWRITE}
	};
	size_t i;
	for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
		if (!memcmp(tbl[i].k, c, 4)) return tbl[i].m;
	return BM_NORMAL;
}

static void parse_layer_header(const char *x, size_t n, LayerInfo *L)
{
	char fc[4];
	memset(L, 0, sizeof(*L));
	xml_fourcc(x, n, "type", fc);
	if (fc[0] == 0 || !memcmp(fc, "rast", 4)) L->type = LT_RASTER;
	else if (!memcmp(fc, "txtb", 4)) L->type = LT_TEXT_BASIC;
	else L->type = LT_OTHER;

	L->visible = xml_bool(x, n, "visible", 1);
	L->opacity = xml_double(x, n, "opacity", 100.0);
	if (L->opacity < 0) L->opacity = 0;
	if (L->opacity > 100) L->opacity = 100;

	xml_fourcc(x, n, "blend-mode", fc);
	L->blend = (fc[0] == 0) ? BM_NORMAL : blend_from_string(fc);
	xml_fourcc(x, n, "alpha-mode", fc);
	if (fc[0] != 0 && !memcmp(fc, "lock", 4)) L->alphaMode = AM_LOCKED;
	else if (fc[0] != 0 && !memcmp(fc, "inhr", 4)) L->alphaMode = AM_INHERIT;
	else L->alphaMode = AM_NORMAL;

	L->offX = xml_int(x, n, "offset-x", 0);
	L->offY = xml_int(x, n, "offset-y", 0);
	L->sx = xml_double(x, n, "stretch-x", 1.0);
	L->sy = xml_double(x, n, "stretch-y", 1.0);
	L->shx = xml_double(x, n, "shear-x", 0.0);
	L->shy = xml_double(x, n, "shear-y", 0.0);
	L->angle = xml_double(x, n, "angle", 0.0);
	L->rcx = xml_double(x, n, "rotation-center-x", 0.5);
	L->rcy = xml_double(x, n, "rotation-center-y", 0.5);

	xml_fourcc(x, n, "sample-quality", fc);
	L->nearest = (fc[0] != 0 && !memcmp(fc, "near", 4));

	L->depth = xml_int(x, n, "px-color-depth", 32);
	L->w = xml_int(x, n, "px-width", 0);
	L->h = xml_int(x, n, "px-height", 0);
	L->stride = xml_int(x, n, "px-stride", 0);
	L->premult = xml_bool(x, n, "px-alpha-premultiplied", 1);

	L->valid = (L->w > 0 && L->h > 0 && L->w <= PDI_MAX_DIM && L->h <= PDI_MAX_DIM && (L->depth == 32 || L->depth == 24));
	if (L->valid)
	{
		int bpp = L->depth / 8;
		if (L->stride < L->w * bpp) L->stride = (L->w * bpp + 3) & ~3; /* DIB default stride */
	}
}

/* ------------------------------------------------------------------------ */
/* blending                                                                 */
/* ------------------------------------------------------------------------ */

static float lum3(const float *c) { return 0.3f * c[2] + 0.59f * c[1] + 0.11f * c[0]; } /* c = b,g,r */

static void clip_color(float *c)
{
	float l = lum3(c);
	float n = c[0] < c[1] ? (c[0] < c[2] ? c[0] : c[2]) : (c[1] < c[2] ? c[1] : c[2]);
	float x = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
	int i;
	if (n < 0.0f && l - n != 0.0f) for (i = 0; i < 3; i++) c[i] = l + (c[i] - l) * l / (l - n);
	if (x > 1.0f && x - l != 0.0f) for (i = 0; i < 3; i++) c[i] = l + (c[i] - l) * (1.0f - l) / (x - l);
}

static void set_lum(const float *c, float l, float *out)
{
	float d = l - lum3(c);
	out[0] = c[0] + d; out[1] = c[1] + d; out[2] = c[2] + d;
	clip_color(out);
}

static float sat3(const float *c)
{
	float n = c[0] < c[1] ? (c[0] < c[2] ? c[0] : c[2]) : (c[1] < c[2] ? c[1] : c[2]);
	float x = c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]);
	return x - n;
}

static void set_sat(const float *c, float s, float *out)
{
	int imax = 0, imin = 0, imid, i;
	for (i = 1; i < 3; i++) { if (c[i] > c[imax]) imax = i; if (c[i] < c[imin]) imin = i; }
	if (imax == imin) { out[0] = out[1] = out[2] = 0.0f; return; }
	imid = 3 - imax - imin;
	out[imid] = (c[imid] - c[imin]) * s / (c[imax] - c[imin]);
	out[imax] = s;
	out[imin] = 0.0f;
}

static float blend_sep(int mode, float cb, float cs)
{
	switch (mode)
	{
	case BM_DARKEN: return cb < cs ? cb : cs;
	case BM_MULTIPLY: return cb * cs;
	case BM_COLORBURN:
		if (cb >= 1.0f) return 1.0f;
		if (cs <= 0.0f) return 0.0f;
		return 1.0f - clampf((1.0f - cb) / cs, 0.0f, 1.0f);
	case BM_LINEARBURN: return clampf(cb + cs - 1.0f, 0.0f, 1.0f);
	case BM_LIGHTEN: return cb > cs ? cb : cs;
	case BM_SCREEN: return cb + cs - cb * cs;
	case BM_COLORDODGE:
		if (cb <= 0.0f) return 0.0f;
		if (cs >= 1.0f) return 1.0f;
		return clampf(cb / (1.0f - cs), 0.0f, 1.0f);
	case BM_LINEARDODGE: return clampf(cb + cs, 0.0f, 1.0f);
	case BM_OVERLAY: return blend_sep(BM_HARDLIGHT, cs, cb);
	case BM_HARDLIGHT:
		if (cs <= 0.5f) return cb * 2.0f * cs;
		{ float s2 = 2.0f * cs - 1.0f; return cb + s2 - cb * s2; }
	case BM_SOFTLIGHT:
		if (cs <= 0.5f) return cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb);
		{
			float d = (cb <= 0.25f) ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb : (float)__builtin_sqrt((double)cb);
			return cb + (2.0f * cs - 1.0f) * (d - cb);
		}
	case BM_VIVIDLIGHT:
		if (cs < 0.5f) return blend_sep(BM_COLORBURN, cb, 2.0f * cs);
		return blend_sep(BM_COLORDODGE, cb, 2.0f * (cs - 0.5f));
	case BM_LINEARLIGHT: return clampf(cb + 2.0f * cs - 1.0f, 0.0f, 1.0f);
	case BM_PINLIGHT:
		if (cs < 0.5f) { float t = 2.0f * cs; return cb < t ? cb : t; }
		{ float t = 2.0f * cs - 1.0f; return cb > t ? cb : t; }
	case BM_HARDMIX: return (cb + cs >= 1.0f) ? 1.0f : 0.0f;
	case BM_DIFFERENCE: return cb > cs ? cb - cs : cs - cb;
	case BM_EXCLUSION: return cb + cs - 2.0f * cb * cs;
	case BM_SUBTRACT: return cb > cs ? cb - cs : 0.0f;
	case BM_DIVIDE: return cs <= 0.0f ? 1.0f : clampf(cb / cs, 0.0f, 1.0f);
	case BM_GRAINEXTRACT: return clampf(cb - cs + 0.5f, 0.0f, 1.0f);
	case BM_GRAINMERGE: return clampf(cb + cs - 0.5f, 0.0f, 1.0f);
	default: return cs;
	}
}

/* cb/cs are b,g,r triples (straight alpha, 0..1) */
static void blend_color(int mode, const float *cb, const float *cs, float *out)
{
	float t[3];
	int i;
	switch (mode)
	{
	case BM_HUE:
		set_sat(cs, sat3(cb), t); set_lum(t, lum3(cb), out); return;
	case BM_SATURATION:
		set_sat(cb, sat3(cs), t); set_lum(t, lum3(cb), out); return;
	case BM_COLOR:
		set_lum(cs, lum3(cb), out); return;
	case BM_LUMINOSITY:
		set_lum(cb, lum3(cs), out); return;
	default:
		for (i = 0; i < 3; i++) out[i] = blend_sep(mode, cb[i], cs[i]);
	}
}

static uint8_t f2b(float v)
{
	int i = (int)(v * 255.0f + 0.5f);
	return (uint8_t)clampi(i, 0, 255);
}

/*
 * d: canvas pixel (premultiplied BGRA), s: layer pixel (premultiplied BGRA)
 */
static void blend_pixel(uint8_t *d, const uint8_t *s, const LayerInfo *L, unsigned opq)
{
	if (L->blend == BM_NORMAL && L->alphaMode == AM_NORMAL)
	{
		unsigned sa = mul255(s[3], opq), inv;
		if (sa == 0) return;
		inv = 255u - sa;
		if (sa == 255u)
		{
			d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
			return;
		}
		{
			unsigned v;
			v = mul255(s[0], opq) + mul255(d[0], inv); d[0] = (uint8_t)(v > 255 ? 255 : v);
			v = mul255(s[1], opq) + mul255(d[1], inv); d[1] = (uint8_t)(v > 255 ? 255 : v);
			v = mul255(s[2], opq) + mul255(d[2], inv); d[2] = (uint8_t)(v > 255 ? 255 : v);
			v = sa + mul255(d[3], inv);                d[3] = (uint8_t)(v > 255 ? 255 : v);
		}
		return;
	}

	{
		const float op = (float)(L->opacity / 100.0);
		float sa = (s[3] / 255.0f) * op;
		float ab = d[3] / 255.0f;
		float cs[3], cb[3], bl[3];
		int i;

		if (s[3] == 0 && L->blend != BM_OVERWRITE) return;

		if (L->blend == BM_ERASE)
		{
			float k = 1.0f - sa;
			for (i = 0; i < 4; i++) d[i] = f2b((d[i] / 255.0f) * k);
			return;
		}
		if (L->blend == BM_OVERWRITE)
		{
			for (i = 0; i < 4; i++) d[i] = f2b((d[i] / 255.0f) * (1.0f - op) + (s[i] / 255.0f) * op);
			return;
		}
		if (L->blend == BM_BEHIND)
		{
			float k = (1.0f - ab) * op;
			for (i = 0; i < 4; i++) d[i] = f2b((d[i] / 255.0f) + (s[i] / 255.0f) * k);
			return;
		}

		if (L->alphaMode == AM_INHERIT) sa *= ab;
		if (sa <= 0.0f) return;

		for (i = 0; i < 3; i++)
		{
			cs[i] = s[3] ? clampf((float)s[i] / (float)s[3], 0.0f, 1.0f) : 0.0f;
			cb[i] = d[3] ? clampf((float)d[i] / (float)d[3], 0.0f, 1.0f) : 0.0f;
		}

		if (d[3] == 0)
		{
			bl[0] = cs[0]; bl[1] = cs[1]; bl[2] = cs[2];
		}
		else
		{
			blend_color(L->blend, cb, cs, bl);
		}

		if (L->alphaMode == AM_LOCKED)
		{
			if (d[3] == 0) return;
			for (i = 0; i < 3; i++) d[i] = f2b(ab * ((1.0f - sa) * cb[i] + sa * clampf(bl[i], 0.0f, 1.0f)));
			return;
		}

		{
			float ar = sa + ab * (1.0f - sa);
			for (i = 0; i < 3; i++)
			{
				float p = (1.0f - sa) * (d[i] / 255.0f) + sa * (1.0f - ab) * cs[i] + sa * ab * clampf(bl[i], 0.0f, 1.0f);
				d[i] = f2b(p);
			}
			d[3] = f2b(ar);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* compositing a layer onto the canvas                                      */
/* ------------------------------------------------------------------------ */

typedef struct Src
{
	const uint8_t *base;
	int w, h;
	int stride;
	int topDown;
	int bpp;      /* 3 or 4 */
} Src;

static const uint8_t *src_row(const Src *s, int y)
{
	int row = s->topDown ? y : (s->h - 1 - y);
	return s->base + (size_t)row * (size_t)s->stride;
}

static void src_fetch(const Src *s, const uint8_t *row, int x, uint8_t out[4])
{
	const uint8_t *p = row + (size_t)x * (size_t)s->bpp;
	out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
	out[3] = (s->bpp == 4) ? p[3] : 255;
}

static void composite_layer(uint8_t *cv, int cw, int ch, const LayerInfo *L, const Src *src)
{
	unsigned opq = (unsigned)clampi((int)(L->opacity * 2.55 + 0.5), 0, 255);
	int identity = (L->sx == 1.0 && L->sy == 1.0 && L->angle == 0.0 && L->shx == 0.0 && L->shy == 0.0);

	if (opq == 0) return;

	if (identity)
	{
		int x0 = L->offX < 0 ? 0 : L->offX;
		int y0 = L->offY < 0 ? 0 : L->offY;
		int x1, y1, x, y;
		long long ex = (long long)L->offX + src->w, ey = (long long)L->offY + src->h;
		x1 = (int)(ex > cw ? cw : ex);
		y1 = (int)(ey > ch ? ch : ey);
		for (y = y0; y < y1; y++)
		{
			const uint8_t *row = src_row(src, y - L->offY);
			uint8_t *drow = cv + (size_t)y * (size_t)cw * 4;
			for (x = x0; x < x1; x++)
			{
				uint8_t px[4];
				src_fetch(src, row, x - L->offX, px);
				blend_pixel(drow + (size_t)x * 4, px, L, opq);
			}
		}
		return;
	}

	{
		/* Forward matrix in column-vector form: x' = a*x + c*y + e ; y' = b*x + d*y + f.
		 * Order (PhotoDemon pdLayer.GetCopyOfLayerTransformationMatrix_Full):
		 * scale -> rotate (about scaled-layer center) -> shear (same center) -> translate. */
		double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
		double scaledW = src->w * L->sx, scaledH = src->h * L->sy;
		double cx = scaledW * L->rcx, cy = scaledH * L->rcy;
		double mnx, mny, mxx, mxy, px[4], py[4];
		double det, ia, ib, ic, id, ie, jf;
		int i, x0, y0, x1, y1, x, y;

		/* scale */
		a *= L->sx; c *= L->sx; e *= L->sx;
		b *= L->sy; d *= L->sy; f *= L->sy;

		if (L->angle != 0.0)
		{
			double sn, cs, na, nb, nc, nd, ne, nf;
			sincos_deg(L->angle, &sn, &cs);
			/* T(p) = center + R*(p-center) */
			na = cs * a - sn * b;   nb = sn * a + cs * b;
			nc = cs * c - sn * d;   nd = sn * c + cs * d;
			ne = cs * (e - cx) - sn * (f - cy) + cx;
			nf = sn * (e - cx) + cs * (f - cy) + cy;
			a = na; b = nb; c = nc; d = nd; e = ne; f = nf;
		}
		if (L->shx != 0.0 || L->shy != 0.0)
		{
			/* x' = x + shx*y ; y' = y + shy*x (relative to center, simultaneous) */
			double na, nb, nc, nd, ne, nf;
			na = a + L->shx * b;  nb = b + L->shy * a;
			nc = c + L->shx * d;  nd = d + L->shy * c;
			ne = (e - cx) + L->shx * (f - cy) + cx;
			nf = (f - cy) + L->shy * (e - cx) + cy;
			a = na; b = nb; c = nc; d = nd; e = ne; f = nf;
		}
		e += L->offX; f += L->offY;

		det = a * d - b * c;
		if (det > -1e-12 && det < 1e-12) return;
		ia = d / det;  ib = -b / det;
		ic = -c / det; id = a / det;
		ie = -(ia * e + ic * f);
		jf = -(ib * e + id * f);

		px[0] = e;                       py[0] = f;
		px[1] = a * src->w + e;          py[1] = b * src->w + f;
		px[2] = a * src->w + c * src->h + e; py[2] = b * src->w + d * src->h + f;
		px[3] = c * src->h + e;          py[3] = d * src->h + f;
		mnx = mxx = px[0]; mny = mxy = py[0];
		for (i = 1; i < 4; i++)
		{
			if (px[i] < mnx) mnx = px[i];
			if (px[i] > mxx) mxx = px[i];
			if (py[i] < mny) mny = py[i];
			if (py[i] > mxy) mxy = py[i];
		}
		if (mxx < 0 || mxy < 0 || mnx > cw || mny > ch) return;
		x0 = (mnx <= 0.0) ? 0 : (int)my_floor(mnx);
		y0 = (mny <= 0.0) ? 0 : (int)my_floor(mny);
		x1 = (mxx >= (double)cw) ? cw : (int)my_floor(mxx) + 1;
		y1 = (mxy >= (double)ch) ? ch : (int)my_floor(mxy) + 1;
		if (x1 > cw) x1 = cw;
		if (y1 > ch) y1 = ch;

		for (y = y0; y < y1; y++)
		{
			uint8_t *drow = cv + (size_t)y * (size_t)cw * 4;
			for (x = x0; x < x1; x++)
			{
				double dx = x + 0.5, dy = y + 0.5;
				double u = ia * dx + ic * dy + ie;
				double v = ib * dx + id * dy + jf;
				uint8_t spx[4];

				if (u < 0.0 || v < 0.0 || u >= src->w || v >= src->h) continue;

				if (L->nearest)
				{
					src_fetch(src, src_row(src, (int)v), (int)u, spx);
				}
				else
				{
					double fx = u - 0.5, fy = v - 0.5;
					int ix = (int)my_floor(fx), iy = (int)my_floor(fy);
					float tx = (float)(fx - ix), ty = (float)(fy - iy);
					int ix0 = clampi(ix, 0, src->w - 1), ix1 = clampi(ix + 1, 0, src->w - 1);
					int iy0 = clampi(iy, 0, src->h - 1), iy1 = clampi(iy + 1, 0, src->h - 1);
					uint8_t p00[4], p10[4], p01[4], p11[4];
					int k;
					const uint8_t *r0 = src_row(src, iy0), *r1 = src_row(src, iy1);
					src_fetch(src, r0, ix0, p00); src_fetch(src, r0, ix1, p10);
					src_fetch(src, r1, ix0, p01); src_fetch(src, r1, ix1, p11);
					for (k = 0; k < 4; k++)
					{
						float top = p00[k] + (p10[k] - p00[k]) * tx;
						float bot = p01[k] + (p11[k] - p01[k]) * tx;
						float val = top + (bot - top) * ty;
						spx[k] = (uint8_t)clampi((int)(val + 0.5f), 0, 255);
					}
				}
				blend_pixel(drow + (size_t)x * 4, spx, L, opq);
			}
		}
	}
}

/* ------------------------------------------------------------------------ */
/* text layers                                                              */
/* ------------------------------------------------------------------------ */

static int align_from_string(const char *v, size_t n)
{
	if (n >= 6 && !memcmp(v, "center", 6)) return 1;
	if (n >= 3 && !memcmp(v, "far", 3)) return 2;
	return 0;
}

static int wrap_from_string(const char *v, size_t n)
{
	if (n >= 4 && !memcmp(v, "word", 4)) return 3;
	if (n >= 4 && !memcmp(v, "char", 4)) return 2;
	if (n >= 6 && !memcmp(v, "manual", 6)) return 1;
	return 0;
}

/* Returns PDI_OK and fills a malloc'ed pixel buffer (premult BGRA, top-down) or NULL if text can't be drawn. */
static uint8_t *render_text_layer(const PdiHost *host, const LayerInfo *L, const char *xml, size_t xmlLen)
{
	const char *v; size_t vl;
	char *inner = NULL, *text = NULL, *face = NULL;
	uint8_t *pix = NULL;
	PdiTextInfo ti;
	size_t bytes;
	int ok = 0;

	if (!host || !host->renderText) return NULL;
	if (!xml_get(xml, xmlLen, "vector-text-data", &v, &vl)) return NULL;
	inner = xml_unescape(v, vl);
	if (!inner) return NULL;
	{
		size_t il = strlen(inner);
		long color;
		memset(&ti, 0, sizeof(ti));

		if (xml_get(inner, il, "obj-text", &v, &vl)) text = xml_unescape(v, vl);
		if (xml_get(inner, il, "obj-font-face", &v, &vl)) face = xml_unescape(v, vl);
		if (!text || !face) goto done;
		ti.text = text;
		ti.fontFace = face;

		color = (long)xml_double(inner, il, "obj-font-color", 0.0);
		ti.r = (unsigned char)(color & 255);
		ti.g = (unsigned char)((color >> 8) & 255);
		ti.b = (unsigned char)((color >> 16) & 255);

		ti.fontSize = xml_double(inner, il, "obj-font-size", 16.0);
		if (ti.fontSize < 1.0) ti.fontSize = 1.0;
		if (xml_get(inner, il, "obj-font-size-unit", &v, &vl)) ti.sizeIsPoints = (vl >= 2 && v[0] == 'p' && v[1] == 't');
		ti.bold = xml_bool(inner, il, "obj-font-bold", 0);
		ti.italic = xml_bool(inner, il, "obj-font-italic", 0);
		ti.underline = xml_bool(inner, il, "obj-font-underling", xml_bool(inner, il, "obj-font-underline", 0));
		ti.strikeout = xml_bool(inner, il, "obj-font-strikeout", 0);
		if (xml_get(inner, il, "obj-align-horizontal", &v, &vl)) ti.alignH = align_from_string(v, vl);
		if (xml_get(inner, il, "obj-align-vertical", &v, &vl)) ti.alignV = align_from_string(v, vl);
		if (xml_get(inner, il, "obj-wordwrap-mode", &v, &vl)) ti.wordWrap = wrap_from_string(v, vl); else ti.wordWrap = 3;
		ti.antialias = 1;
		if (xml_get(inner, il, "obj-antialiasing-type", &v, &vl) && vl >= 4 && !memcmp(v, "none", 4)) ti.antialias = 0;
		ti.marginLeft = xml_int(inner, il, "obj-margin-left", 0);
		ti.marginTop = xml_int(inner, il, "obj-margin-top", 0);
		ti.marginRight = xml_int(inner, il, "obj-margin-right", 0);
		ti.marginBottom = xml_int(inner, il, "obj-margin-bottom", 0);
	}

	bytes = (size_t)L->w * (size_t)L->h * 4u;
	pix = (uint8_t *)calloc(1, bytes);
	if (!pix) goto done;
	ok = host->renderText(host->user, &ti, L->w, L->h, pix);
done:
	free(inner); free(text); free(face);
	if (!ok) { free(pix); return NULL; }
	return pix;
}

/* ------------------------------------------------------------------------ */
/* public api                                                               */
/* ------------------------------------------------------------------------ */

int pdi_check(const uint8_t *buf, size_t len)
{
	return len >= 20 && header_ok(buf, len) && idEq(buf + 12, "IHDR");
}

static int parse_ihdr(const uint8_t *buf, size_t len, Reader *rd, PdiInfo *info, int *layerCount)
{
	Chunk c;
	uint8_t *xml = NULL;
	size_t xl = 0;
	int rc;
	double dx, dy;

	if (!pdi_check(buf, len)) return PDI_ERR_FORMAT;
	rd->buf = buf; rd->len = len; rd->pos = 12;
	if (!next_chunk(rd, &c) || !idEq(c.id, "IHDR")) return PDI_ERR_FORMAT;
	rc = chunk_payload(&c, &xml, &xl);
	if (rc != PDI_OK) return rc;
	if (!xml) return PDI_ERR_FORMAT;

	info->width = xml_int((const char *)xml, xl, "width", 0);
	info->height = xml_int((const char *)xml, xl, "height", 0);
	dx = xml_double((const char *)xml, xl, "x-resolution", 0.0);
	dy = xml_double((const char *)xml, xl, "y-resolution", 0.0);
	if (dx <= 0.0) dx = xml_double((const char *)xml, xl, "dpi", 0.0);
	if (dy <= 0.0) dy = dx;
	info->dpiX = (dx >= 1.0 && dx < 100000.0) ? (int)(dx + 0.5) : 0;
	info->dpiY = (dy >= 1.0 && dy < 100000.0) ? (int)(dy + 0.5) : 0;
	if (layerCount) *layerCount = xml_int((const char *)xml, xl, "layer-count-active", 0);
	free(xml);

	if (info->width < 1 || info->height < 1 || info->width > PDI_MAX_DIM || info->height > PDI_MAX_DIM) return PDI_ERR_FORMAT;
	return PDI_OK;
}

int pdi_read_info(const uint8_t *buf, size_t len, PdiInfo *info)
{
	Reader rd;
	return parse_ihdr(buf, len, &rd, info, NULL);
}

void pdi_free_image(PdiImage *img)
{
	if (img)
	{
		free(img->bgra);
		img->bgra = NULL;
	}
}

int pdi_render(const uint8_t *buf, size_t len, const PdiHost *host, PdiImage *out)
{
	Reader rd;
	PdiInfo info;
	Chunk c;
	LayerInfo L;
	uint8_t *canvas = NULL;
	size_t canvasBytes;
	int rc, totalLayers = 0, done = 0, aborted = 0;

	memset(out, 0, sizeof(*out));
	memset(&L, 0, sizeof(L));

	rc = parse_ihdr(buf, len, &rd, &info, &totalLayers);
	if (rc != PDI_OK) return rc;

	{
		uint64_t b = (uint64_t)info.width * (uint64_t)info.height * 4u;
		if (b > 0x7FFFFFFFull || b > (uint64_t)(size_t)-1 / 2) return PDI_ERR_MEMORY;
		canvasBytes = (size_t)b;
	}
	canvas = (uint8_t *)calloc(1, canvasBytes);
	if (!canvas) return PDI_ERR_MEMORY;

	while (!aborted && next_chunk(&rd, &c))
	{
		if (idEq(c.id, "LHDR"))
		{
			uint8_t *xml = NULL; size_t xl = 0;
			memset(&L, 0, sizeof(L));
			rc = chunk_payload(&c, &xml, &xl);
			if (rc == PDI_ERR_MEMORY) { free(canvas); return rc; }
			if (rc == PDI_OK && xml)
			{
				parse_layer_header((const char *)xml, xl, &L);
				free(xml);
			}
		}
		else if (idEq(c.id, "LDAT"))
		{
			if (L.valid)
			{
				LayerInfo cur = L;
				L.valid = 0;

				if (cur.visible && cur.type != LT_OTHER)
				{
					uint8_t *data = NULL; size_t dl = 0;
					rc = chunk_payload(&c, &data, &dl);
					if (rc == PDI_ERR_MEMORY) { free(canvas); return rc; }
					if (rc == PDI_OK && data)
					{
						Src src;
						uint8_t *textPix = NULL;
						int draw = 0;

						src.w = cur.w; src.h = cur.h;
						if (cur.type == LT_RASTER)
						{
							int bpp = cur.depth / 8;
							if ((uint64_t)cur.stride * (uint64_t)cur.h <= (uint64_t)dl)
							{
								if (bpp == 4 && !cur.premult)
								{
									size_t i, n = (size_t)cur.stride * (size_t)cur.h;
									/* convert straight alpha to premultiplied, in place */
									for (i = 0; i + 4 <= n; i += 4)
									{
										uint8_t a = data[i + 3];
										if (a != 255)
										{
											data[i] = mul255(data[i], a);
											data[i + 1] = mul255(data[i + 1], a);
											data[i + 2] = mul255(data[i + 2], a);
										}
									}
								}
								src.base = data; src.stride = cur.stride; src.topDown = 1; src.bpp = bpp; /* PhotoDemon DIBs are top-down (pdDIB: Height = -imgHeight) */
								draw = 1;
							}
						}
						else if (cur.type == LT_TEXT_BASIC)
						{
							textPix = render_text_layer(host, &cur, (const char *)data, dl);
							if (textPix)
							{
								src.base = textPix; src.stride = cur.w * 4; src.topDown = 1; src.bpp = 4;
								draw = 1;
							}
						}

						if (draw) composite_layer(canvas, info.width, info.height, &cur, &src);
						free(textPix);
						free(data);
					}
				}

				done++;
				if (host && host->progress && !host->progress(host->user, done, totalLayers)) aborted = 1;
			}
		}
		/* anything else (metadata, unknown chunks) is skipped */
	}

	if (aborted)
	{
		free(canvas);
		return PDI_ERR_ABORTED;
	}

	/* premultiplied -> straight alpha, detect transparency */
	{
		size_t i, n = canvasBytes;
		int hasAlpha = 0;
		for (i = 0; i + 4 <= n; i += 4)
		{
			uint8_t a = canvas[i + 3];
			if (a == 255) continue;
			hasAlpha = 1;
			if (a == 0)
			{
				canvas[i] = canvas[i + 1] = canvas[i + 2] = 0;
			}
			else
			{
				unsigned k;
				for (k = 0; k < 3; k++)
				{
					unsigned v = ((unsigned)canvas[i + k] * 255u + a / 2u) / a;
					canvas[i + k] = (uint8_t)(v > 255u ? 255u : v);
				}
			}
		}
		out->hasAlpha = hasAlpha;
	}

	out->width = info.width;
	out->height = info.height;
	out->bgra = canvas;
	return PDI_OK;
}

/* Composites a straight-alpha image over a light/grey checkerboard (the usual "transparent" backdrop). */
void pdi_composite_over_checker(PdiImage *img, int cell)
{
	int x, y;
	if (!img || !img->bgra || cell < 1) return;
	for (y = 0; y < img->height; y++)
	{
		uint8_t *p = img->bgra + (size_t)y * (size_t)img->width * 4u;
		for (x = 0; x < img->width; x++, p += 4)
		{
			unsigned a = p[3], bg, k;
			if (a == 255) continue;
			bg = (((x / cell) + (y / cell)) & 1) ? 204u : 255u;
			for (k = 0; k < 3; k++)
				p[k] = (uint8_t)(((unsigned)p[k] * a + bg * (255u - a) + 127u) / 255u);
			p[3] = 255;
		}
	}
	img->hasAlpha = 0;
}
