/*
 * PNGSAVER.C -- PNG Image Saver for LightWave 3D
 *
 * Saves rendered frames as compressed PNG files using the ImSaverLocal
 * interface.  Includes a self-contained deflate compressor (LZ77 +
 * fixed Huffman) and applies the Sub filter per scanline for better
 * compression.  No external library dependencies.
 *
 * Uses AllocMem/FreeMem and AmigaOS DOS — no libnix runtime.
 */

#include <splug.h>
#include <image.h>

#include <string.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <exec/memory.h>

extern struct DosLibrary *DOSBase;
extern struct ExecBase   *SysBase;

/* AmigaOS version string */
static const char __attribute__((used)) verstag[] =
	"\0$VER: PNG_saver " PLUGIN_VERSION " (c) D. Panokostas";

/* ----------------------------------------------------------------
 * Memory helpers
 * ---------------------------------------------------------------- */

static void *
plugin_alloc(unsigned long size)
{
	unsigned long *p;
	p = (unsigned long *)AllocMem(size + 4, MEMF_PUBLIC | MEMF_CLEAR);
	if (!p) return 0;
	*p = size + 4;
	return (void *)(p + 1);
}

static void
plugin_free(void *ptr)
{
	unsigned long *p;
	if (!ptr) return;
	p = ((unsigned long *)ptr) - 1;
	FreeMem(p, *p);
}

/* ----------------------------------------------------------------
 * CRC32
 * ---------------------------------------------------------------- */

static unsigned long crc_table[256];
static int crc_table_ready = 0;

static void
make_crc_table(void)
{
	unsigned long c;
	int n, k;
	for (n = 0; n < 256; n++) {
		c = (unsigned long)n;
		for (k = 0; k < 8; k++) {
			if (c & 1) c = 0xEDB88320UL ^ (c >> 1);
			else       c = c >> 1;
		}
		crc_table[n] = c;
	}
	crc_table_ready = 1;
}

static unsigned long
update_crc(unsigned long crc, const unsigned char *buf, int len)
{
	int n;
	for (n = 0; n < len; n++)
		crc = crc_table[(crc ^ buf[n]) & 0xFF] ^ (crc >> 8);
	return crc;
}

/* ----------------------------------------------------------------
 * Adler-32
 * ---------------------------------------------------------------- */

#define ADLER_MOD 65521

static void
adler32_update(unsigned long *s1, unsigned long *s2,
               const unsigned char *buf, int len)
{
	int i;
	for (i = 0; i < len; i++) {
		*s1 = (*s1 + buf[i]) % ADLER_MOD;
		*s2 = (*s2 + *s1) % ADLER_MOD;
	}
}

/* ----------------------------------------------------------------
 * Big-endian helpers
 * ---------------------------------------------------------------- */

static void
put_be32(unsigned char *p, unsigned long v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)(v);
}

/* ----------------------------------------------------------------
 * File I/O
 * ---------------------------------------------------------------- */

static int
file_write(BPTR fh, const void *buf, int len)
{
	return (Write(fh, (APTR)buf, len) == len);
}

/* ----------------------------------------------------------------
 * PNG chunk writer
 * ---------------------------------------------------------------- */

static int
write_chunk(BPTR fh, const unsigned char *type,
            const unsigned char *data, unsigned long len)
{
	unsigned char buf[4];
	unsigned long crc;

	put_be32(buf, len);
	if (!file_write(fh, buf, 4)) return 0;
	if (!file_write(fh, type, 4)) return 0;

	crc = 0xFFFFFFFFUL;
	crc = update_crc(crc, type, 4);
	if (len > 0) {
		if (!file_write(fh, data, (int)len)) return 0;
		crc = update_crc(crc, data, (int)len);
	}
	crc ^= 0xFFFFFFFFUL;
	put_be32(buf, crc);
	if (!file_write(fh, buf, 4)) return 0;
	return 1;
}

/* ----------------------------------------------------------------
 * PNG signature, IHDR, IEND
 * ---------------------------------------------------------------- */

static const unsigned char png_sig[8] = {
	137, 80, 78, 71, 13, 10, 26, 10
};

static int
write_ihdr(BPTR fh, int w, int h)
{
	unsigned char ihdr[13];
	static const unsigned char t[4] = { 'I','H','D','R' };
	put_be32(ihdr, (unsigned long)w);
	put_be32(ihdr + 4, (unsigned long)h);
	ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
	return write_chunk(fh, t, ihdr, 13);
}

static int
write_iend(BPTR fh)
{
	static const unsigned char t[4] = { 'I','E','N','D' };
	return write_chunk(fh, t, 0, 0);
}

/* ----------------------------------------------------------------
 * Bit writer  (LSB-first packing, as deflate requires)
 * ---------------------------------------------------------------- */

typedef struct {
	unsigned char *buf;
	unsigned long  cap;
	unsigned long  pos;
	unsigned long  bits;
	int            nbits;
} BitWriter;

static void
bw_init(BitWriter *bw, unsigned char *buf, unsigned long cap)
{
	bw->buf   = buf;
	bw->cap   = cap;
	bw->pos   = 0;
	bw->bits  = 0;
	bw->nbits = 0;
}

static void
bw_putbits(BitWriter *bw, unsigned int val, int n)
{
	bw->bits |= (unsigned long)val << bw->nbits;
	bw->nbits += n;
	while (bw->nbits >= 8) {
		if (bw->pos < bw->cap)
			bw->buf[bw->pos++] = (unsigned char)(bw->bits & 0xFF);
		bw->bits >>= 8;
		bw->nbits -= 8;
	}
}

static void
bw_flush(BitWriter *bw)
{
	if (bw->nbits > 0 && bw->pos < bw->cap)
		bw->buf[bw->pos++] = (unsigned char)(bw->bits & 0xFF);
	bw->bits  = 0;
	bw->nbits = 0;
}

/* ----------------------------------------------------------------
 * Fixed Huffman tables  (RFC 1951 section 3.2.6)
 *
 * Pre-computed reversed codes for LSB-first bit writing.
 * ---------------------------------------------------------------- */

typedef struct { unsigned short code; unsigned char bits; } HuffCode;

static HuffCode fixed_lit[288];
static HuffCode fixed_dist[30];
static int fixed_tables_ready = 0;

static unsigned short
reverse_bits(unsigned short v, int n)
{
	unsigned short r = 0;
	int i;
	for (i = 0; i < n; i++) {
		r = (unsigned short)((r << 1) | (v & 1));
		v >>= 1;
	}
	return r;
}

static void
build_fixed_tables(void)
{
	int i;
	/* literals/lengths 0-287 */
	for (i = 0; i <= 143; i++) {
		fixed_lit[i].bits = 8;
		fixed_lit[i].code = reverse_bits((unsigned short)(0x30 + i), 8);
	}
	for (i = 144; i <= 255; i++) {
		fixed_lit[i].bits = 9;
		fixed_lit[i].code = reverse_bits((unsigned short)(0x190 + i - 144), 9);
	}
	for (i = 256; i <= 279; i++) {
		fixed_lit[i].bits = 7;
		fixed_lit[i].code = reverse_bits((unsigned short)(i - 256), 7);
	}
	for (i = 280; i <= 287; i++) {
		fixed_lit[i].bits = 8;
		fixed_lit[i].code = reverse_bits((unsigned short)(0xC0 + i - 280), 8);
	}
	/* distances 0-29 */
	for (i = 0; i < 30; i++) {
		fixed_dist[i].bits = 5;
		fixed_dist[i].code = reverse_bits((unsigned short)i, 5);
	}
	fixed_tables_ready = 1;
}

/* ----------------------------------------------------------------
 * Length / distance tables  (RFC 1951 section 3.2.5)
 * ---------------------------------------------------------------- */

static const unsigned short len_base[29] = {
	3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
	35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const unsigned char len_extra[29] = {
	0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
	3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const unsigned short dist_base[30] = {
	1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
	257,385,513,769,1025,1537,2049,3073,4097,6145,
	8193,12289,16385,24577
};
static const unsigned char dist_extra[30] = {
	0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
	7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static int
find_len_sym(unsigned int length)
{
	int lo = 0, hi = 28, mid;
	while (lo < hi) {
		mid = (lo + hi + 1) / 2;
		if (len_base[mid] <= length) lo = mid;
		else hi = mid - 1;
	}
	return lo;
}

static int
find_dist_sym(unsigned long dist)
{
	int lo = 0, hi = 29, mid;
	while (lo < hi) {
		mid = (lo + hi + 1) / 2;
		if (dist_base[mid] <= dist) lo = mid;
		else hi = mid - 1;
	}
	return lo;
}

/* ----------------------------------------------------------------
 * Deflate compressor  (LZ77 + fixed Huffman)
 *
 * Uses a hash-chain matcher with a 32 KB sliding window.
 * Produces a single fixed-Huffman block (BTYPE=01).
 * ---------------------------------------------------------------- */

#define HASH_BITS    15
#define HASH_SIZE    (1 << HASH_BITS)
#define HASH_MASK    (HASH_SIZE - 1)
#define WINDOW_SIZE  32768
#define WINDOW_MASK  (WINDOW_SIZE - 1)
#define MIN_MATCH    3
#define MAX_MATCH    258
#define MAX_CHAIN    16
#define NIL          0xFFFFFFFFUL

static unsigned int
hash3(const unsigned char *p)
{
	return ((unsigned int)p[0] * 2654435761UL ^
	        (unsigned int)p[1] * 40503 ^
	        (unsigned int)p[2]) & HASH_MASK;
}

static unsigned long
deflate_compress(const unsigned char *in, unsigned long inLen,
                 unsigned char *out, unsigned long outCap)
{
	BitWriter bw;
	unsigned long *head = 0;
	unsigned long *prev = 0;
	unsigned long pos;

	head = (unsigned long *)plugin_alloc(HASH_SIZE * sizeof(unsigned long));
	prev = (unsigned long *)plugin_alloc(WINDOW_SIZE * sizeof(unsigned long));
	if (!head || !prev) {
		if (head) plugin_free(head);
		if (prev) plugin_free(prev);
		return 0;
	}

	{
		unsigned long i;
		for (i = 0; i < HASH_SIZE; i++) head[i] = NIL;
		for (i = 0; i < WINDOW_SIZE; i++) prev[i] = NIL;
	}

	bw_init(&bw, out, outCap);

	/* block header: BFINAL=1, BTYPE=01 (fixed Huffman) */
	bw_putbits(&bw, 1, 1);
	bw_putbits(&bw, 1, 2);

	pos = 0;
	while (pos < inLen) {
		int bestLen = 0;
		unsigned long bestDist = 0;

		if (pos + MIN_MATCH <= inLen) {
			unsigned int h = hash3(in + pos);
			unsigned long mp = head[h];
			int chain = MAX_CHAIN;

			/* search hash chain for best match */
			while (mp != NIL && chain-- > 0) {
				unsigned long dist = pos - mp;
				if (dist > WINDOW_SIZE) break;
				if (dist > 0) {
					const unsigned char *a = in + mp;
					const unsigned char *b = in + pos;
					int maxLen = (int)(inLen - pos);
					int len = 0;
					if (maxLen > MAX_MATCH) maxLen = MAX_MATCH;
					while (len < maxLen && a[len] == b[len])
						len++;
					if (len > bestLen) {
						bestLen = len;
						bestDist = dist;
						if (len >= MAX_MATCH) break;
					}
				}
				mp = prev[mp & WINDOW_MASK];
			}

			/* update hash chain */
			prev[pos & WINDOW_MASK] = head[h];
			head[h] = pos;
		}

		if (bestLen >= MIN_MATCH) {
			/* emit length */
			int ls = find_len_sym((unsigned int)bestLen);
			bw_putbits(&bw, fixed_lit[257 + ls].code,
			                 fixed_lit[257 + ls].bits);
			if (len_extra[ls])
				bw_putbits(&bw, (unsigned int)bestLen - len_base[ls],
				                 len_extra[ls]);

			/* emit distance */
			{
				int ds = find_dist_sym(bestDist);
				bw_putbits(&bw, fixed_dist[ds].code,
				                 fixed_dist[ds].bits);
				if (dist_extra[ds])
					bw_putbits(&bw,
					  (unsigned int)(bestDist - dist_base[ds]),
					  dist_extra[ds]);
			}

			/* insert skipped positions into hash */
			{
				unsigned long i;
				for (i = 1; i < (unsigned long)bestLen && pos + i + MIN_MATCH <= inLen; i++) {
					unsigned int h2 = hash3(in + pos + i);
					prev[(pos + i) & WINDOW_MASK] = head[h2];
					head[h2] = pos + i;
				}
			}
			pos += (unsigned long)bestLen;
		} else {
			/* emit literal */
			bw_putbits(&bw, fixed_lit[in[pos]].code,
			                 fixed_lit[in[pos]].bits);
			pos++;
		}
	}

	/* end of block (symbol 256) */
	bw_putbits(&bw, fixed_lit[256].code, fixed_lit[256].bits);
	bw_flush(&bw);

	plugin_free(prev);
	plugin_free(head);

	return bw.pos;
}

/* ----------------------------------------------------------------
 * PNG scanline Sub filter  (filter type 1)
 *
 * For each byte: filtered[i] = raw[i] - raw[i - bpp]
 * Applied in-place after the filter-type byte.
 * ---------------------------------------------------------------- */

static void
apply_sub_filter(unsigned char *row, int stride, int bpp)
{
	int i;
	/* work backwards so we don't overwrite values we still need */
	for (i = stride - 1; i >= bpp; i--)
		row[i] = (unsigned char)(row[i] - row[i - bpp]);
}

/* ----------------------------------------------------------------
 * ColorProtocol callbacks
 *
 * setSize : allocate raw image buffer, write PNG sig + IHDR
 * sendLine: copy scanline, apply Sub filter
 * done    : compress, write IDAT + IEND, free everything
 * ---------------------------------------------------------------- */

typedef struct {
	BPTR           fh;
	int            width, height;
	int            lineBufSize;     /* 1 + width*3 */
	unsigned char *rawBuf;          /* all scanlines */
	unsigned long  rawSize;
	int            linesReceived;
	int            error;
} PNGState;

static void
png_setSize(void *priv, int w, int h, int flags)
{
	PNGState *st = (PNGState *)priv;

	(void)flags;

	if (w <= 0 || h <= 0) { st->error = 1; return; }

	st->width       = w;
	st->height      = h;
	st->lineBufSize = 1 + w * 3;
	st->rawSize     = (unsigned long)h * st->lineBufSize;

	st->rawBuf = (unsigned char *)plugin_alloc(st->rawSize);
	if (!st->rawBuf) {
		st->error = 1;
		return;
	}

	st->linesReceived = 0;

	if (!file_write(st->fh, png_sig, 8) ||
	    !write_ihdr(st->fh, w, h)) {
		st->error = 1;
	}
}

static int
png_sendLine(void *priv, int line, const ImageValue *data,
             const ImageValue *alpha)
{
	PNGState *st = (PNGState *)priv;
	unsigned char *dst;

	(void)line;
	(void)alpha;

	if (st->error) return IPSTAT_FAILED;

	dst = st->rawBuf + (unsigned long)st->linesReceived * st->lineBufSize;

	/* filter byte + RGB data */
	dst[0] = 1;                                  /* Sub filter */
	memcpy(dst + 1, data, st->width * 3);

	/* apply Sub filter in-place (skip the filter-type byte) */
	apply_sub_filter(dst + 1, st->width * 3, 3);

	st->linesReceived++;
	return IPSTAT_OK;
}

static int
png_done(void *priv, int err)
{
	PNGState *st = (PNGState *)priv;
	int result = IPSTAT_OK;

	if (err || st->error) {
		result = IPSTAT_FAILED;
		goto cleanup;
	}

	/* --- compress and write IDAT --- */
	{
		unsigned long compCap, compLen, zlibLen;
		unsigned char *compBuf;
		unsigned char *zlibBuf;
		unsigned long adlerS1 = 1, adlerS2 = 0;
		static const unsigned char idat_type[4] = { 'I','D','A','T' };

		/* worst case: deflate output ≤ input + input/8 + overhead */
		compCap = st->rawSize + st->rawSize / 8 + 512;
		compBuf = (unsigned char *)plugin_alloc(compCap);
		if (!compBuf) { result = IPSTAT_FAILED; goto cleanup; }

		compLen = deflate_compress(st->rawBuf, st->rawSize,
		                           compBuf, compCap);
		if (compLen == 0) {
			plugin_free(compBuf);
			result = IPSTAT_FAILED;
			goto cleanup;
		}

		/* compute Adler-32 over raw (uncompressed) data */
		adler32_update(&adlerS1, &adlerS2,
		               st->rawBuf, (int)st->rawSize);

		/* build zlib stream: header + deflate + adler32 */
		zlibLen = 2 + compLen + 4;
		zlibBuf = (unsigned char *)plugin_alloc(zlibLen);
		if (!zlibBuf) {
			plugin_free(compBuf);
			result = IPSTAT_FAILED;
			goto cleanup;
		}

		zlibBuf[0] = 0x78;             /* CMF */
		zlibBuf[1] = 0x01;             /* FLG */
		memcpy(zlibBuf + 2, compBuf, compLen);
		{
			unsigned long adler = (adlerS2 << 16) | adlerS1;
			put_be32(zlibBuf + 2 + compLen, adler);
		}

		plugin_free(compBuf);

		/* write IDAT chunk */
		if (!write_chunk(st->fh, idat_type, zlibBuf, zlibLen))
			result = IPSTAT_FAILED;

		plugin_free(zlibBuf);

		/* write IEND */
		if (result == IPSTAT_OK && !write_iend(st->fh))
			result = IPSTAT_FAILED;
	}

cleanup:
	if (st->fh) {
		Close(st->fh);
		st->fh = 0;
	}
	if (st->rawBuf) { plugin_free(st->rawBuf); st->rawBuf = 0; }

	return result;
}

/* ----------------------------------------------------------------
 * Activation
 * ---------------------------------------------------------------- */

XCALL_(int)
Activate(
	long          version,
	GlobalFunc   *global,
	ImSaverLocal *local,
	void         *serverData)
{
	ImageProtocol ip;
	PNGState      st;

	XCALL_INIT;
	(void)version;
	(void)global;
	(void)serverData;

	if (!crc_table_ready) make_crc_table();
	if (!fixed_tables_ready) build_fixed_tables();

	memset(&st, 0, sizeof(st));

	st.fh = Open((STRPTR)local->filename, MODE_NEWFILE);
	if (!st.fh) {
		local->result = IPSTAT_FAILED;
		return AFUNC_OK;
	}

	memset(&ip, 0, sizeof(ip));
	ip.type            = IMG_RGB24;
	ip.color.type      = IMG_RGB24;
	ip.color.priv_data = &st;
	ip.color.setSize   = png_setSize;
	ip.color.sendLine  = png_sendLine;
	ip.color.done      = png_done;

	local->result = (*local->sendData)(local->priv_data,
	                                   &ip, IMG_RGB24);

	/* safety cleanup */
	if (st.fh)     Close(st.fh);
	if (st.rawBuf) plugin_free(st.rawBuf);

	return AFUNC_OK;
}

/* ----------------------------------------------------------------
 * Server description
 * ---------------------------------------------------------------- */

ServerRecord ServerDesc[] = {
	{ "ImageSaver", "PNG(.png)", (ActivateFunc *)Activate },
	{ 0 }
};
