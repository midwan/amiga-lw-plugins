/*
 * PNGLOADER.C -- PNG Image Loader for LightWave 3D
 *
 * Loads PNG files as textures, backgrounds and foreground images.
 * Implements a complete PNG decoder with full deflate decompression,
 * scanline de-filtering, and color type conversion — no external
 * library dependencies.
 *
 * Supported: color types 0/2/3/4/6, bit depths 1-16, all filter
 * types (None/Sub/Up/Average/Paeth), interlace: none.
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
	"\0$VER: PNG_loader " PLUGIN_VERSION " (c) D. Panokostas";

/* ----------------------------------------------------------------
 * Memory helpers  (same pattern as other plugins)
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
 * CRC32  (for PNG chunk validation)
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
			if (c & 1)
				c = 0xEDB88320UL ^ (c >> 1);
			else
				c = c >> 1;
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
 * Big-endian read helpers
 * ---------------------------------------------------------------- */

static unsigned long
get_be32(const unsigned char *p)
{
	return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
	       ((unsigned long)p[2] << 8)  | (unsigned long)p[3];
}

/* ----------------------------------------------------------------
 * AmigaOS file I/O helpers
 * ---------------------------------------------------------------- */

static int
file_read(BPTR fh, void *buf, int len)
{
	return (Read(fh, buf, len) == len);
}

static int
file_read_be32(BPTR fh, unsigned long *val)
{
	unsigned char buf[4];
	if (!file_read(fh, buf, 4)) return 0;
	*val = get_be32(buf);
	return 1;
}

/* ----------------------------------------------------------------
 * Deflate decompressor (inflate)
 *
 * Full RFC 1951 implementation: stored, fixed Huffman, and dynamic
 * Huffman blocks with LZ77 back-references.
 * ---------------------------------------------------------------- */

#define INF_OK        0
#define INF_ERR_DATA  1
#define INF_ERR_MEM   2

/* ---- Bit reader ---- */

typedef struct {
	const unsigned char *src;
	unsigned long        srcLen;
	unsigned long        pos;        /* byte position */
	unsigned long        bitBuf;
	int                  bitCnt;
} BitReader;

static void
br_init(BitReader *br, const unsigned char *data, unsigned long len)
{
	br->src    = data;
	br->srcLen = len;
	br->pos    = 0;
	br->bitBuf = 0;
	br->bitCnt = 0;
}

static unsigned int
br_bits(BitReader *br, int n)
{
	unsigned int val;
	while (br->bitCnt < n) {
		if (br->pos < br->srcLen)
			br->bitBuf |= (unsigned long)br->src[br->pos++]
			              << br->bitCnt;
		br->bitCnt += 8;
	}
	val = (unsigned int)(br->bitBuf & ((1UL << n) - 1));
	br->bitBuf >>= n;
	br->bitCnt -= n;
	return val;
}

static void
br_align(BitReader *br)
{
	br->bitBuf = 0;
	br->bitCnt = 0;
}

/* ---- Huffman decoder ---- */

#define MAXBITS  15
#define MAXCODES 320    /* 288 lit/len + 32 dist */

typedef struct {
	unsigned short count[MAXBITS + 1];
	unsigned short symbol[MAXCODES];
} Huffman;

static int
huff_build(Huffman *h, const unsigned short *lengths, int n)
{
	int i;
	unsigned short offs[MAXBITS + 1];

	memset(h->count, 0, sizeof(h->count));
	for (i = 0; i < n; i++)
		h->count[lengths[i]]++;

	/* check for no codes */
	{
		int total = 0;
		for (i = 1; i <= MAXBITS; i++) total += h->count[i];
		if (total == 0) return 0;
	}

	offs[1] = 0;
	for (i = 1; i < MAXBITS; i++)
		offs[i + 1] = offs[i] + h->count[i];

	for (i = 0; i < n; i++)
		if (lengths[i])
			h->symbol[offs[lengths[i]]++] = (unsigned short)i;

	return 1;
}

static int
huff_decode(BitReader *br, const Huffman *h)
{
	int code = 0, first = 0, index = 0, len;

	for (len = 1; len <= MAXBITS; len++) {
		code |= (int)br_bits(br, 1);
		first += h->count[len];
		if (code < first)
			return h->symbol[index + (code - (first - h->count[len]))];
		index += h->count[len];
		code <<= 1;
		first <<= 1;
	}
	return -1;  /* invalid code */
}

/* ---- Length/distance tables (RFC 1951) ---- */

static const unsigned short len_base[29] = {
	3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
	35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const unsigned short len_extra[29] = {
	0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
	3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const unsigned short dist_base[30] = {
	1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
	257,385,513,769,1025,1537,2049,3073,4097,6145,
	8193,12289,16385,24577
};
static const unsigned short dist_extra[30] = {
	0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
	7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* Code length alphabet order for dynamic Huffman */
static const unsigned char cl_order[19] = {
	16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

/* ---- Inflate core ---- */

static int
inflate_codes(BitReader *br, const Huffman *litlen, const Huffman *dist,
              unsigned char *out, unsigned long outLen, unsigned long *outPos)
{
	int sym;

	for (;;) {
		sym = huff_decode(br, litlen);
		if (sym < 0) return INF_ERR_DATA;

		if (sym < 256) {
			/* literal byte */
			if (*outPos >= outLen) return INF_ERR_DATA;
			out[(*outPos)++] = (unsigned char)sym;
		} else if (sym == 256) {
			/* end of block */
			return INF_OK;
		} else {
			/* length/distance pair */
			unsigned int length, distance;
			int dsym;
			unsigned long i, srcOff;

			sym -= 257;
			if (sym >= 29) return INF_ERR_DATA;
			length = len_base[sym] + br_bits(br, len_extra[sym]);

			dsym = huff_decode(br, dist);
			if (dsym < 0 || dsym >= 30) return INF_ERR_DATA;
			distance = dist_base[dsym] + br_bits(br, dist_extra[dsym]);

			if (distance > *outPos) return INF_ERR_DATA;
			srcOff = *outPos - distance;

			for (i = 0; i < length; i++) {
				if (*outPos >= outLen) return INF_ERR_DATA;
				out[*outPos] = out[srcOff + (i % distance)];
				(*outPos)++;
			}
		}
	}
}

static int
inflate_stored(BitReader *br, unsigned char *out,
               unsigned long outLen, unsigned long *outPos)
{
	unsigned int len, nlen;

	br_align(br);

	if (br->pos + 4 > br->srcLen) return INF_ERR_DATA;
	len  = br->src[br->pos] | ((unsigned int)br->src[br->pos + 1] << 8);
	nlen = br->src[br->pos + 2] | ((unsigned int)br->src[br->pos + 3] << 8);
	br->pos += 4;

	if ((len ^ nlen) != 0xFFFF) return INF_ERR_DATA;
	if (br->pos + len > br->srcLen) return INF_ERR_DATA;
	if (*outPos + len > outLen) return INF_ERR_DATA;

	memcpy(out + *outPos, br->src + br->pos, len);
	br->pos += len;
	*outPos += len;

	return INF_OK;
}

static int
inflate_fixed(BitReader *br, unsigned char *out,
              unsigned long outLen, unsigned long *outPos)
{
	Huffman litlen, dist;
	unsigned short lengths[320];
	int i;

	for (i = 0;   i < 144; i++) lengths[i] = 8;
	for (i = 144; i < 256; i++) lengths[i] = 9;
	for (i = 256; i < 280; i++) lengths[i] = 7;
	for (i = 280; i < 288; i++) lengths[i] = 8;
	huff_build(&litlen, lengths, 288);

	for (i = 0; i < 32; i++) lengths[i] = 5;
	huff_build(&dist, lengths, 32);

	return inflate_codes(br, &litlen, &dist, out, outLen, outPos);
}

static int
inflate_dynamic(BitReader *br, unsigned char *out,
                unsigned long outLen, unsigned long *outPos)
{
	Huffman litlen, dist, codelen;
	unsigned short lengths[320];
	int hlit, hdist, hclen;
	int i, n, total;

	hlit  = (int)br_bits(br, 5) + 257;
	hdist = (int)br_bits(br, 5) + 1;
	hclen = (int)br_bits(br, 4) + 4;

	if (hlit > 286 || hdist > 30) return INF_ERR_DATA;

	/* read code length alphabet */
	memset(lengths, 0, 19 * sizeof(unsigned short));
	for (i = 0; i < hclen; i++)
		lengths[cl_order[i]] = (unsigned short)br_bits(br, 3);
	huff_build(&codelen, lengths, 19);

	/* decode literal/length + distance code lengths */
	total = hlit + hdist;
	memset(lengths, 0, sizeof(lengths));
	i = 0;
	while (i < total) {
		int sym = huff_decode(br, &codelen);
		if (sym < 0) return INF_ERR_DATA;

		if (sym < 16) {
			lengths[i++] = (unsigned short)sym;
		} else if (sym == 16) {
			n = (int)br_bits(br, 2) + 3;
			if (i == 0) return INF_ERR_DATA;
			while (n-- > 0 && i < total)
				lengths[i] = lengths[i - 1], i++;
		} else if (sym == 17) {
			n = (int)br_bits(br, 3) + 3;
			while (n-- > 0 && i < total)
				lengths[i++] = 0;
		} else if (sym == 18) {
			n = (int)br_bits(br, 7) + 11;
			while (n-- > 0 && i < total)
				lengths[i++] = 0;
		} else {
			return INF_ERR_DATA;
		}
	}

	huff_build(&litlen, lengths, hlit);
	huff_build(&dist, lengths + hlit, hdist);

	return inflate_codes(br, &litlen, &dist, out, outLen, outPos);
}

/*
 * inflate -- decompress a zlib stream (RFC 1950 wrapper + RFC 1951 deflate).
 *
 * src/srcLen: compressed data (including 2-byte zlib header + 4-byte trailer)
 * out/outLen: pre-allocated output buffer
 * Returns decompressed size, or 0 on error.
 */
static unsigned long
inflate(const unsigned char *src, unsigned long srcLen,
        unsigned char *out, unsigned long outLen)
{
	BitReader br;
	unsigned long outPos = 0;
	int bfinal;

	/* validate zlib header */
	if (srcLen < 6) return 0;
	if ((src[0] & 0x0F) != 8) return 0;         /* CM must be 8 */
	if ((src[0] * 256 + src[1]) % 31 != 0) return 0;
	if (src[1] & 0x20) return 0;                 /* FDICT not supported */

	br_init(&br, src + 2, srcLen - 6);  /* skip zlib header, exclude trailer */

	do {
		int btype;
		int err;

		bfinal = (int)br_bits(&br, 1);
		btype  = (int)br_bits(&br, 2);

		switch (btype) {
		case 0: err = inflate_stored(&br, out, outLen, &outPos); break;
		case 1: err = inflate_fixed(&br, out, outLen, &outPos); break;
		case 2: err = inflate_dynamic(&br, out, outLen, &outPos); break;
		default: return 0;
		}
		if (err != INF_OK) return 0;
	} while (!bfinal);

	/* verify Adler-32 */
	{
		unsigned long a32_stored, a32_calc;
		unsigned long s1 = 1, s2 = 0;
		unsigned long i;
		const unsigned char *trailer = src + srcLen - 4;

		a32_stored = get_be32(trailer);

		for (i = 0; i < outPos; i++) {
			s1 = (s1 + out[i]) % 65521;
			s2 = (s2 + s1) % 65521;
		}
		a32_calc = (s2 << 16) | s1;

		if (a32_stored != a32_calc) return 0;
	}

	return outPos;
}

/* ----------------------------------------------------------------
 * PNG chunk reader
 * ---------------------------------------------------------------- */

typedef struct {
	unsigned long  length;
	unsigned char  type[4];
	unsigned long  crc;
} ChunkHeader;

static int
read_chunk_header(BPTR fh, ChunkHeader *ch)
{
	if (!file_read_be32(fh, &ch->length)) return 0;
	if (!file_read(fh, ch->type, 4)) return 0;
	return 1;
}

/* Read chunk data + CRC, validate CRC */
static unsigned char *
read_chunk_data(BPTR fh, ChunkHeader *ch)
{
	unsigned char *data;
	unsigned char crcbuf[4];
	unsigned long crc;

	if (ch->length == 0) {
		if (!file_read(fh, crcbuf, 4)) return 0;
		return (unsigned char *)1;  /* non-null sentinel for empty data */
	}

	data = (unsigned char *)plugin_alloc(ch->length);
	if (!data) return 0;

	if (!file_read(fh, data, (int)ch->length)) {
		plugin_free(data);
		return 0;
	}

	if (!file_read(fh, crcbuf, 4)) {
		plugin_free(data);
		return 0;
	}

	/* validate CRC */
	crc = 0xFFFFFFFFUL;
	crc = update_crc(crc, ch->type, 4);
	crc = update_crc(crc, data, (int)ch->length);
	crc ^= 0xFFFFFFFFUL;

	if (crc != get_be32(crcbuf)) {
		plugin_free(data);
		return 0;
	}

	return data;
}

static void
skip_chunk_data(BPTR fh, ChunkHeader *ch)
{
	Seek(fh, (long)ch->length + 4, OFFSET_CURRENT);  /* data + CRC */
}

/* ----------------------------------------------------------------
 * PNG IHDR parsing
 * ---------------------------------------------------------------- */

typedef struct {
	int width, height;
	int bitDepth;
	int colorType;
	int interlace;
	int srcChannels;      /* channels in the file */
	int srcBpp;           /* bytes per pixel (at 8+ bit depth) */
	int srcStride;        /* raw bytes per row (before filter byte) */
} PNGInfo;

static int
parse_ihdr(const unsigned char *data, unsigned long len, PNGInfo *info)
{
	if (len < 13) return 0;

	info->width     = (int)get_be32(data);
	info->height    = (int)get_be32(data + 4);
	info->bitDepth  = data[8];
	info->colorType = data[9];
	info->interlace = data[12];

	if (info->width <= 0 || info->height <= 0) return 0;
	if (data[10] != 0 || data[11] != 0) return 0;  /* compression/filter */
	if (info->interlace != 0) return 0;              /* no Adam7 support */

	switch (info->colorType) {
	case 0: info->srcChannels = 1; break;  /* grayscale */
	case 2: info->srcChannels = 3; break;  /* RGB */
	case 3: info->srcChannels = 1; break;  /* indexed */
	case 4: info->srcChannels = 2; break;  /* grayscale+alpha */
	case 6: info->srcChannels = 4; break;  /* RGBA */
	default: return 0;
	}

	/* compute bytes per pixel and stride */
	{
		int bitsPerPixel = info->bitDepth * info->srcChannels;
		info->srcBpp = (bitsPerPixel + 7) / 8;
		info->srcStride = (info->width * bitsPerPixel + 7) / 8;
	}

	return 1;
}

/* ----------------------------------------------------------------
 * PNG scanline de-filtering  (RFC 2083 section 6)
 * ---------------------------------------------------------------- */

static int
paeth(int a, int b, int c)
{
	int p  = a + b - c;
	int pa = p - a; if (pa < 0) pa = -pa;
	int pb = p - b; if (pb < 0) pb = -pb;
	int pc = p - c; if (pc < 0) pc = -pc;
	if (pa <= pb && pa <= pc) return a;
	if (pb <= pc) return b;
	return c;
}

static int
defilter(unsigned char *raw, int height, int stride, int bpp)
{
	int y;
	unsigned char *cur, *prev;

	for (y = 0; y < height; y++) {
		int filterType;
		int i;

		cur  = raw + (unsigned long)y * (1 + stride);
		prev = (y > 0) ? raw + (unsigned long)(y - 1) * (1 + stride) + 1
		               : 0;
		filterType = cur[0];
		cur++;  /* skip filter byte */

		switch (filterType) {
		case 0: /* None */
			break;

		case 1: /* Sub */
			for (i = bpp; i < stride; i++)
				cur[i] = (unsigned char)(cur[i] + cur[i - bpp]);
			break;

		case 2: /* Up */
			if (prev) {
				for (i = 0; i < stride; i++)
					cur[i] = (unsigned char)(cur[i] + prev[i]);
			}
			break;

		case 3: /* Average */
			for (i = 0; i < stride; i++) {
				int a = (i >= bpp) ? cur[i - bpp] : 0;
				int b = prev ? prev[i] : 0;
				cur[i] = (unsigned char)(cur[i] + (a + b) / 2);
			}
			break;

		case 4: /* Paeth */
			for (i = 0; i < stride; i++) {
				int a = (i >= bpp) ? cur[i - bpp] : 0;
				int b = prev ? prev[i] : 0;
				int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
				cur[i] = (unsigned char)(cur[i] + paeth(a, b, c));
			}
			break;

		default:
			return 0;  /* unknown filter */
		}
	}
	return 1;
}

/* ----------------------------------------------------------------
 * Pixel conversion to RGB24
 * ---------------------------------------------------------------- */

static void
convert_row_to_rgb(const unsigned char *src, unsigned char *dst,
                   int width, const PNGInfo *info,
                   const unsigned char *palette)
{
	int x;

	switch (info->colorType) {
	case 0:  /* Grayscale */
		if (info->bitDepth == 16) {
			for (x = 0; x < width; x++) {
				unsigned char g = src[x * 2];
				dst[x * 3] = dst[x * 3 + 1] = dst[x * 3 + 2] = g;
			}
		} else if (info->bitDepth == 8) {
			for (x = 0; x < width; x++)
				dst[x * 3] = dst[x * 3 + 1] = dst[x * 3 + 2] = src[x];
		} else {
			/* sub-byte: 1/2/4 bit */
			int shift = 8 - info->bitDepth;
			int mask  = (1 << info->bitDepth) - 1;
			int scale = 255 / mask;
			for (x = 0; x < width; x++) {
				int byteIdx = (x * info->bitDepth) / 8;
				int bitIdx  = shift - ((x * info->bitDepth) % 8);
				unsigned char g = (unsigned char)(((src[byteIdx] >> bitIdx) & mask) * scale);
				dst[x * 3] = dst[x * 3 + 1] = dst[x * 3 + 2] = g;
			}
		}
		break;

	case 2:  /* RGB */
		if (info->bitDepth == 16) {
			for (x = 0; x < width; x++) {
				dst[x * 3 + 0] = src[x * 6 + 0];
				dst[x * 3 + 1] = src[x * 6 + 2];
				dst[x * 3 + 2] = src[x * 6 + 4];
			}
		} else {
			memcpy(dst, src, width * 3);
		}
		break;

	case 3:  /* Indexed */
		if (palette) {
			if (info->bitDepth == 8) {
				for (x = 0; x < width; x++) {
					int idx = src[x];
					dst[x * 3 + 0] = palette[idx * 3 + 0];
					dst[x * 3 + 1] = palette[idx * 3 + 1];
					dst[x * 3 + 2] = palette[idx * 3 + 2];
				}
			} else {
				int shift = 8 - info->bitDepth;
				int mask  = (1 << info->bitDepth) - 1;
				for (x = 0; x < width; x++) {
					int byteIdx = (x * info->bitDepth) / 8;
					int bitIdx  = shift - ((x * info->bitDepth) % 8);
					int idx = (src[byteIdx] >> bitIdx) & mask;
					dst[x * 3 + 0] = palette[idx * 3 + 0];
					dst[x * 3 + 1] = palette[idx * 3 + 1];
					dst[x * 3 + 2] = palette[idx * 3 + 2];
				}
			}
		} else {
			memset(dst, 0, width * 3);
		}
		break;

	case 4:  /* Grayscale + Alpha (discard alpha) */
		if (info->bitDepth == 16) {
			for (x = 0; x < width; x++) {
				unsigned char g = src[x * 4];
				dst[x * 3] = dst[x * 3 + 1] = dst[x * 3 + 2] = g;
			}
		} else {
			for (x = 0; x < width; x++) {
				unsigned char g = src[x * 2];
				dst[x * 3] = dst[x * 3 + 1] = dst[x * 3 + 2] = g;
			}
		}
		break;

	case 6:  /* RGBA (discard alpha) */
		if (info->bitDepth == 16) {
			for (x = 0; x < width; x++) {
				dst[x * 3 + 0] = src[x * 8 + 0];
				dst[x * 3 + 1] = src[x * 8 + 2];
				dst[x * 3 + 2] = src[x * 8 + 4];
			}
		} else {
			for (x = 0; x < width; x++) {
				dst[x * 3 + 0] = src[x * 4 + 0];
				dst[x * 3 + 1] = src[x * 4 + 1];
				dst[x * 3 + 2] = src[x * 4 + 2];
			}
		}
		break;
	}
}

/* ----------------------------------------------------------------
 * Activation  (ImLoaderLocal handler)
 * ---------------------------------------------------------------- */

XCALL_(int)
Activate(
	long            version,
	GlobalFunc     *global,
	ImLoaderLocal  *local,
	void           *serverData)
{
	BPTR            fh = 0;
	PNGInfo         info;
	unsigned char  *idatBuf   = 0;
	unsigned long   idatSize  = 0;
	unsigned long   idatAlloc = 0;
	unsigned char  *rawBuf    = 0;
	unsigned long   rawSize;
	unsigned long   decSize;
	unsigned char  *rgbRow    = 0;
	unsigned char  *palette   = 0;
	ImageProtocolID proto;
	int             y;

	unsigned char sig[8];
	static const unsigned char png_sig[8] = {
		137,80,78,71,13,10,26,10
	};

	XCALL_INIT;
	(void)version;
	(void)global;
	(void)serverData;

	if (!crc_table_ready)
		make_crc_table();

	/* --- open file and validate PNG signature --- */

	fh = Open((STRPTR)local->filename, MODE_OLDFILE);
	if (!fh) {
		local->result = IPSTAT_BADFILE;
		return AFUNC_OK;
	}

	if (!file_read(fh, sig, 8) || memcmp(sig, png_sig, 8) != 0)
		goto bad_file;

	/* --- parse chunks: read IHDR, collect IDAT, read PLTE --- */

	memset(&info, 0, sizeof(info));

	for (;;) {
		ChunkHeader ch;
		unsigned char *data;

		if (!read_chunk_header(fh, &ch))
			goto bad_file;

		/* IEND — done reading */
		if (memcmp(ch.type, "IEND", 4) == 0)
			break;

		/* IHDR */
		if (memcmp(ch.type, "IHDR", 4) == 0) {
			data = read_chunk_data(fh, &ch);
			if (!data) goto bad_file;
			if (!parse_ihdr(data, ch.length, &info)) {
				plugin_free(data);
				goto bad_file;
			}
			plugin_free(data);
			continue;
		}

		/* PLTE */
		if (memcmp(ch.type, "PLTE", 4) == 0) {
			data = read_chunk_data(fh, &ch);
			if (!data) goto bad_file;
			palette = data;  /* keep — freed at end */
			continue;
		}

		/* IDAT — accumulate compressed data */
		if (memcmp(ch.type, "IDAT", 4) == 0) {
			data = read_chunk_data(fh, &ch);
			if (!data) goto bad_file;

			/* grow IDAT buffer if needed */
			if (idatSize + ch.length > idatAlloc) {
				unsigned char *newBuf;
				unsigned long newAlloc = (idatSize + ch.length) * 2;
				if (newAlloc < 4096) newAlloc = 4096;
				newBuf = (unsigned char *)plugin_alloc(newAlloc);
				if (!newBuf) {
					plugin_free(data);
					goto fail;
				}
				if (idatBuf) {
					memcpy(newBuf, idatBuf, idatSize);
					plugin_free(idatBuf);
				}
				idatBuf = newBuf;
				idatAlloc = newAlloc;
			}

			memcpy(idatBuf + idatSize, data, ch.length);
			idatSize += ch.length;
			plugin_free(data);
			continue;
		}

		/* unknown chunk — skip */
		skip_chunk_data(fh, &ch);
	}

	Close(fh);
	fh = 0;

	/* validate we got what we need */
	if (info.width == 0 || idatSize == 0)
		goto fail;

	/* --- decompress IDAT data --- */

	rawSize = (unsigned long)info.height * (1 + info.srcStride);
	rawBuf = (unsigned char *)plugin_alloc(rawSize);
	if (!rawBuf) goto fail;

	decSize = inflate(idatBuf, idatSize, rawBuf, rawSize);
	plugin_free(idatBuf);
	idatBuf = 0;

	if (decSize != rawSize)
		goto fail;

	/* --- de-filter scanlines --- */

	if (!defilter(rawBuf, info.height, info.srcStride, info.srcBpp))
		goto fail;

	/* --- send image to LightWave --- */

	rgbRow = (unsigned char *)plugin_alloc((unsigned long)info.width * 3);
	if (!rgbRow) goto fail;

	proto = (*local->begin)(local->priv_data, IMG_RGB24);
	if (!proto) goto fail;

	IP_SETSIZE(&proto->color, info.width, info.height, 0);

	for (y = 0; y < info.height; y++) {
		const unsigned char *src =
			rawBuf + (unsigned long)y * (1 + info.srcStride) + 1;

		convert_row_to_rgb(src, rgbRow, info.width, &info, palette);

		if (IP_SENDLINE(&proto->color, y, rgbRow, 0) != IPSTAT_OK)
			break;
	}

	IP_DONE(&proto->color, 0);
	(*local->done)(local->priv_data, proto);
	local->result = IPSTAT_OK;

	plugin_free(rgbRow);
	plugin_free(rawBuf);
	if (palette) plugin_free(palette);
	return AFUNC_OK;

bad_file:
	local->result = IPSTAT_BADFILE;
	goto cleanup;

fail:
	local->result = IPSTAT_FAILED;

cleanup:
	if (fh)       Close(fh);
	if (idatBuf)  plugin_free(idatBuf);
	if (rawBuf)   plugin_free(rawBuf);
	if (rgbRow)   plugin_free(rgbRow);
	if (palette)  plugin_free(palette);
	return AFUNC_OK;
}

/* ----------------------------------------------------------------
 * Server description
 * ---------------------------------------------------------------- */

ServerRecord ServerDesc[] = {
	{ "ImageLoader", "PNG(.png)", (ActivateFunc *)Activate },
	{ 0 }
};
