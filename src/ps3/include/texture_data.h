#ifndef __TEXTURE_DATA_H__
#define __TEXTURE_DATA_H__

/*
 * Checkerboard texture data.
 *
 * PIXEL BYTE ORDER: define each pixel as R, G, B, A (source order).
 *
 * The texture is uploaded in main.cpp:init_texture() as
 * GCM_TEXTURE_FORMAT_A8R8G8B8. On PS3 (PPU is big-endian) that format
 * reads a pixel as the 32-bit value 0xAARRGGBB, i.e. bytes in memory
 * are A, R, G, B (lowest address = A, highest = B). init_texture()
 * therefore shuffles the RGBA bytes below into ARGB order while
 * copying. Do NOT emit ARGB here, and do NOT copy these bytes straight
 * through into the texture buffer -- either mistake re-introduces the
 * blue-tint bug where the alpha byte gets read as the blue channel.
 *
 * Image is 64x64, 8x8-pixel squares => 8x8 grid of squares, tiled
 * horizontally and vertically.
 */

#define RED  0xff, 0x00, 0x00, 0xff   /* one red  pixel, RGBA */
#define GRN  0x00, 0xff, 0x00, 0xff   /* one green pixel, RGBA */

#define RED4 RED, RED, RED, RED       /* 4 pixels */
#define GRN4 GRN, GRN, GRN, GRN

#define RED8 RED4, RED4               /* 8 pixels  = one square-width row-slice */
#define GRN8 GRN4, GRN4

/* one full 64-pixel row; alternates 8-pixel squares */
#define ROW_RED_START RED8, GRN8, RED8, GRN8, RED8, GRN8, RED8, GRN8
#define ROW_GRN_START GRN8, RED8, GRN8, RED8, GRN8, RED8, GRN8, RED8

/* 8 identical rows = one square-height band */
#define BAND_RED_START ROW_RED_START, ROW_RED_START, ROW_RED_START, ROW_RED_START, \
                       ROW_RED_START, ROW_RED_START, ROW_RED_START, ROW_RED_START
#define BAND_GRN_START ROW_GRN_START, ROW_GRN_START, ROW_GRN_START, ROW_GRN_START, \
                       ROW_GRN_START, ROW_GRN_START, ROW_GRN_START, ROW_GRN_START

/* one 16-row tile = a full vertical cycle of the checkerboard */
#define TILE BAND_RED_START, BAND_GRN_START

/* 64 rows = 4 tiles */
#define CHECKERBOARD TILE, TILE, TILE, TILE

static const unsigned char checkerboard_pixels[] = {
    CHECKERBOARD
};

static const struct {
    unsigned int width;
    unsigned int height;
    unsigned int bytes_per_pixel;
    const unsigned char *pixel_data;
} checkerboard = {
    64, 64, 4,
    checkerboard_pixels
};

#endif
