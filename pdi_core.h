/*
 * pdi_core - portable PhotoDemon Image (PDI) reader + layer flattener.
 *
 * No Win32 dependencies: everything platform specific (text rendering for
 * PhotoDemon's text layers) is supplied through the PdiHost callback.
 */
#ifndef PDI_CORE_H
#define PDI_CORE_H

#include <stddef.h>
#include <stdint.h>

#define PDI_OK 0
#define PDI_ERR_FORMAT 1      /* not a (modern) PDI file / corrupt */
#define PDI_ERR_MEMORY 2
#define PDI_ERR_ABORTED 3

typedef struct PdiInfo
{
	int width;
	int height;
	int dpiX;                 /* 0 when unknown */
	int dpiY;
} PdiInfo;

/* Text-layer description handed to the host's text renderer. */
typedef struct PdiTextInfo
{
	const char *text;         /* UTF-8, entities decoded */
	const char *fontFace;     /* UTF-8 */
	unsigned char r, g, b;    /* font color */
	double fontSize;
	int sizeIsPoints;         /* 0: pixels, 1: points */
	int bold, italic, underline, strikeout;
	int alignH;               /* 0 near, 1 center, 2 far */
	int alignV;               /* 0 near, 1 center, 2 far */
	int wordWrap;             /* 0 none, 1 manual, 2 char, 3 word */
	int antialias;            /* 0 off, 1 on */
	int marginLeft, marginTop, marginRight, marginBottom;
} PdiTextInfo;

typedef struct PdiHost
{
	void *user;
	/*
	 * Render text into dst: w*h premultiplied BGRA pixels, top-down rows,
	 * pre-zeroed.  Return non-zero on success.  May be NULL (text layers are
	 * then skipped).
	 */
	int (*renderText)(void *user, const PdiTextInfo *info, int w, int h, uint8_t *dst);
	/* Optional: called with (layersDone, layersTotalEstimate); return 0 to abort. */
	int (*progress)(void *user, int done, int total);
} PdiHost;

typedef struct PdiImage
{
	int width;
	int height;
	int hasAlpha;             /* any pixel with alpha < 255 */
	uint8_t *bgra;            /* width*height*4, straight alpha, top-down; release with pdi_free_image() */
} PdiImage;

/* Non-zero if the buffer starts like a modern PDI file. Only needs ~32 bytes. */
int pdi_check(const uint8_t *buf, size_t len);

/* Reads just the image header. */
int pdi_read_info(const uint8_t *buf, size_t len, PdiInfo *info);

/* Decodes all layers and composites them into a single image. */
int pdi_render(const uint8_t *buf, size_t len, const PdiHost *host, PdiImage *out);

void pdi_free_image(PdiImage *img);

/* Blends transparent areas over a checkerboard with the given square size; result is fully opaque. */
void pdi_composite_over_checker(PdiImage *img, int cell);

#endif
