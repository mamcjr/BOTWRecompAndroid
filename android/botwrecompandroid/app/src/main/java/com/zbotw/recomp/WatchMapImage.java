package com.zbotw.recomp;

/** Uncompressed Zepp palette TGA: 64-byte header, BGRA palette, top-left pixel indices. */
final class WatchMapImage {
    static final int SIZE = 240;

    static byte[] encode(int[] pixels) {
        if (pixels.length != SIZE * SIZE) throw new IllegalArgumentException("Map dimensions");
        byte[] data = new byte[64 + 1024 + pixels.length];
        data[0] = 46; // Image ID length; includes Zepp SOMH extension.
        data[1] = 1;
        data[2] = 1; // Uncompressed palette; never RLE.
        data[6] = 1; // 256 palette entries, little endian.
        data[7] = 32;
        data[12] = (byte) SIZE;
        data[14] = (byte) SIZE;
        data[16] = 8;
        data[17] = 32; // Top-left origin.
        data[18] = 'S'; data[19] = 'O'; data[20] = 'M'; data[21] = 'H';
        data[22] = (byte) SIZE;
        for (int i = 0; i < 256; i++) {
            int offset = 64 + i * 4;
            data[offset] = (byte) ((i & 3) * 255 / 3);
            data[offset + 1] = (byte) (((i >> 2) & 7) * 255 / 7);
            data[offset + 2] = (byte) (((i >> 5) & 7) * 255 / 7);
            data[offset + 3] = (byte) 255;
        }
        for (int i = 0; i < pixels.length; i++) {
            int color = pixels[i];
            // Round to the nearest RGB332 palette entry instead of truncating dark colors.
            int r = (((color >> 16) & 255) * 7 + 127) / 255;
            int g = (((color >> 8) & 255) * 7 + 127) / 255;
            int b = ((color & 255) * 3 + 127) / 255;
            data[1088 + i] = (byte) ((r << 5) | (g << 2) | b);
        }
        return data;
    }
}
