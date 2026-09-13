#pragma once
// E-ink display controller ioctl ABI for Kobo e-readers.
//
// Two generations matter here:
//
//  * NTX/Freescale boards (i.MX5/6/7) expose the i.MX EPDC through
//    <linux/mxcfb.h> style ioctls (MXCFB_SEND_UPDATE and friends).
//  * The 2024 MediaTek boards (MT8113: Clara BW, Clara Colour, Libra
//    Colour, Elipsa 2E) expose MediaTek's "hwtcon" driver instead, with its
//    own ioctl numbers, waveform list, and - new for Kaleido panels -
//    colour-filter-array processing modes.
//
// These declarations describe the kernel ABI (structure layout and ioctl
// numbers) as published in Kobo's own kernel sources at
// github.com/kobolabs/Kobo-Reader; they are reproduced here so CrossKobo
// needs no vendor headers to build. Values cross-checked against NiLuJe's
// FBInk, which documents the practical waveform/flag pairings.

#include <linux/fb.h>
#include <stdint.h>
#include <sys/ioctl.h>

// ---------------------------------------------------------------- generic
struct ck_epd_rect {
  uint32_t top;
  uint32_t left;
  uint32_t width;
  uint32_t height;
};

#define CK_UPDATE_MODE_PARTIAL 0x0
#define CK_UPDATE_MODE_FULL    0x1

#define CK_TEMP_USE_AMBIENT 0x1000

// ------------------------------------------------------- MediaTek (hwtcon)
#define HWTCON_FLAG_USE_DITHERING         0x1
#define HWTCON_FLAG_FORCE_A2_OUTPUT       0x10
#define HWTCON_FLAG_FORCE_A2_OUTPUT_WHITE 0x20
#define HWTCON_FLAG_FORCE_A2_OUTPUT_BLACK 0x40

// Colour filter array processing (Kaleido panels only).
#define HWTCON_FLAG_CFA_EINK_AIE_S4 0x00000200
#define HWTCON_FLAG_CFA_EINK_AIE_S7 0x00000300
#define HWTCON_FLAG_CFA_EINK_AIE_S9 0x00000400
#define HWTCON_FLAG_CFA_EINK_G0     0x00000500  // desaturate
#define HWTCON_FLAG_CFA_EINK_G1     0x00000100  // standard
#define HWTCON_FLAG_CFA_EINK_G2     0x00000600  // gentle saturation boost
#define HWTCON_FLAG_CFA_EINK_NTX    0x00000a00
#define HWTCON_FLAG_CFA_EINK_NTX_SF 0x00000b00
#define HWTCON_FLAG_CFA_SKIP        0x00008000  // render as greyscale

enum {
  HWTCON_WAVEFORM_MODE_INIT = 0,
  HWTCON_WAVEFORM_MODE_DU = 1,
  HWTCON_WAVEFORM_MODE_GC16 = 2,
  HWTCON_WAVEFORM_MODE_GL16 = 3,
  HWTCON_WAVEFORM_MODE_GLR16 = 4,
  HWTCON_WAVEFORM_MODE_A2 = 6,
  HWTCON_WAVEFORM_MODE_GCK16 = 8,   // eclipse (night mode)
  HWTCON_WAVEFORM_MODE_GLKW16 = 9,  // eclipse REAGL
  HWTCON_WAVEFORM_MODE_GCC16 = 10,  // colour images
  HWTCON_WAVEFORM_MODE_GLRC16 = 11, // colour highlights over text
  HWTCON_WAVEFORM_MODE_AUTO = 257,
};

// Dither targets: Y8 source to Y4 (16 grey) or Y1 (bilevel) output.
#define HWTCON_DITHER_Y8_Y4_S 0x102
#define HWTCON_DITHER_Y8_Y1_S 0x302
#define HWTCON_DITHER_Y8_Y4_B 0x101

struct hwtcon_update_data {
  struct ck_epd_rect update_region;
  uint32_t waveform_mode;
  uint32_t update_mode;
  uint32_t update_marker;
  unsigned int flags;
  int dither_mode;
};

struct hwtcon_update_marker_data {
  uint32_t update_marker;
  uint32_t collision_test;
};

#define HWTCON_IOCTL_MAGIC 'F'
#define HWTCON_SET_NIGHTMODE             _IOW(HWTCON_IOCTL_MAGIC, 0x26, int32_t)
#define HWTCON_SET_TEMPERATURE           _IOW(HWTCON_IOCTL_MAGIC, 0x2C, int32_t)
#define HWTCON_SEND_UPDATE               _IOW(HWTCON_IOCTL_MAGIC, 0x2E, struct hwtcon_update_data)
#define HWTCON_WAIT_FOR_UPDATE_COMPLETE  _IOWR(HWTCON_IOCTL_MAGIC, 0x2F, struct hwtcon_update_marker_data)
#define HWTCON_SET_PWRDOWN_DELAY         _IOW(HWTCON_IOCTL_MAGIC, 0x30, int32_t)
#define HWTCON_SET_PAUSE                 _IOW(HWTCON_IOCTL_MAGIC, 0x33, uint32_t)
#define HWTCON_SET_RESUME                _IOW(HWTCON_IOCTL_MAGIC, 0x35, uint32_t)
#define HWTCON_SET_CFA_MODE              _IOW(HWTCON_IOCTL_MAGIC, 0x50, uint32_t)

// Kernel-side CFA mode constants for HWTCON_SET_CFA_MODE.
#define HWTCON_CFA_MODE_NONE    0
#define HWTCON_CFA_MODE_EINK_G1 1
#define HWTCON_CFA_MODE_EINK_G2 6

// --------------------------------------------------------- NTX i.MX (mxcfb)
#define MXCFB_WAVEFORM_MODE_INIT 0
#define MXCFB_WAVEFORM_MODE_DU   1
#define MXCFB_WAVEFORM_MODE_GC16 2
#define MXCFB_WAVEFORM_MODE_GC4  3
#define MXCFB_WAVEFORM_MODE_A2   4
#define MXCFB_WAVEFORM_MODE_GL16 5
#define MXCFB_WAVEFORM_MODE_REAGL 6
#define MXCFB_WAVEFORM_MODE_REAGLD 7
#define MXCFB_WAVEFORM_MODE_AUTO 257

#define EPDC_FLAG_ENABLE_INVERSION 0x01
#define EPDC_FLAG_FORCE_MONOCHROME 0x02
#define EPDC_FLAG_USE_DITHERING_Y1 0x2000
#define EPDC_FLAG_USE_DITHERING_Y4 0x4000

struct mxcfb_alt_buffer_data_ntx {
  uint32_t phys_addr;
  uint32_t width;
  uint32_t height;
  struct ck_epd_rect alt_update_region;
};

struct mxcfb_update_data_ntx {
  struct ck_epd_rect update_region;
  uint32_t waveform_mode;
  uint32_t update_mode;
  uint32_t update_marker;
  int temp;
  unsigned int flags;
  int dither_mode;
  int quant_bit;
  struct mxcfb_alt_buffer_data_ntx alt_buffer_data;
};

#define MXCFB_SEND_UPDATE              _IOW('F', 0x2E, struct mxcfb_update_data_ntx)
#define MXCFB_WAIT_FOR_UPDATE_COMPLETE _IOWR('F', 0x2F, uint32_t)
#define MXCFB_SET_PWRDOWN_DELAY        _IOW('F', 0x30, int32_t)
