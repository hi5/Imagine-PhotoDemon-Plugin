/*
 * PDI plugin for Imagine (https://www.nyam.pe.kr/dev/imagine/)
 *
 * Lets Imagine open PhotoDemon Image (.pdi) files.  All layers are decoded and
 * composited into one flattened picture (opacity, blend modes, offsets and
 * PhotoDemon's non-destructive scale / rotate / shear are honoured).
 *
 * The PDI parsing/compositing lives in pdi_core.c; this file only connects it
 * to Imagine's plugin interface and renders PhotoDemon text layers via GDI.
 */
#define STRICT 1
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

#include "ImagPlug.H"
#include "pdi_core.h"

#define __UNICODE_TEXT(x) L##x
#define UNICODE_TEXT(x) __UNICODE_TEXT(x)

#define FILETYPE_PDI "PDI Image"
#define EXTENSION_PDI "pdi\0"
#define PLUGIN_NAME "PDI Format Plugin"

/* Interface version this plugin declares to Imagine. The host refuses plugins that claim a newer
 * interface than it implements, so declare the lowest version providing the calls we use (same as
 * the SDK's sample plugin). Only RegisterFileType/Create/Destroy/GetBits/GetWidthBytes/SetDPI/MessageBox are used. */
#ifndef PDI_DECLARED_INTERFACE_VERSION
#define PDI_DECLARED_INTERFACE_VERSION MAKE_IMAGINEPLUGININTERFACE_VERSION(1,5,3,0)
#endif

#define VERSION_NUMBER ((1u << 24) | (0u << 16) | (0u << 8) | 0u)

#ifdef PDI_DEBUG_LOG
#include <stdarg.h>
/* Diagnostic build only: appends to %TEMP%\\PDI_plugin_log.txt */
static void plog(const char *fmt, ...)
{
	char path[MAX_PATH + 32], line[512];
	va_list ap;
	HANDLE h;
	DWORD n, w;
	n = GetTempPathA(MAX_PATH, path);
	if (n == 0 || n > MAX_PATH) return;
	lstrcatA(path, "PDI_plugin_log.txt");
	va_start(ap, fmt);
	n = (DWORD)wvsprintfA(line, fmt, ap);
	va_end(ap);
	if (n > 500) n = 500;
	line[n++] = '\r'; line[n++] = '\n';
	h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) return;
	WriteFile(h, line, n, &w, NULL);
	CloseHandle(h);
}
#define PLOG(...) plog(__VA_ARGS__)
#else
#define PLOG(...) ((void)0)
#endif

#ifdef __cplusplus
#define EXTC extern "C"
#else
#define EXTC
#endif

/* ------------------------------------------------------------------------ */
/* text layers (GDI)                                                        */
/* ------------------------------------------------------------------------ */

static int renderTextGDI(void *user, const PdiTextInfo *ti, int w, int h, uint8_t *dst)
{
	int ok = 0;
	HDC dc = NULL;
	HBITMAP dib = NULL, oldBmp = NULL;
	HFONT font = NULL, oldFont = NULL;
	void *bits = NULL;
	WCHAR *wtext = NULL;
	WCHAR face[128];
	BITMAPINFO bmi;
	LOGFONTW lf;
	RECT rc, calc;
	UINT flags;
	int n, i, pixels;
	const unsigned char *src;

	(void)user;
	if (!ti || !ti->text || !ti->fontFace || w <= 0 || h <= 0) return 0;

	n = MultiByteToWideChar(CP_UTF8, 0, ti->text, -1, NULL, 0);
	if (n <= 0) return 0;
	wtext = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n * sizeof(WCHAR));
	if (!wtext) return 0;
	if (MultiByteToWideChar(CP_UTF8, 0, ti->text, -1, wtext, n) <= 0) goto done;

	face[0] = 0;
	MultiByteToWideChar(CP_UTF8, 0, ti->fontFace, -1, face, 127);
	face[127] = 0;

	dc = CreateCompatibleDC(NULL);
	if (!dc) goto done;

	ZeroMemory(&bmi, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = w;
	bmi.bmiHeader.biHeight = -h; /* top-down */
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!dib || !bits) goto done;
	ZeroMemory(bits, (SIZE_T)w * (SIZE_T)h * 4);
	oldBmp = (HBITMAP)SelectObject(dc, dib);

	ZeroMemory(&lf, sizeof(lf));
	{
		int px = ti->sizeIsPoints ? (int)(ti->fontSize * 96.0 / 72.0 + 0.5) : (int)(ti->fontSize + 0.5);
		if (px < 1) px = 1;
		lf.lfHeight = -px;
	}
	lf.lfWeight = ti->bold ? FW_BOLD : FW_NORMAL;
	lf.lfItalic = (BYTE)(ti->italic ? 1 : 0);
	lf.lfUnderline = (BYTE)(ti->underline ? 1 : 0);
	lf.lfStrikeOut = (BYTE)(ti->strikeout ? 1 : 0);
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_TT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = (BYTE)(ti->antialias ? ANTIALIASED_QUALITY : NONANTIALIASED_QUALITY);
	lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
	font = CreateFontIndirectW(&lf);
	if (!font) goto done;
	oldFont = (HFONT)SelectObject(dc, font);

	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(255, 255, 255));
	SetTextAlign(dc, TA_LEFT | TA_TOP);

	rc.left = ti->marginLeft;
	rc.top = ti->marginTop;
	rc.right = w - ti->marginRight;
	rc.bottom = h - ti->marginBottom;
	if (rc.right <= rc.left || rc.bottom <= rc.top)
	{
		ok = 1; /* nothing visible */
		goto done;
	}

	flags = DT_NOPREFIX | DT_NOCLIP;
	flags |= (ti->alignH == 1) ? DT_CENTER : ((ti->alignH == 2) ? DT_RIGHT : DT_LEFT);
	if (ti->wordWrap >= 2) flags |= DT_WORDBREAK;

	if (ti->alignV != 0)
	{
		int th, avail = rc.bottom - rc.top;
		calc = rc;
		DrawTextW(dc, wtext, -1, &calc, flags | DT_CALCRECT);
		th = calc.bottom - calc.top;
		if (ti->alignV == 1) rc.top += (avail - th) / 2;
		else rc.top += avail - th;
	}
	DrawTextW(dc, wtext, -1, &rc, flags);
	GdiFlush();

	/* white-on-black coverage -> premultiplied BGRA in the requested color */
	src = (const unsigned char *)bits;
	pixels = w * h;
	for (i = 0; i < pixels; i++)
	{
		unsigned b = src[i * 4], g = src[i * 4 + 1], r = src[i * 4 + 2];
		unsigned a = r > g ? r : g;
		if (b > a) a = b;
		if (a)
		{
			dst[i * 4 + 0] = (uint8_t)((ti->b * a + 127u) / 255u);
			dst[i * 4 + 1] = (uint8_t)((ti->g * a + 127u) / 255u);
			dst[i * 4 + 2] = (uint8_t)((ti->r * a + 127u) / 255u);
			dst[i * 4 + 3] = (uint8_t)a;
		}
	}
	ok = 1;

done:
	if (oldFont) SelectObject(dc, oldFont);
	if (font) DeleteObject(font);
	if (oldBmp) SelectObject(dc, oldBmp);
	if (dib) DeleteObject(dib);
	if (dc) DeleteDC(dc);
	if (wtext) HeapFree(GetProcessHeap(), 0, wtext);
	return ok;
}

/* ------------------------------------------------------------------------ */
/* Imagine file type callbacks                                              */
/* ------------------------------------------------------------------------ */

static BOOL IMAGINEAPI checkFile(IMAGINEPLUGINFILEINFOTABLE *fileInfoTable, IMAGINELOADPARAM *loadParam, int flags)
{
	BOOL r;
	(void)fileInfoTable; (void)flags;
	if (!loadParam || !loadParam->buffer) { PLOG("checkFile: no buffer"); return FALSE; }
	r = pdi_check((const uint8_t *)loadParam->buffer, (size_t)loadParam->length) ? TRUE : FALSE;
#ifdef PDI_DEBUG_LOG
	{
		const unsigned char *b = (const unsigned char *)loadParam->buffer;
		unsigned long len = (unsigned long)loadParam->length;
		PLOG("checkFile: flags=%d len=%lu first16=%02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x -> %d",
			flags, len, len>0?b[0]:0, len>1?b[1]:0, len>2?b[2]:0, len>3?b[3]:0, len>4?b[4]:0, len>5?b[5]:0, len>6?b[6]:0, len>7?b[7]:0,
			len>8?b[8]:0, len>9?b[9]:0, len>10?b[10]:0, len>11?b[11]:0, len>12?b[12]:0, len>13?b[13]:0, len>14?b[14]:0, len>15?b[15]:0, (int)r);
	}
#endif
	return r;
}

static void setDPI(const IMAGINEPLUGININTERFACE *iface, LPIMAGINEBITMAP bitmap, int dpiX, int dpiY)
{
	if (dpiX > 0 && dpiY > 0)
	{
		SIZE dpi;
		dpi.cx = dpiX;
		dpi.cy = dpiY;
		iface->lpVtbl->SetDPI(bitmap, &dpi);
	}
}

static LPIMAGINEBITMAP IMAGINEAPI loadFile(IMAGINEPLUGINFILEINFOTABLE *fileInfoTable, IMAGINELOADPARAM *loadParam, int flags)
{
	const IMAGINEPLUGININTERFACE *iface = fileInfoTable ? fileInfoTable->iface : NULL;
	const uint8_t *buffer = (const uint8_t *)loadParam->buffer;
	size_t length = (size_t)loadParam->length;
	LPIMAGINEBITMAP bitmap = NULL;
	PdiInfo info;
	int rc;

	PLOG("loadFile: flags=0x%x len=%lu iface=%p", flags, (unsigned long)length, (void *)iface);
	if (!iface || !buffer) return NULL;

	rc = pdi_read_info(buffer, length, &info);
	PLOG("loadFile: pdi_read_info rc=%d %dx%d", rc, info.width, info.height);
	if (rc != PDI_OK)
	{
		loadParam->errorCode = (rc == PDI_ERR_MEMORY) ? IMAGINEERROR_OUTOFMEMORY : IMAGINEERROR_INVALIDDATA;
		return NULL;
	}

	if (flags & IMAGINELOADPARAM_GETINFO)
	{
		/* Only the header was requested. */
		bitmap = iface->lpVtbl->Create(info.width, info.height, 32, flags);
		if (!bitmap)
		{
			loadParam->errorCode = IMAGINEERROR_OUTOFMEMORY;
			return NULL;
		}
		setDPI(iface, bitmap, info.dpiX, info.dpiY);
		return bitmap;
	}

	{
		PdiHost host;
		PdiImage img;
		LONG bitCount, widthBytes;
		BYTE *bits;
		int y, bpp, keepGoing = 1;
		IMAGINECALLBACKPARAM cb;

		host.user = NULL;
		host.renderText = renderTextGDI;
		host.progress = NULL;

		rc = pdi_render(buffer, length, &host, &img);
		PLOG("loadFile: pdi_render rc=%d alpha=%d", rc, img.hasAlpha);
		if (rc != PDI_OK)
		{
			loadParam->errorCode = (rc == PDI_ERR_MEMORY) ? IMAGINEERROR_OUTOFMEMORY :
				(rc == PDI_ERR_ABORTED ? IMAGINEERROR_ABORTED : IMAGINEERROR_INVALIDDATA);
			return NULL;
		}

#ifndef PDI_CHECKER_CELL
#define PDI_CHECKER_CELL 8   /* set to 0 to hand over real (straight) alpha instead */
#endif
#if PDI_CHECKER_CELL > 0
		/* Show transparent areas over a checkerboard, as most image viewers do. */
		if (img.hasAlpha) pdi_composite_over_checker(&img, PDI_CHECKER_CELL);
#endif
		/* Opaque pictures are handed over as 24 bit, the rest as 32 bit with straight alpha. */
		bitCount = img.hasAlpha ? 32 : 24;
		bpp = (int)(bitCount / 8);
		bitmap = iface->lpVtbl->Create(img.width, img.height, bitCount, flags);
		if (!bitmap)
		{
			pdi_free_image(&img);
			loadParam->errorCode = IMAGINEERROR_OUTOFMEMORY;
			return NULL;
		}

		PLOG("loadFile: Create(%d,%d,%d) -> %p", img.width, img.height, (int)bitCount, (void *)bitmap);
		bits = (BYTE *)iface->lpVtbl->GetBits(bitmap);
		widthBytes = iface->lpVtbl->GetWidthBytes(bitmap);
		if (!bits || widthBytes <= 0)
		{
			iface->lpVtbl->Destroy(bitmap);
			pdi_free_image(&img);
			loadParam->errorCode = IMAGINEERROR_UNKNOWNERROR;
			return NULL;
		}

		cb.dib = bitmap;
		cb.param = loadParam->callback.param;
		cb.current = 0;
		cb.overall = img.height - 1;

		for (y = 0; y < img.height && keepGoing; y++)
		{
			/* Imagine bitmaps are bottom-up in memory, PDI canvas rows are top-down. */
			BYTE *dstRow = bits + (size_t)(img.height - 1 - y) * (size_t)widthBytes;
			const uint8_t *srcRow = img.bgra + (size_t)y * (size_t)img.width * 4u;

			if (bpp == 4)
			{
				memcpy(dstRow, srcRow, (size_t)img.width * 4u);
			}
			else
			{
				int x;
				for (x = 0; x < img.width; x++)
				{
					dstRow[x * 3 + 0] = srcRow[x * 4 + 0];
					dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
					dstRow[x * 3 + 2] = srcRow[x * 4 + 2];
				}
			}

			if ((flags & IMAGINELOADPARAM_CALLBACK) && loadParam->callback.proc)
			{
				cb.current = y;
				if (!loadParam->callback.proc(&cb))
				{
					loadParam->errorCode = IMAGINEERROR_ABORTED;
					keepGoing = 0;
				}
			}
		}

		if (img.hasAlpha) loadParam->caps |= IMAGINECAPS_ALPHA;
		pdi_free_image(&img);

		if (!keepGoing)
		{
			iface->lpVtbl->Destroy(bitmap);
			return NULL;
		}

		setDPI(iface, bitmap, info.dpiX, info.dpiY);
		PLOG("loadFile: done OK");
	}

	return bitmap;
}

static BOOL IMAGINEAPI saveFile(IMAGINEPLUGINFILEINFOTABLE *fileInfoTable, LPIMAGINEBITMAP bitmap, IMAGINESAVEPARAM *saveParam, int flags)
{
	(void)fileInfoTable; (void)bitmap; (void)flags;
	if (saveParam) saveParam->errorCode = IMAGINEERROR_UNSUPPORTEDTYPE;
	return FALSE; /* read-only plugin */
}

/* ------------------------------------------------------------------------ */
/* plugin registration                                                      */
/* ------------------------------------------------------------------------ */

static const IMAGINEFILEINFOITEM fileInfoItemA =
{
	checkFile,
	loadFile,
	saveFile,
	FILETYPE_PDI,
	EXTENSION_PDI,
};

static const IMAGINEFILEINFOITEM fileInfoItemW =
{
	checkFile,
	loadFile,
	saveFile,
	(LPCTSTR)UNICODE_TEXT(FILETYPE_PDI),
	(LPCTSTR)UNICODE_TEXT(EXTENSION_PDI),
};

static BOOL IMAGINEAPI registerProcA(const IMAGINEPLUGININTERFACE *iface)
{
	BOOL r = (iface->lpVtbl->RegisterFileType(&fileInfoItemA) != NULL) ? TRUE : FALSE;
	PLOG("registerProcA -> %d", (int)r);
	return r;
}

static BOOL IMAGINEAPI registerProcW(const IMAGINEPLUGININTERFACE *iface)
{
	BOOL r = (iface->lpVtbl->RegisterFileType(&fileInfoItemW) != NULL) ? TRUE : FALSE;
	PLOG("registerProcW -> %d", (int)r);
	return r;
}

static BOOL IMAGINEAPI infoProcA(const IMAGINEPLUGININTERFACE *iface, HINSTANCE hInstance, HWND parent)
{
	(void)hInstance;
	iface->lpVtbl->MessageBox(parent,
		(LPCTSTR)"PhotoDemon PDI (flattened) plugin for Imagine\n\nOpens PhotoDemon Image (.pdi) files as a single flattened picture.\nSee https://github.com/tannerhelland/PhotoDemon for the format.",
		(LPCTSTR)"PDI", MB_OK | MB_ICONINFORMATION);
	return TRUE;
}

static BOOL IMAGINEAPI infoProcW(const IMAGINEPLUGININTERFACE *iface, HINSTANCE hInstance, HWND parent)
{
	(void)hInstance;
	iface->lpVtbl->MessageBox(parent,
		(LPCTSTR)L"PhotoDemon PDI (flattened) plugin for Imagine\n\nOpens PhotoDemon Image (.pdi) files as a single flattened picture.\nSee https://github.com/tannerhelland/PhotoDemon for the format.",
		(LPCTSTR)L"PDI", MB_OK | MB_ICONINFORMATION);
	return TRUE;
}

static const IMAGINEPLUGININFOA pluginInfoA =
{
	sizeof(IMAGINEPLUGININFOA),
	registerProcA,
	VERSION_NUMBER,
	PLUGIN_NAME,
	PDI_DECLARED_INTERFACE_VERSION,
	NULL,
	infoProcA,
};

static const IMAGINEPLUGININFOW pluginInfoW =
{
	sizeof(IMAGINEPLUGININFOW),
	registerProcW,
	VERSION_NUMBER,
	UNICODE_TEXT(PLUGIN_NAME),
	PDI_DECLARED_INTERFACE_VERSION,
	NULL,
	infoProcW,
};

EXTC __declspec(dllexport) BOOL IMAGINEAPI ImaginePluginGetInfoA(IMAGINEPLUGININFOA *dest)
{
	PLOG("ImaginePluginGetInfoA called (%d-bit) dest->size on entry=%lu, ours=%lu, declared iface=0x%08lx", (int)(sizeof(void *) * 8), (unsigned long)dest->size, (unsigned long)sizeof(IMAGINEPLUGININFOA), (unsigned long)PDI_DECLARED_INTERFACE_VERSION);
	*dest = pluginInfoA;
	return TRUE;
}

EXTC __declspec(dllexport) BOOL IMAGINEAPI ImaginePluginGetInfoW(IMAGINEPLUGININFOW *dest)
{
	PLOG("ImaginePluginGetInfoW called (%d-bit) dest->size on entry=%lu, ours=%lu, declared iface=0x%08lx", (int)(sizeof(void *) * 8), (unsigned long)dest->size, (unsigned long)sizeof(IMAGINEPLUGININFOW), (unsigned long)PDI_DECLARED_INTERFACE_VERSION);
	*dest = pluginInfoW;
	return TRUE;
}
