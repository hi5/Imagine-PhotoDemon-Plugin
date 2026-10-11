/* Linux test harness for pdi_core: renders a .pdi to a PNG-ready PPM/PAM. */
#include "pdi_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fakeText(void *u, const PdiTextInfo *t, int w, int h, uint8_t *dst)
{
	int x, y;
	(void)u;
	fprintf(stderr, "text hook: '%s' face='%s' size=%.1f%s rgb=%d,%d,%d bold=%d alignH=%d alignV=%d wrap=%d box=%dx%d\n",
		t->text, t->fontFace, t->fontSize, t->sizeIsPoints ? "pt" : "px", t->r, t->g, t->b, t->bold, t->alignH, t->alignV, t->wordWrap, w, h);
	/* mark the layer box with a translucent tint so placement can be checked */
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
		{
			uint8_t *p = dst + ((size_t)y * w + x) * 4;
			int edge = (x < 2 || y < 2 || x >= w - 2 || y >= h - 2);
			if (edge) { p[0] = t->b; p[1] = t->g; p[2] = t->r; p[3] = 255; }
		}
	return 1;
}

int main(int argc, char **argv)
{
	FILE *f; long n; uint8_t *buf; PdiImage img; PdiHost host; PdiInfo info; int rc;
	if (argc < 3) { fprintf(stderr, "usage: %s in.pdi out.pam [hook]\n", argv[0]); return 2; }
	f = fopen(argv[1], "rb"); if (!f) return 2;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	buf = malloc(n); if (fread(buf, 1, n, f) != (size_t)n) return 2; fclose(f);
	printf("check=%d\n", pdi_check(buf, n));
	rc = pdi_read_info(buf, n, &info);
	printf("info rc=%d %dx%d dpi=%d,%d\n", rc, info.width, info.height, info.dpiX, info.dpiY);
	memset(&host, 0, sizeof(host));
	if (argc > 3) host.renderText = fakeText;
	rc = pdi_render(buf, n, &host, &img);
	if (rc == 0 && argc > 4) pdi_composite_over_checker(&img, 8);
	printf("render rc=%d %dx%d alpha=%d\n", rc, img.width, img.height, img.hasAlpha);
	if (rc == 0)
	{
		FILE *o = fopen(argv[2], "wb"); int i, npx = img.width * img.height;
		fprintf(o, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", img.width, img.height);
		for (i = 0; i < npx; i++) { uint8_t *p = img.bgra + (size_t)i * 4; fputc(p[2], o); fputc(p[1], o); fputc(p[0], o); fputc(p[3], o); }
		fclose(o);
		pdi_free_image(&img);
	}
	free(buf);
	return rc;
}
