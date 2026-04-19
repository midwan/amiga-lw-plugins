# PNGsaver — PNG Image Saver for LightWave 3D

Image saver plugin that adds PNG output support to LightWave 3D 5.x. Select
it as the save format in render settings and rendered frames are written as
compressed RGB PNG files.

## How It Works

1. LightWave calls the saver with the output filename and image data
2. The plugin receives scanlines one at a time via the `ColorProtocol` interface
3. Each scanline is Sub-filtered (filter type 1) for better compressibility
4. After all scanlines are collected, the data is compressed using LZ77 + fixed
   Huffman coding and written as a single IDAT chunk
5. Output is a standard compressed RGB PNG readable by any PNG viewer

### PNG Encoding Details

The plugin contains a self-contained PNG encoder with deflate compression — no
zlib or external library dependencies:

- **LZ77**: Hash-chain matcher with 32 KB sliding window for finding repeated
  byte sequences
- **Fixed Huffman**: RFC 1951 fixed code tables for encoding literals, lengths,
  and distances
- **Sub filter**: Each scanline byte stores the difference from the
  corresponding byte in the previous pixel, making smooth gradients highly
  compressible
- **CRC-32**: Standard PNG chunk checksums via a 256-entry lookup table
- **Adler-32**: Zlib stream checksum
- **Output format**: Always RGB (color type 2, 24-bit) — LightWave's alpha
  channel represents render coverage and is not embedded in the output

Typical compression ratios for rendered 3D images are 70-85% smaller than raw
pixel data, comparable to standard PNG encoders.

## Installation

1. Copy `pngsaver.p` to your LightWave plugins directory
2. Run Layout and, without loading any scene or object, under Options tab click on 'Add Plug-Ins'
3. Navigate to the directory you copied the plugin and select it.
4. Restart Layout so that the configuration file get updated with the new plug-in entry.

## Usage

1. Open **Record** (or the render output settings)
2. Set **Save RGB** to enabled
3. Select **PNG(.png)** as the image format
4. Set the output path and filename
5. Render — each frame is saved as a `.png` file

## Technical Notes

- **Memory**: Accumulates all scanlines in memory, then compresses in one pass.
  Total memory is approximately twice the raw image size (raw buffer +
  compressed output buffer). For a 1920x1080 RGB image this is around 12 MB.
- **Compression**: Uses LZ77 with hash chains (chain depth 16) and fixed
  Huffman coding. A single deflate block is emitted per image.
- **File I/O**: Uses AmigaOS DOS library calls (Open/Write/Close).
- **No UI panel**: The saver has no configurable settings.
