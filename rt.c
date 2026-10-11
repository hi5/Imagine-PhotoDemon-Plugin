/*
 * Minimal freestanding runtime so the plugin has NO dependency on any C
 * runtime DLL (msvcrt/ucrt/mingw): only kernel32, user32 and gdi32 are used.
 */
#include <windows.h>
#include <stddef.h>

void *malloc(size_t n)
{
	return HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
}

void *calloc(size_t a, size_t b)
{
	if (b && a > (size_t)-1 / b) return NULL;
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (a * b) ? (a * b) : 1);
}

void *realloc(void *p, size_t n)
{
	if (!p) return malloc(n);
	return HeapReAlloc(GetProcessHeap(), 0, p, n ? n : 1);
}

void free(void *p)
{
	if (p) HeapFree(GetProcessHeap(), 0, p);
}

void *memcpy(void *d, const void *s, size_t n)
{
	void *r = d;
	__asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	return r;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *dp = (unsigned char *)d;
	const unsigned char *sp = (const unsigned char *)s;
	if (dp == sp || n == 0) return d;
	if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
	dp += n - 1;
	sp += n - 1;
	__asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(dp), "+S"(sp), "+c"(n) : : "memory");
	return d;
}

void *memset(void *d, int c, size_t n)
{
	void *r = d;
	__asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"((unsigned char)c) : "memory");
	return r;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
	while (n--)
	{
		if (*p != *q) return (int)*p - (int)*q;
		p++; q++;
	}
	return 0;
}

size_t strlen(const char *s)
{
	const char *p = s;
	while (*p) p++;
	return (size_t)(p - s);
}

/* DLL entry point (the linker is told to use this name; no CRT start-up code is linked). */
BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD reason, LPVOID reserved)
{
	(void)h; (void)reason; (void)reserved;
	return TRUE;
}
