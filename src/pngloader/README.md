# PNGloader — PNG Image Loader for LightWave 3D

Image loader plugin that adds PNG input support to LightWave 3D 5.x. Load
PNG files as textures, background images, and foreground images from any
source.

## How It Works

1. LightWave calls the loader with a PNG filename
2. The plugin reads and validates the PNG file structure (signature, chunks)
3. IDAT chunk data is collected and decompressed using a built-in deflate
   decompressor
4. Scanlines are de-filtered (None/Sub/Up/Average/Paeth)
5. Pixels are converted to RGB24 and sent to LightWave line by line

### Supported Formats

| Color Type | Description | Bit Depths |
|---|---|---|
| 0 | Grayscale | 1, 2, 4, 8, 16 |
| 2 | RGB | 8, 16 |
| 3 | Indexed (palette) | 1, 2, 4, 8 |
| 4 | Grayscale + Alpha | 8, 16 |
| 6 | RGBA | 8, 16 |

All color types are converted to RGB24 for LightWave. Alpha channels are
discarded. 16-bit channels are reduced to 8-bit. Sub-byte grayscale and
indexed images are expanded to full 8-bit.

### Deflate Decompressor

The plugin includes a complete RFC 1951 deflate decompressor:

- **Stored blocks** (uncompressed) — e.g. PNGs from our own saver
- **Fixed Huffman** — common in simple encoders
- **Dynamic Huffman** — used by most PNG encoders (Photoshop, GIMP, etc.)
- **LZ77** back-references with full 32KB sliding window
- Validates zlib header and Adler-32 checksum

No external zlib dependency required.

### Limitations

- Adam7 interlaced PNGs are not supported (non-interlaced only)
- Ancillary chunks (gAMA, sRGB, tEXt, etc.) are skipped

## Installation

1. Copy `pngloader.p` to your LightWave plugins directory
2. Add this line to your LW config file:

```
Plugin ImageLoader PNG(.png) pngloader.p PNG(.png)
```

3. Restart LightWave

## Usage

Once installed, PNG files will appear in LightWave's image file requesters.
You can use them anywhere LightWave accepts images:

- **Textures**: Load PNG files as surface textures in the Surfaces panel
- **Background**: Set a PNG as the background image in Effects
- **Foreground**: Use a PNG as a foreground overlay

## Technical Notes

- **Memory**: Allocates buffers for compressed IDAT data, decompressed
  scanlines, and one RGB row. Total memory is approximately the compressed
  file size plus the raw image size.
- **File I/O**: Uses AmigaOS DOS library calls (Open/Read/Seek/Close).
- **CRC validation**: All chunk CRCs are verified during loading.
- **No UI panel**: The loader has no configurable settings.
