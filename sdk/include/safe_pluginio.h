#ifndef SAFE_PLUGINIO_H
#define SAFE_PLUGINIO_H

#include <lwran.h>
#include <string.h>

#ifdef __GNUC__
#define SPI_UNUSED __attribute__((unused))
#else
#define SPI_UNUSED
#endif

static int
SPI_UNUSED
spi_read_line(const LWLoadState *ls, char *buf, int buflen)
{
	int n;

	if (!buf || buflen < 1)
		return 0;

	buf[0] = '\0';
	n = (*ls->read)(ls->readData, buf, buflen - 1);
	if (n < 0) {
		buf[0] = '\0';
		return 0;
	}

	buf[buflen - 1] = '\0';
	if (n < buflen)
		buf[n] = '\0';

	return n;
}

static void
SPI_UNUSED
spi_write_line(const LWSaveState *ss, const char *buf)
{
	(*ss->write)(ss->writeData, (char *)buf, (int)strlen(buf) + 1);
}

static int
SPI_UNUSED
spi_read_u32be(const LWLoadState *ls, unsigned long *value)
{
	unsigned char buf[4];
	int n;

	n = (*ls->read)(ls->readData, (char *)buf, 4);
	if (n != 4)
		return 0;

	*value = ((unsigned long)buf[0] << 24)
	       | ((unsigned long)buf[1] << 16)
	       | ((unsigned long)buf[2] << 8)
	       | (unsigned long)buf[3];

	return 1;
}

static int
SPI_UNUSED
spi_read_i32be(const LWLoadState *ls, int *value)
{
	unsigned long u;

	if (!spi_read_u32be(ls, &u))
		return 0;

	*value = (int)u;
	return 1;
}

static void
SPI_UNUSED
spi_write_u32be(const LWSaveState *ss, unsigned long value)
{
	unsigned char buf[4];

	buf[0] = (unsigned char)((value >> 24) & 0xFFu);
	buf[1] = (unsigned char)((value >> 16) & 0xFFu);
	buf[2] = (unsigned char)((value >> 8) & 0xFFu);
	buf[3] = (unsigned char)(value & 0xFFu);

	(*ss->write)(ss->writeData, (char *)buf, 4);
}

static void
SPI_UNUSED
spi_write_i32be(const LWSaveState *ss, int value)
{
	spi_write_u32be(ss, (unsigned long)value);
}

static int
SPI_UNUSED
spi_read_string_record(const LWLoadState *ls, char *buf, int buflen)
{
	if (!buf || buflen < 1)
		return 0;

	if (ls->ioMode == LWIO_SCENE) {
		return spi_read_line(ls, buf, buflen);
	} else {
		unsigned long len, keep, left;
		char discard[16];

		buf[0] = '\0';
		if (!spi_read_u32be(ls, &len))
			return 0;

		keep = len;
		if (keep >= (unsigned long)buflen)
			keep = (unsigned long)buflen - 1;

		if (keep > 0) {
			if ((*ls->read)(ls->readData, buf, (int)keep) != (int)keep) {
				buf[0] = '\0';
				return 0;
			}
		}
		buf[keep] = '\0';

		left = len - keep;
		while (left > 0) {
			int chunk = (left > sizeof(discard)) ? (int)sizeof(discard)
			                                     : (int)left;
			if ((*ls->read)(ls->readData, discard, chunk) != chunk)
				return 0;
			left -= (unsigned long)chunk;
		}

		return (int)keep;
	}
}

static void
SPI_UNUSED
spi_write_string_record(const LWSaveState *ss, const char *buf)
{
	if (ss->ioMode == LWIO_SCENE) {
		spi_write_line(ss, buf);
	} else {
		unsigned long len = (unsigned long)strlen(buf);
		spi_write_u32be(ss, len);
		if (len > 0)
			(*ss->write)(ss->writeData, (char *)buf, (int)len);
	}
}

/*
 * LightWave 5.00r's FilterAccess setRGB returns with a6 changed, although
 * a6 is callee-saved in the m68k calling convention (bufLine and setAlpha
 * do preserve it). GCC may keep a loop pointer in a6 across the call; the
 * corrupted pointer then hangs or crashes the filter. Call setRGB through
 * spi_set_rgb, which saves a6 around the host call.
 */
#ifdef __HAVE_68881__
#define SPI_FP_CLOBBERS , "fp0", "fp1"
#else
#define SPI_FP_CLOBBERS
#endif

static void
SPI_UNUSED
spi_set_rgb(const FilterAccess *fa, int x, int y, BufferValue rgb[3])
{
#ifdef __GNUC__
	void (*fn)(int, int, BufferValue *) = fa->setRGB;

	__asm__ __volatile__(
		"move.l a6,-(sp)\n\t"
		"move.l %3,-(sp)\n\t"
		"move.l %2,-(sp)\n\t"
		"move.l %1,-(sp)\n\t"
		"jsr (%0)\n\t"
		"lea 12(sp),sp\n\t"
		"move.l (sp)+,a6"
		:
		: "a" (fn), "r" (x), "r" (y), "r" (rgb)
		: "d0", "d1", "a0", "a1", "cc", "memory" SPI_FP_CLOBBERS);
#else
	(*fa->setRGB)(x, y, rgb);
#endif
}

#endif
