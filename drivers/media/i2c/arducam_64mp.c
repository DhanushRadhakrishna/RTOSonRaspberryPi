// SPDX-License-Identifier: GPL-2.0
/*
 * A V4L2 driver for Arducam 64MP cameras.
 * Copyright (C) 2021 Arducam Technology co., Ltd.
 *
 * Based on Sony IMX477 camera driver
 * Copyright (C) 2020 Raspberry Pi (Trading) Ltd
 */
#include <asm/unaligned.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

#define ARDUCAM_64MP_REG_VALUE_08BIT	1
#define ARDUCAM_64MP_REG_VALUE_16BIT	2

/* Chip ID */
#define ARDUCAM_64MP_REG_CHIP_ID	0x005E
#define ARDUCAM_64MP_CHIP_ID		0x4136

#define ARDUCAM_64MP_REG_MODE_SELECT	0x0100
#define ARDUCAM_64MP_MODE_STANDBY	0x00
#define ARDUCAM_64MP_MODE_STREAMING	0x01

#define ARDUCAM_64MP_REG_ORIENTATION	0x101

#define ARDUCAM_64MP_XCLK_FREQ		24000000

#define ARDUCAM_64MP_DEFAULT_LINK_FREQ	456000000

/* Pixel rate is fixed at 900MHz for all the modes */
#define ARDUCAM_64MP_PIXEL_RATE		900000000

/* V_TIMING internal */
#define ARDUCAM_64MP_REG_FRAME_LENGTH	0x0340
#define ARDUCAM_64MP_FRAME_LENGTH_MAX	0xffff

/* Long exposure multiplier */
#define ARDUCAM_64MP_LONG_EXP_SHIFT_MAX	7
#define ARDUCAM_64MP_LONG_EXP_SHIFT_REG	0x3100

/* Exposure control */
#define ARDUCAM_64MP_REG_EXPOSURE	0x0202
#define ARDUCAM_64MP_EXPOSURE_OFFSET	48
#define ARDUCAM_64MP_EXPOSURE_MIN	9
#define ARDUCAM_64MP_EXPOSURE_STEP	1
#define ARDUCAM_64MP_EXPOSURE_DEFAULT	0x3e8
#define ARDUCAM_64MP_EXPOSURE_MAX	(ARDUCAM_64MP_FRAME_LENGTH_MAX - \
					 ARDUCAM_64MP_EXPOSURE_OFFSET)

/* Analog gain control */
#define ARDUCAM_64MP_REG_ANALOG_GAIN		0x0204
#define ARDUCAM_64MP_ANA_GAIN_MIN		0
#define ARDUCAM_64MP_ANA_GAIN_MAX		1008
#define ARDUCAM_64MP_ANA_GAIN_STEP		1
#define ARDUCAM_64MP_ANA_GAIN_DEFAULT		0x0

/* Digital gain control */
#define ARDUCAM_64MP_REG_DIGITAL_GAIN		0x020e
#define ARDUCAM_64MP_DGTL_GAIN_MIN		0x0100
#define ARDUCAM_64MP_DGTL_GAIN_MAX		0x0fff
#define ARDUCAM_64MP_DGTL_GAIN_DEFAULT		0x0100
#define ARDUCAM_64MP_DGTL_GAIN_STEP		1

/* Test Pattern Control */
#define ARDUCAM_64MP_REG_TEST_PATTERN		0x0600
#define ARDUCAM_64MP_TEST_PATTERN_DISABLE	0
#define ARDUCAM_64MP_TEST_PATTERN_SOLID_COLOR	1
#define ARDUCAM_64MP_TEST_PATTERN_COLOR_BARS	2
#define ARDUCAM_64MP_TEST_PATTERN_GREY_COLOR	3
#define ARDUCAM_64MP_TEST_PATTERN_PN9		4

/* Test pattern colour components */
#define ARDUCAM_64MP_REG_TEST_PATTERN_R		0x0602
#define ARDUCAM_64MP_REG_TEST_PATTERN_GR	0x0604
#define ARDUCAM_64MP_REG_TEST_PATTERN_B		0x0606
#define ARDUCAM_64MP_REG_TEST_PATTERN_GB	0x0608
#define ARDUCAM_64MP_TEST_PATTERN_COLOUR_MIN	0
#define ARDUCAM_64MP_TEST_PATTERN_COLOUR_MAX	0x0fff
#define ARDUCAM_64MP_TEST_PATTERN_COLOUR_STEP	1
#define ARDUCAM_64MP_TEST_PATTERN_R_DEFAULT	\
	ARDUCAM_64MP_TEST_PATTERN_COLOUR_MAX
#define ARDUCAM_64MP_TEST_PATTERN_GR_DEFAULT	0
#define ARDUCAM_64MP_TEST_PATTERN_B_DEFAULT	0
#define ARDUCAM_64MP_TEST_PATTERN_GB_DEFAULT	0

/* Embedded metadata stream structure */
#define ARDUCAM_64MP_EMBEDDED_LINE_WIDTH (11560 * 3)
#define ARDUCAM_64MP_NUM_EMBEDDED_LINES 1

enum pad_types {
	IMAGE_PAD,
	METADATA_PAD,
	NUM_PADS
};

/* ARDUCAM_64MP native and active pixel array size. */
#define ARDUCAM_64MP_NATIVE_WIDTH		9344U
#define ARDUCAM_64MP_NATIVE_HEIGHT		7032U
#define ARDUCAM_64MP_PIXEL_ARRAY_LEFT		48U
#define ARDUCAM_64MP_PIXEL_ARRAY_TOP		40U
#define ARDUCAM_64MP_PIXEL_ARRAY_WIDTH		9248U
#define ARDUCAM_64MP_PIXEL_ARRAY_HEIGHT		6944U

struct arducam_64mp_reg {
	u16 address;
	u8 val;
};

struct arducam_64mp_reg_list {
	unsigned int num_of_regs;
	const struct arducam_64mp_reg *regs;
};

/* Mode : resolution and related config&values */
struct arducam_64mp_mode {
	/* Frame width */
	unsigned int width;

	/* Frame height */
	unsigned int height;

	/* H-timing in pixels */
	unsigned int line_length_pix;

	/* Analog crop rectangle. */
	struct v4l2_rect crop;

	/* Default framerate. */
	struct v4l2_fract timeperframe_default;

	/* Default register values */
	struct arducam_64mp_reg_list reg_list;
};

static const s64 arducam_64mp_link_freq_menu[] = {
	ARDUCAM_64MP_DEFAULT_LINK_FREQ,
};

static const struct arducam_64mp_reg mode_common_regs[] = {
	{0x0100, 0x00},
	{0x0136, 0x18},
	{0x0137, 0x00},
	{0x33F0, 0x01},
	{0x33F1, 0x03},
	{0x0111, 0x02},
	{0x3062, 0x00},
	{0x3063, 0x30},
	{0x3076, 0x00},
	{0x3077, 0x30},
	{0x1f06, 0x06},
	{0x1f07, 0x82},
	{0x1f04, 0x71},
	{0x1f05, 0x01},
	{0x1f08, 0x01},
	{0x5bfe, 0x14},
	{0x5c0d, 0x2d},
	{0x5c1c, 0x30},
	{0x5c2b, 0x32},
	{0x5c37, 0x2e},
	{0x5c40, 0x30},
	{0x5c50, 0x14},
	{0x5c5f, 0x28},
	{0x5c6e, 0x28},
	{0x5c7d, 0x32},
	{0x5c89, 0x37},
	{0x5c92, 0x56},
	{0x5bfc, 0x12},
	{0x5c0b, 0x2a},
	{0x5c1a, 0x2c},
	{0x5c29, 0x2f},
	{0x5c36, 0x2e},
	{0x5c3f, 0x2e},
	{0x5c4e, 0x06},
	{0x5c5d, 0x1e},
	{0x5c6c, 0x20},
	{0x5c7b, 0x1e},
	{0x5c88, 0x32},
	{0x5c91, 0x32},
	{0x5c02, 0x14},
	{0x5c11, 0x2f},
	{0x5c20, 0x32},
	{0x5c2f, 0x34},
	{0x5c39, 0x31},
	{0x5c42, 0x31},
	{0x5c8b, 0x28},
	{0x5c94, 0x28},
	{0x5c00, 0x10},
	{0x5c0f, 0x2c},
	{0x5c1e, 0x2e},
	{0x5c2d, 0x32},
	{0x5c38, 0x2e},
	{0x5c41, 0x2b},
	{0x5c61, 0x0a},
	{0x5c70, 0x0a},
	{0x5c7f, 0x0a},
	{0x5c8a, 0x1e},
	{0x5c93, 0x2a},
	{0x5bfa, 0x2b},
	{0x5c09, 0x2d},
	{0x5c18, 0x2e},
	{0x5c27, 0x30},
	{0x5c5b, 0x28},
	{0x5c6a, 0x22},
	{0x5c79, 0x42},
	{0x5bfb, 0x2c},
	{0x5c0a, 0x2f},
	{0x5c19, 0x2e},
	{0x5c28, 0x2e},
	{0x5c4d, 0x20},
	{0x5c5c, 0x1e},
	{0x5c6b, 0x32},
	{0x5c7a, 0x32},
	{0x5bfd, 0x30},
	{0x5c0c, 0x32},
	{0x5c1b, 0x2e},
	{0x5c2a, 0x30},
	{0x5c4f, 0x28},
	{0x5c5e, 0x32},
	{0x5c6d, 0x37},
	{0x5c7c, 0x56},
	{0x5bff, 0x2e},
	{0x5c0e, 0x32},
	{0x5c1d, 0x2e},
	{0x5c2c, 0x2b},
	{0x5c51, 0x0a},
	{0x5c60, 0x0a},
	{0x5c6f, 0x1e},
	{0x5c7e, 0x2a},
	{0x5c01, 0x32},
	{0x5c10, 0x34},
	{0x5c1f, 0x31},
	{0x5c2e, 0x31},
	{0x5c71, 0x28},
	{0x5c80, 0x28},
	{0x5c4c, 0x2a},
	{0x33f2, 0x01},
	{0x1f04, 0x73},
	{0x1f05, 0x01},
	{0x5bfa, 0x35},
	{0x5c09, 0x38},
	{0x5c18, 0x3a},
	{0x5c27, 0x38},
	{0x5c5b, 0x25},
	{0x5c6a, 0x24},
	{0x5c79, 0x47},
	{0x5bfc, 0x15},
	{0x5c0b, 0x2e},
	{0x5c1a, 0x36},
	{0x5c29, 0x38},
	{0x5c36, 0x36},
	{0x5c3f, 0x36},
	{0x5c4e, 0x0b},
	{0x5c5d, 0x20},
	{0x5c6c, 0x2a},
	{0x5c7b, 0x25},
	{0x5c88, 0x25},
	{0x5c91, 0x22},
	{0x5bfe, 0x15},
	{0x5c0d, 0x32},
	{0x5c1c, 0x36},
	{0x5c2b, 0x36},
	{0x5c37, 0x3a},
	{0x5c40, 0x39},
	{0x5c50, 0x06},
	{0x5c5f, 0x22},
	{0x5c6e, 0x23},
	{0x5c7d, 0x2e},
	{0x5c89, 0x44},
	{0x5c92, 0x51},
	{0x5d7f, 0x0a},
	{0x5c00, 0x17},
	{0x5c0f, 0x36},
	{0x5c1e, 0x38},
	{0x5c2d, 0x3c},
	{0x5c38, 0x38},
	{0x5c41, 0x36},
	{0x5c52, 0x0a},
	{0x5c61, 0x21},
	{0x5c70, 0x23},
	{0x5c7f, 0x1b},
	{0x5c8a, 0x22},
	{0x5c93, 0x20},
	{0x5c02, 0x1a},
	{0x5c11, 0x3e},
	{0x5c20, 0x3f},
	{0x5c2f, 0x3d},
	{0x5c39, 0x3e},
	{0x5c42, 0x3c},
	{0x5c54, 0x02},
	{0x5c63, 0x12},
	{0x5c72, 0x14},
	{0x5c81, 0x24},
	{0x5c8b, 0x1c},
	{0x5c94, 0x4e},
	{0x5d8a, 0x09},
	{0x5bfb, 0x36},
	{0x5c0a, 0x38},
	{0x5c19, 0x36},
	{0x5c28, 0x36},
	{0x5c4d, 0x2a},
	{0x5c5c, 0x25},
	{0x5c6b, 0x25},
	{0x5c7a, 0x22},
	{0x5bfd, 0x36},
	{0x5c0c, 0x36},
	{0x5c1b, 0x3a},
	{0x5c2a, 0x39},
	{0x5c4f, 0x23},
	{0x5c5e, 0x2e},
	{0x5c6d, 0x44},
	{0x5c7c, 0x51},
	{0x5d63, 0x0a},
	{0x5bff, 0x38},
	{0x5c0e, 0x3c},
	{0x5c1d, 0x38},
	{0x5c2c, 0x36},
	{0x5c51, 0x23},
	{0x5c60, 0x1b},
	{0x5c6f, 0x22},
	{0x5c7e, 0x20},
	{0x5c01, 0x3f},
	{0x5c10, 0x3d},
	{0x5c1f, 0x3e},
	{0x5c2e, 0x3c},
	{0x5c53, 0x14},
	{0x5c62, 0x24},
	{0x5c71, 0x1c},
	{0x5c80, 0x4e},
	{0x5d76, 0x09},
	{0x5c4c, 0x2a},
	{0x33f2, 0x02},
	{0x1f04, 0x78},
	{0x1f05, 0x01},
	{0x5bfa, 0x37},
	{0x5c09, 0x36},
	{0x5c18, 0x39},
	{0x5c27, 0x38},
	{0x5c5b, 0x27},
	{0x5c6a, 0x2b},
	{0x5c79, 0x48},
	{0x5bfc, 0x16},
	{0x5c0b, 0x32},
	{0x5c1a, 0x33},
	{0x5c29, 0x37},
	{0x5c36, 0x36},
	{0x5c3f, 0x35},
	{0x5c4e, 0x0d},
	{0x5c5d, 0x2d},
	{0x5c6c, 0x23},
	{0x5c7b, 0x25},
	{0x5c88, 0x31},
	{0x5c91, 0x2e},
	{0x5bfe, 0x15},
	{0x5c0d, 0x31},
	{0x5c1c, 0x35},
	{0x5c2b, 0x36},
	{0x5c37, 0x35},
	{0x5c40, 0x37},
	{0x5c50, 0x0f},
	{0x5c5f, 0x31},
	{0x5c6e, 0x30},
	{0x5c7d, 0x33},
	{0x5c89, 0x36},
	{0x5c92, 0x5b},
	{0x5c00, 0x13},
	{0x5c0f, 0x2f},
	{0x5c1e, 0x2e},
	{0x5c2d, 0x34},
	{0x5c38, 0x33},
	{0x5c41, 0x32},
	{0x5c52, 0x0d},
	{0x5c61, 0x27},
	{0x5c70, 0x28},
	{0x5c7f, 0x1f},
	{0x5c8a, 0x25},
	{0x5c93, 0x2c},
	{0x5c02, 0x15},
	{0x5c11, 0x36},
	{0x5c20, 0x39},
	{0x5c2f, 0x3a},
	{0x5c39, 0x37},
	{0x5c42, 0x37},
	{0x5c54, 0x04},
	{0x5c63, 0x1c},
	{0x5c72, 0x1c},
	{0x5c81, 0x1c},
	{0x5c8b, 0x28},
	{0x5c94, 0x24},
	{0x5bfb, 0x33},
	{0x5c0a, 0x37},
	{0x5c19, 0x36},
	{0x5c28, 0x35},
	{0x5c4d, 0x23},
	{0x5c5c, 0x25},
	{0x5c6b, 0x31},
	{0x5c7a, 0x2e},
	{0x5bfd, 0x35},
	{0x5c0c, 0x36},
	{0x5c1b, 0x35},
	{0x5c2a, 0x37},
	{0x5c4f, 0x30},
	{0x5c5e, 0x33},
	{0x5c6d, 0x36},
	{0x5c7c, 0x5b},
	{0x5bff, 0x2e},
	{0x5c0e, 0x34},
	{0x5c1d, 0x33},
	{0x5c2c, 0x32},
	{0x5c51, 0x28},
	{0x5c60, 0x1f},
	{0x5c6f, 0x25},
	{0x5c7e, 0x2c},
	{0x5c01, 0x39},
	{0x5c10, 0x3a},
	{0x5c1f, 0x37},
	{0x5c2e, 0x37},
	{0x5c53, 0x1c},
	{0x5c62, 0x1c},
	{0x5c71, 0x28},
	{0x5c80, 0x24},
	{0x5c4c, 0x2c},
	{0x33f2, 0x03},
	{0x1f08, 0x00},
	{0x32c8, 0x00},
	{0x4017, 0x40},
	{0x40a2, 0x01},
	{0x40ac, 0x01},
	{0x4328, 0x00},
	{0x4329, 0xb3},
	{0x4e15, 0x10},
	{0x4e19, 0x2f},
	{0x4e21, 0x0f},
	{0x4e2f, 0x10},
	{0x4e3d, 0x10},
	{0x4e41, 0x2f},
	{0x4e57, 0x29},
	{0x4ffb, 0x2f},
	{0x5011, 0x24},
	{0x501d, 0x03},
	{0x505f, 0x41},
	{0x5060, 0xdf},
	{0x5065, 0xdf},
	{0x5066, 0x37},
	{0x506e, 0x57},
	{0x5070, 0xc5},
	{0x5072, 0x57},
	{0x5075, 0x53},
	{0x5076, 0x55},
	{0x5077, 0xc1},
	{0x5078, 0xc3},
	{0x5079, 0x53},
	{0x507a, 0x55},
	{0x507d, 0x57},
	{0x507e, 0xdf},
	{0x507f, 0xc5},
	{0x5081, 0x57},
	{0x53c8, 0x01},
	{0x53c9, 0xe2},
	{0x53ca, 0x03},
	{0x5422, 0x7a},
	{0x548e, 0x40},
	{0x5497, 0x5e},
	{0x54a1, 0x40},
	{0x54a9, 0x40},
	{0x54b2, 0x5e},
	{0x54bc, 0x40},
	{0x57c6, 0x00},
	{0x583d, 0x0e},
	{0x583e, 0x0e},
	{0x583f, 0x0e},
	{0x5840, 0x0e},
	{0x5841, 0x0e},
	{0x5842, 0x0e},
	{0x5900, 0x12},
	{0x5901, 0x12},
	{0x5902, 0x14},
	{0x5903, 0x12},
	{0x5904, 0x14},
	{0x5905, 0x12},
	{0x5906, 0x14},
	{0x5907, 0x12},
	{0x590f, 0x12},
	{0x5911, 0x12},
	{0x5913, 0x12},
	{0x591c, 0x12},
	{0x591e, 0x12},
	{0x5920, 0x12},
	{0x5948, 0x08},
	{0x5949, 0x08},
	{0x594a, 0x08},
	{0x594b, 0x08},
	{0x594c, 0x08},
	{0x594d, 0x08},
	{0x594e, 0x08},
	{0x594f, 0x08},
	{0x595c, 0x08},
	{0x595e, 0x08},
	{0x5960, 0x08},
	{0x596e, 0x08},
	{0x5970, 0x08},
	{0x5972, 0x08},
	{0x597e, 0x0f},
	{0x597f, 0x0f},
	{0x599a, 0x0f},
	{0x59de, 0x08},
	{0x59df, 0x08},
	{0x59fa, 0x08},
	{0x5a59, 0x22},
	{0x5a5b, 0x22},
	{0x5a5d, 0x1a},
	{0x5a5f, 0x22},
	{0x5a61, 0x1a},
	{0x5a63, 0x22},
	{0x5a65, 0x1a},
	{0x5a67, 0x22},
	{0x5a77, 0x22},
	{0x5a7b, 0x22},
	{0x5a7f, 0x22},
	{0x5a91, 0x22},
	{0x5a95, 0x22},
	{0x5a99, 0x22},
	{0x5ae9, 0x66},
	{0x5aeb, 0x66},
	{0x5aed, 0x5e},
	{0x5aef, 0x66},
	{0x5af1, 0x5e},
	{0x5af3, 0x66},
	{0x5af5, 0x5e},
	{0x5af7, 0x66},
	{0x5b07, 0x66},
	{0x5b0b, 0x66},
	{0x5b0f, 0x66},
	{0x5b21, 0x66},
	{0x5b25, 0x66},
	{0x5b29, 0x66},
	{0x5b79, 0x46},
	{0x5b7b, 0x3e},
	{0x5b7d, 0x3e},
	{0x5b89, 0x46},
	{0x5b8b, 0x46},
	{0x5b97, 0x46},
	{0x5b99, 0x46},
	{0x5c9e, 0x0a},
	{0x5c9f, 0x08},
	{0x5ca0, 0x0a},
	{0x5ca1, 0x0a},
	{0x5ca2, 0x0b},
	{0x5ca3, 0x06},
	{0x5ca4, 0x04},
	{0x5ca5, 0x06},
	{0x5ca6, 0x04},
	{0x5cad, 0x0b},
	{0x5cae, 0x0a},
	{0x5caf, 0x0c},
	{0x5cb0, 0x0a},
	{0x5cb1, 0x0b},
	{0x5cb2, 0x08},
	{0x5cb3, 0x06},
	{0x5cb4, 0x08},
	{0x5cb5, 0x04},
	{0x5cbc, 0x0b},
	{0x5cbd, 0x09},
	{0x5cbe, 0x08},
	{0x5cbf, 0x09},
	{0x5cc0, 0x0a},
	{0x5cc1, 0x08},
	{0x5cc2, 0x06},
	{0x5cc3, 0x08},
	{0x5cc4, 0x06},
	{0x5ccb, 0x0a},
	{0x5ccc, 0x09},
	{0x5ccd, 0x0a},
	{0x5cce, 0x08},
	{0x5ccf, 0x0a},
	{0x5cd0, 0x08},
	{0x5cd1, 0x08},
	{0x5cd2, 0x08},
	{0x5cd3, 0x08},
	{0x5cda, 0x09},
	{0x5cdb, 0x09},
	{0x5cdc, 0x08},
	{0x5cdd, 0x08},
	{0x5ce3, 0x09},
	{0x5ce4, 0x08},
	{0x5ce5, 0x08},
	{0x5ce6, 0x08},
	{0x5cf4, 0x04},
	{0x5d04, 0x04},
	{0x5d13, 0x06},
	{0x5d22, 0x06},
	{0x5d23, 0x04},
	{0x5d2e, 0x06},
	{0x5d37, 0x06},
	{0x5d6f, 0x09},
	{0x5d72, 0x0f},
	{0x5d88, 0x0f},
	{0x5de6, 0x01},
	{0x5de7, 0x01},
	{0x5de8, 0x01},
	{0x5de9, 0x01},
	{0x5dea, 0x01},
	{0x5deb, 0x01},
	{0x5dec, 0x01},
	{0x5df2, 0x01},
	{0x5df3, 0x01},
	{0x5df4, 0x01},
	{0x5df5, 0x01},
	{0x5df6, 0x01},
	{0x5df7, 0x01},
	{0x5df8, 0x01},
	{0x5dfe, 0x01},
	{0x5dff, 0x01},
	{0x5e00, 0x01},
	{0x5e01, 0x01},
	{0x5e02, 0x01},
	{0x5e03, 0x01},
	{0x5e04, 0x01},
	{0x5e0a, 0x01},
	{0x5e0b, 0x01},
	{0x5e0c, 0x01},
	{0x5e0d, 0x01},
	{0x5e0e, 0x01},
	{0x5e0f, 0x01},
	{0x5e10, 0x01},
	{0x5e16, 0x01},
	{0x5e17, 0x01},
	{0x5e18, 0x01},
	{0x5e1e, 0x01},
	{0x5e1f, 0x01},
	{0x5e20, 0x01},
	{0x5e6e, 0x5a},
	{0x5e6f, 0x46},
	{0x5e70, 0x46},
	{0x5e71, 0x3c},
	{0x5e72, 0x3c},
	{0x5e73, 0x28},
	{0x5e74, 0x28},
	{0x5e75, 0x6e},
	{0x5e76, 0x6e},
	{0x5e81, 0x46},
	{0x5e83, 0x3c},
	{0x5e85, 0x28},
	{0x5e87, 0x6e},
	{0x5e92, 0x46},
	{0x5e94, 0x3c},
	{0x5e96, 0x28},
	{0x5e98, 0x6e},
	{0x5ecb, 0x26},
	{0x5ecc, 0x26},
	{0x5ecd, 0x26},
	{0x5ece, 0x26},
	{0x5ed2, 0x26},
	{0x5ed3, 0x26},
	{0x5ed4, 0x26},
	{0x5ed5, 0x26},
	{0x5ed9, 0x26},
	{0x5eda, 0x26},
	{0x5ee5, 0x08},
	{0x5ee6, 0x08},
	{0x5ee7, 0x08},
	{0x6006, 0x14},
	{0x6007, 0x14},
	{0x6008, 0x14},
	{0x6009, 0x14},
	{0x600a, 0x14},
	{0x600b, 0x14},
	{0x600c, 0x14},
	{0x600d, 0x22},
	{0x600e, 0x22},
	{0x600f, 0x14},
	{0x601a, 0x14},
	{0x601b, 0x14},
	{0x601c, 0x14},
	{0x601d, 0x14},
	{0x601e, 0x14},
	{0x601f, 0x14},
	{0x6020, 0x14},
	{0x6021, 0x22},
	{0x6022, 0x22},
	{0x6023, 0x14},
	{0x602e, 0x14},
	{0x602f, 0x14},
	{0x6030, 0x14},
	{0x6031, 0x22},
	{0x6039, 0x14},
	{0x603a, 0x14},
	{0x603b, 0x14},
	{0x603c, 0x22},
	{0x6132, 0x0f},
	{0x6133, 0x0f},
	{0x6134, 0x0f},
	{0x6135, 0x0f},
	{0x6136, 0x0f},
	{0x6137, 0x0f},
	{0x6138, 0x0f},
	{0x613e, 0x0f},
	{0x613f, 0x0f},
	{0x6140, 0x0f},
	{0x6141, 0x0f},
	{0x6142, 0x0f},
	{0x6143, 0x0f},
	{0x6144, 0x0f},
	{0x614a, 0x0f},
	{0x614b, 0x0f},
	{0x614c, 0x0f},
	{0x614d, 0x0f},
	{0x614e, 0x0f},
	{0x614f, 0x0f},
	{0x6150, 0x0f},
	{0x6156, 0x0f},
	{0x6157, 0x0f},
	{0x6158, 0x0f},
	{0x6159, 0x0f},
	{0x615a, 0x0f},
	{0x615b, 0x0f},
	{0x615c, 0x0f},
	{0x6162, 0x0f},
	{0x6163, 0x0f},
	{0x6164, 0x0f},
	{0x616a, 0x0f},
	{0x616b, 0x0f},
	{0x616c, 0x0f},
	{0x6226, 0x00},
	{0x84f8, 0x01},
	{0x8501, 0x00},
	{0x8502, 0x01},
	{0x8505, 0x00},
	{0x8744, 0x00},
	{0x883c, 0x01},
	{0x8845, 0x00},
	{0x8846, 0x01},
	{0x8849, 0x00},
	{0x9004, 0x1f},
	{0x9064, 0x4d},
	{0x9065, 0x3d},
	{0x922e, 0x91},
	{0x922f, 0x2a},
	{0x9230, 0xe2},
	{0x9231, 0xc0},
	{0x9232, 0xe2},
	{0x9233, 0xc1},
	{0x9234, 0xe2},
	{0x9235, 0xc2},
	{0x9236, 0xe2},
	{0x9237, 0xc3},
	{0x9238, 0xe2},
	{0x9239, 0xd4},
	{0x923a, 0xe2},
	{0x923b, 0xd5},
	{0x923c, 0x90},
	{0x923d, 0x64},
	{0xb0b9, 0x10},
	{0xbc76, 0x00},
	{0xbc77, 0x00},
	{0xbc78, 0x00},
	{0xbc79, 0x00},
	{0xbc7b, 0x28},
	{0xbc7c, 0x00},
	{0xbc7d, 0x00},
	{0xbc7f, 0xc0},
	{0xc6b9, 0x01},
	{0xecb5, 0x04},
	{0xecbf, 0x04},
	{0x0112, 0x0a},
	{0x0113, 0x0a},
	{0x0114, 0x01},
	{0x0301, 0x08},
	{0x0303, 0x02},
	{0x0305, 0x04},
	{0x0306, 0x01},
	{0x0307, 0x2c},
	{0x030b, 0x02},
	{0x030d, 0x04},
	{0x030e, 0x01},
	{0x030f, 0x30},
	{0x0310, 0x01},
	{0x4018, 0x00},
	{0x4019, 0x00},
	{0x401a, 0x00},
	{0x401b, 0x00},
	{0x3400, 0x01},
	{0x3092, 0x01},
	{0x3093, 0x00},
	{0x0350, 0x00},
	{0x3419, 0x00},
};

/* 64 mpix 2.7fps */
static const struct arducam_64mp_reg mode_9152x6944_regs[] = {
	{0x0342, 0xb6},
	{0x0343, 0xb2},
	{0x0340, 0x1b},
	{0x0341, 0x76},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x24},
	{0x0349, 0x1f},
	{0x034a, 0x1b},
	{0x034b, 0x1f},
	{0x0900, 0x00},
	{0x0901, 0x11},
	{0x0902, 0x0a},
	{0x30d8, 0x00},
	{0x3200, 0x01},
	{0x3201, 0x01},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040a, 0x00},
	{0x040b, 0x00},
	{0x040c, 0x23},
	{0x040d, 0xc0},
	{0x040e, 0x1b},
	{0x040f, 0x20},
	{0x034c, 0x23},
	{0x034d, 0xc0},
	{0x034e, 0x1b},
	{0x034f, 0x20},
	{0x30d9, 0x01},
	{0x32d5, 0x01},
	{0x32d6, 0x00},
	{0x401e, 0x00},
	{0x40b8, 0x04},
	{0x40b9, 0x20},
	{0x40bc, 0x02},
	{0x40bd, 0x58},
	{0x40be, 0x02},
	{0x40bf, 0x58},
	{0x41a4, 0x00},
	{0x5a09, 0x01},
	{0x5a17, 0x01},
	{0x5a25, 0x01},
	{0x5a33, 0x01},
	{0x98d7, 0x14},
	{0x98d8, 0x14},
	{0x98d9, 0x00},
	{0x99c4, 0x00},
	{0x0202, 0x03},
	{0x0203, 0xe8},
	{0x0204, 0x00},
	{0x0205, 0x00},
	{0x020e, 0x01},
	{0x020f, 0x00},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x02},
	{0x341f, 0x3c},
	{0x3420, 0x02},
	{0x3421, 0x42},
};

/* 48 mpix 3.0fps */
static const struct arducam_64mp_reg mode_8000x6000_regs[] = {
	{0x0342, 0xb6},
	{0x0343, 0xb2},
	{0x0340, 0x19},
	{0x0341, 0x0e},
	{0x0344, 0x02},
	{0x0345, 0x70},
	{0x0346, 0x01},
	{0x0347, 0xd8},
	{0x0348, 0x21},
	{0x0349, 0xaf},
	{0x034a, 0x19},
	{0x034b, 0x47},
	{0x0900, 0x00},
	{0x0901, 0x11},
	{0x0902, 0x0a},
	{0x30d8, 0x00},
	{0x3200, 0x01},
	{0x3201, 0x01},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040a, 0x00},
	{0x040b, 0x00},
	{0x040c, 0x1f},
	{0x040d, 0x40},
	{0x040e, 0x17},
	{0x040f, 0x70},
	{0x034c, 0x1f},
	{0x034d, 0x40},
	{0x034e, 0x17},
	{0x034f, 0x70},
	{0x30d9, 0x01},
	{0x32d5, 0x01},
	{0x32d6, 0x00},
	{0x401e, 0x00},
	{0x40b8, 0x04},
	{0x40b9, 0x20},
	{0x40bc, 0x02},
	{0x40bd, 0x58},
	{0x40be, 0x02},
	{0x40bf, 0x58},
	{0x41a4, 0x00},
	{0x5a09, 0x01},
	{0x5a17, 0x01},
	{0x5a25, 0x01},
	{0x5a33, 0x01},
	{0x98d7, 0x14},
	{0x98d8, 0x14},
	{0x98d9, 0x00},
	{0x99c4, 0x00},
	{0x0202, 0x03},
	{0x0203, 0xe8},
	{0x0204, 0x00},
	{0x0205, 0x00},
	{0x020e, 0x01},
	{0x020f, 0x00},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x01},
	{0x341f, 0xf4},
	{0x3420, 0x01},
	{0x3421, 0xf4},
};

/* 16 mpix 10fps */
static const struct arducam_64mp_reg mode_4624x3472_regs[] = {
	{0x0342, 0x63},
	{0x0343, 0x97},
	{0x0340, 0x0d},
	{0x0341, 0xca},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x24},
	{0x0349, 0x1f},
	{0x034a, 0x1b},
	{0x034b, 0x1f},
	{0x0900, 0x01},
	{0x0901, 0x22},
	{0x0902, 0x08},
	{0x30d8, 0x04},
	{0x3200, 0x41},
	{0x3201, 0x41},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040a, 0x00},
	{0x040b, 0x00},
	{0x040c, 0x12},
	{0x040d, 0x10},
	{0x040e, 0x0d},
	{0x040f, 0x90},
	{0x034c, 0x12},
	{0x034d, 0x10},
	{0x034e, 0x0d},
	{0x034f, 0x90},
	{0x30d9, 0x00},
	{0x32d5, 0x00},
	{0x32d6, 0x00},
	{0x401e, 0x00},
	{0x40b8, 0x01},
	{0x40b9, 0x2c},
	{0x40bc, 0x01},
	{0x40bd, 0x18},
	{0x40be, 0x00},
	{0x40bf, 0x00},
	{0x41a4, 0x00},
	{0x5a09, 0x01},
	{0x5a17, 0x01},
	{0x5a25, 0x01},
	{0x5a33, 0x01},
	{0x98d7, 0xb4},
	{0x98d8, 0x8c},
	{0x98d9, 0x0a},
	{0x99c4, 0x16},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x01},
	{0x341f, 0x21},
	{0x3420, 0x01},
	{0x3421, 0x21},
};

/* 4k 20fps mode */
static const struct arducam_64mp_reg mode_3840x2160_regs[] = {
	{0x0342, 0x4e},
	{0x0343, 0xb7},
	{0x0340, 0x08},
	{0x0341, 0xb9},
	{0x0344, 0x03},
	{0x0345, 0x10},
	{0x0346, 0x05},
	{0x0347, 0x20},
	{0x0348, 0x21},
	{0x0349, 0x0f},
	{0x034a, 0x15},
	{0x034b, 0xff},
	{0x0900, 0x01},
	{0x0901, 0x22},
	{0x0902, 0x08},
	{0x30d8, 0x04},
	{0x3200, 0x41},
	{0x3201, 0x41},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040a, 0x00},
	{0x040b, 0x00},
	{0x040c, 0x0f},
	{0x040d, 0x00},
	{0x040e, 0x08},
	{0x040f, 0x70},
	{0x034c, 0x0f},
	{0x034d, 0x00},
	{0x034e, 0x08},
	{0x034f, 0x70},
	{0x30d9, 0x00},
	{0x32d5, 0x00},
	{0x32d6, 0x00},
	{0x401e, 0x00},
	{0x40b8, 0x01},
	{0x40b9, 0x2c},
	{0x40bc, 0x01},
	{0x40bd, 0x18},
	{0x40be, 0x00},
	{0x40bf, 0x00},
	{0x41a4, 0x00},
	{0x5a09, 0x01},
	{0x5a17, 0x01},
	{0x5a25, 0x01},
	{0x5a33, 0x01},
	{0x98d7, 0xb4},
	{0x98d8, 0x8c},
	{0x98d9, 0x0a},
	{0x99c4, 0x16},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x00},
	{0x341f, 0xf0},
	{0x3420, 0x00},
	{0x3421, 0xb4},
};

/* 4x4 binned 30fps mode */
static const struct arducam_64mp_reg mode_2312x1736_regs[] = {
	{0x0342, 0x33},
	{0x0343, 0x60},
	{0x0340, 0x08},
	{0x0341, 0xe9},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x24},
	{0x0349, 0x1f},
	{0x034a, 0x1b},
	{0x034b, 0x1f},
	{0x0900, 0x01},
	{0x0901, 0x44},
	{0x0902, 0x08},
	{0x30d8, 0x04},
	{0x3200, 0x43},
	{0x3201, 0x43},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040a, 0x00},
	{0x040b, 0x00},
	{0x040c, 0x09},
	{0x040d, 0x08},
	{0x040e, 0x06},
	{0x040f, 0xc8},
	{0x034c, 0x09},
	{0x034d, 0x08},
	{0x034e, 0x06},
	{0x034f, 0xc8},
	{0x30d9, 0x00},
	{0x32d5, 0x00},
	{0x32d6, 0x00},
	{0x401e, 0x00},
	{0x40b8, 0x01},
	{0x40b9, 0x2c},
	{0x40bc, 0x01},
	{0x40bd, 0x18},
	{0x40be, 0x00},
	{0x40bf, 0x00},
	{0x41a4, 0x00},
	{0x5a09, 0x01},
	{0x5a17, 0x01},
	{0x5a25, 0x01},
	{0x5a33, 0x01},
	{0x98d7, 0xb4},
	{0x98d8, 0x8c},
	{0x98d9, 0x0a},
	{0x99c4, 0x16},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x00},
	{0x341f, 0x90},
	{0x3420, 0x00},
	{0x3421, 0x90},
};

/* 1080p 60fps mode */
static const struct arducam_64mp_reg mode_1920x1080_regs[] = {
	{0x0342, 0x29},
	{0x0343, 0xe3},
	{0x0340, 0x05},
	{0x0341, 0x76},
	{0x0344, 0x03},
	{0x0345, 0x10},
	{0x0346, 0x05},
	{0x0347, 0x20},
	{0x0348, 0x21},
	{0x0349, 0x0f},
	{0x034a, 0x16},
	{0x034b, 0x0f},
	{0x0900, 0x01},
	{0x0901, 0x44},
	{0x0902, 0x08},
	{0x30d8, 0x04},
	{0x3200, 0x43},
	{0x3201, 0x43},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040a, 0x00},
	{0x040b, 0x00},
	{0x040c, 0x07},
	{0x040d, 0x80},
	{0x040e, 0x04},
	{0x040f, 0x38},
	{0x034c, 0x07},
	{0x034d, 0x80},
	{0x034e, 0x04},
	{0x034f, 0x38},
	{0x30d9, 0x00},
	{0x32d5, 0x00},
	{0x32d6, 0x00},
	{0x401e, 0x00},
	{0x40b8, 0x01},
	{0x40b9, 0x2c},
	{0x40bc, 0x01},
	{0x40bd, 0x18},
	{0x40be, 0x00},
	{0x40bf, 0x00},
	{0x41a4, 0x00},
	{0x5a09, 0x01},
	{0x5a17, 0x01},
	{0x5a25, 0x01},
	{0x5a33, 0x01},
	{0x98d7, 0xb4},
	{0x98d8, 0x8c},
	{0x98d9, 0x0a},
	{0x99c4, 0x16},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x00},
	{0x341f, 0x78},
	{0x3420, 0x00},
	{0x3421, 0x5a},
};

/* 720p 120fps mode */
static const struct arducam_64mp_reg mode_1280x720_regs[] = {
	{0x0342, 0x1b},
	{0x0343, 0x08},
	{0x0340, 0x04},
	{0x0341, 0x3b},
	{0x0344, 0x08},
	{0x0345, 0x10},
	{0x0346, 0x07},
	{0x0347, 0xf0},
	{0x0348, 0x1c},
	{0x0349, 0x0f},
	{0x034a, 0x13},
	{0x034b, 0x3f},
	{0x0900, 0x01},
	{0x0901, 0x44},
	{0x0902, 0x08},
	{0x30d8, 0x04},
	{0x3200, 0x43},
	{0x3201, 0x43},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040a, 0x00},
	{0x040b, 0x00},
	{0x040c, 0x05},
	{0x040d, 0x00},
	{0x040e, 0x02},
	{0x040f, 0xd0},
	{0x034c, 0x05},
	{0x034d, 0x00},
	{0x034e, 0x02},
	{0x034f, 0xd0},
	{0x30d9, 0x00},
	{0x32d5, 0x00},
	{0x32d6, 0x00},
	{0x401e, 0x00},
	{0x40b8, 0x01},
	{0x40b9, 0x2c},
	{0x40bc, 0x01},
	{0x40bd, 0x18},
	{0x40be, 0x00},
	{0x40bf, 0x00},
	{0x41a4, 0x00},
	{0x5a09, 0x01},
	{0x5a17, 0x01},
	{0x5a25, 0x01},
	{0x5a33, 0x01},
	{0x98d7, 0xb4},
	{0x98d8, 0x8c},
	{0x98d9, 0x0a},
	{0x99c4, 0x16},
	{0x341a, 0x00},
	{0x341b, 0x00},
	{0x341c, 0x00},
	{0x341d, 0x00},
	{0x341e, 0x00},
	{0x341f, 0x50},
	{0x3420, 0x00},
	{0x3421, 0x3c},
};

/* Mode configs */
static const struct arducam_64mp_mode supported_modes[] = {
	{
		.width = 9152,
		.height = 6944,
		.line_length_pix = 0xb6b2,
		.crop = {
			.left = ARDUCAM_64MP_PIXEL_ARRAY_LEFT,
			.top = ARDUCAM_64MP_PIXEL_ARRAY_TOP,
			.width = 9248,
			.height = 6944,
		},
		.timeperframe_default = {
			.numerator = 100,
			.denominator = 270
		},
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_9152x6944_regs),
			.regs = mode_9152x6944_regs,
		}
	}, {
		.width = 8000,
		.height = 6000,
		.line_length_pix = 0xb6b2,
		.crop = {
			.left = ARDUCAM_64MP_PIXEL_ARRAY_LEFT + 624,
			.top = ARDUCAM_64MP_PIXEL_ARRAY_TOP + 472,
			.width = 9248,
			.height = 6944,
		},
		.timeperframe_default = {
			.numerator = 100,
			.denominator = 300
		},
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_8000x6000_regs),
			.regs = mode_8000x6000_regs,
		}
	}, {
		.width = 4624,
		.height = 3472,
		.line_length_pix = 0x6397,
		.crop = {
			.left = ARDUCAM_64MP_PIXEL_ARRAY_LEFT,
			.top = ARDUCAM_64MP_PIXEL_ARRAY_TOP,
			.width = 9248,
			.height = 6944,
		},
		.timeperframe_default = {
			.numerator = 100,
			.denominator = 1000
		},
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_4624x3472_regs),
			.regs = mode_4624x3472_regs,
		}
	}, {
		.width = 3840,
		.height = 2160,
		.line_length_pix = 0x4eb7,
		.crop = {
			.left = ARDUCAM_64MP_PIXEL_ARRAY_LEFT + 784,
			.top = ARDUCAM_64MP_PIXEL_ARRAY_TOP + 1312,
			.width = 7680,
			.height = 4320,
		},
		.timeperframe_default = {
			.numerator = 100,
			.denominator = 2000
		},
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_3840x2160_regs),
			.regs = mode_3840x2160_regs,
		}
	}, {
		.width = 2312,
		.height = 1736,
		.line_length_pix = 0x3360,
		.crop = {
			.left = ARDUCAM_64MP_PIXEL_ARRAY_LEFT,
			.top = ARDUCAM_64MP_PIXEL_ARRAY_TOP,
			.width = 9248,
			.height = 6944,
		},
		.timeperframe_default = {
			.numerator = 100,
			.denominator = 3000
		},
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2312x1736_regs),
			.regs = mode_2312x1736_regs,
		}
	}, {
		.width = 1920,
		.height = 1080,
		.line_length_pix = 0x29e3,
		.crop = {
			.left = ARDUCAM_64MP_PIXEL_ARRAY_LEFT + 784,
			.top = ARDUCAM_64MP_PIXEL_ARRAY_TOP + 1312,
			.width = 7680,
			.height = 4320,
		},
		.timeperframe_default = {
			.numerator = 100,
			.denominator = 6000
		},
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1920x1080_regs),
			.regs = mode_1920x1080_regs,
		}
	}, {
		.width = 1280,
		.height = 720,
		.line_length_pix = 0x1b08,
		.crop = {
			.left = ARDUCAM_64MP_PIXEL_ARRAY_LEFT + 2064,
			.top = ARDUCAM_64MP_PIXEL_ARRAY_TOP + 2032,
			.width = 5120,
			.height = 2880,
		},
		.timeperframe_default = {
			.numerator = 100,
			.denominator = 12000
		},
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1280x720_regs),
			.regs = mode_1280x720_regs,
		}
	},
};

/*
 * The supported formats.
 * This table MUST contain 4 entries per format, to cover the various flip
 * combinations in the order
 * - no flip
 * - h flip
 * - v flip
 * - h&v flips
 */
static const u32 codes[] = {
	/* 10-bit modes. */
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,
};

static const char * const arducam_64mp_test_pattern_menu[] = {
	"Disabled",
	"Color Bars",
	"Solid Color",
	"Grey Color Bars",
	"PN9"
};

static const int arducam_64mp_test_pattern_val[] = {
	ARDUCAM_64MP_TEST_PATTERN_DISABLE,
	ARDUCAM_64MP_TEST_PATTERN_COLOR_BARS,
	ARDUCAM_64MP_TEST_PATTERN_SOLID_COLOR,
	ARDUCAM_64MP_TEST_PATTERN_GREY_COLOR,
	ARDUCAM_64MP_TEST_PATTERN_PN9,
};

/* regulator supplies */
static const char * const arducam_64mp_supply_name[] = {
	/* Supplies can be enabled in any order */
	"VANA",  /* Analog (2.8V) supply */
	"VDIG",  /* Digital Core (1.05V) supply */
	"VDDL",  /* IF (1.8V) supply */
};

#define ARDUCAM_64MP_NUM_SUPPLIES ARRAY_SIZE(arducam_64mp_supply_name)

/*
 * Initialisation delay between XCLR low->high and the moment when the sensor
 * can start capture (i.e. can leave software standby), given by T7 in the
 * datasheet is 7.7ms.  This does include I2C setup time as well.
 *
 * Note, that delay between XCLR low->high and reading the CCI ID register (T6
 * in the datasheet) is much smaller - 1ms.
 */
#define ARDUCAM_64MP_XCLR_MIN_DELAY_US		8000
#define ARDUCAM_64MP_XCLR_DELAY_RANGE_US	1000

struct arducam_64mp {
	struct v4l2_subdev sd;
	struct media_pad pad[NUM_PADS];

	unsigned int fmt_code;

	struct clk *xclk;

	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARDUCAM_64MP_NUM_SUPPLIES];

	struct v4l2_ctrl_handler ctrl_handler;
	/* V4L2 Controls */
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;

	/* Current mode */
	const struct arducam_64mp_mode *mode;

	/*
	 * Mutex for serialized access:
	 * Protect sensor module set pad format and start/stop streaming safely.
	 */
	struct mutex mutex;

	/* Streaming on/off */
	bool streaming;

	/* Rewrite common registers on stream on? */
	bool common_regs_written;

	/* Current long exposure factor in use. Set through V4L2_CID_VBLANK */
	unsigned int long_exp_shift;
};

static inline struct arducam_64mp *to_arducam_64mp(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct arducam_64mp, sd);
}

/* Read registers up to 2 at a time */
static int arducam_64mp_read_reg(struct i2c_client *client,
				 u16 reg, u32 len, u32 *val)
{
	struct i2c_msg msgs[2];
	u8 addr_buf[2] = { reg >> 8, reg & 0xff };
	u8 data_buf[4] = { 0, };
	int ret;

	if (len > 4)
		return -EINVAL;

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_buf[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = get_unaligned_be32(data_buf);

	return 0;
}

/* Write registers up to 2 at a time */
static int arducam_64mp_write_reg(struct arducam_64mp *arducam_64mp,
				  u16 reg, u32 len, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&arducam_64mp->sd);
	u8 buf[6];

	if (len > 4)
		return -EINVAL;

	put_unaligned_be16(reg, buf);
	put_unaligned_be32(val << (8 * (4 - len)), buf + 2);
	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

/* Write a list of registers */
static int arducam_64mp_write_regs(struct arducam_64mp *arducam_64mp,
				   const struct arducam_64mp_reg *regs, u32 len)
{
	struct i2c_client *client = v4l2_get_subdevdata(&arducam_64mp->sd);
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = arducam_64mp_write_reg(arducam_64mp, regs[i].address, 1,
					     regs[i].val);
		if (ret) {
			dev_err_ratelimited(&client->dev,
					    "Failed to write reg 0x%4.4x. error = %d\n",
					    regs[i].address, ret);

			return ret;
		}
	}

	return 0;
}

/* Get bayer order based on flip setting. */
static u32 arducam_64mp_get_format_code(struct arducam_64mp *arducam_64mp)
{
	unsigned int i;

	lockdep_assert_held(&arducam_64mp->mutex);

	i = (arducam_64mp->vflip->val ? 2 : 0) |
	    (arducam_64mp->hflip->val ? 1 : 0);

	return codes[i];
}

static int arducam_64mp_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct arducam_64mp *arducam_64mp = to_arducam_64mp(sd);
	struct v4l2_mbus_framefmt *try_fmt_img =
		v4l2_subdev_get_try_format(sd, fh->state, IMAGE_PAD);
	struct v4l2_mbus_framefmt *try_fmt_meta =
		v4l2_subdev_get_try_format(sd, fh->state, METADATA_PAD);
	struct v4l2_rect *try_crop;

	mutex_lock(&arducam_64mp->mutex);

	/* Initialize try_fmt for the image pad */
	try_fmt_img->width = supported_modes[0].width;
	try_fmt_img->height = supported_modes[0].height;
	try_fmt_img->code = arducam_64mp_get_format_code(arducam_64mp);
	try_fmt_img->field = V4L2_FIELD_NONE;

	/* Initialize try_fmt for the embedded metadata pad */
	try_fmt_meta->width = ARDUCAM_64MP_EMBEDDED_LINE_WIDTH;
	try_fmt_meta->height = ARDUCAM_64MP_NUM_EMBEDDED_LINES;
	try_fmt_meta->code = MEDIA_BUS_FMT_SENSOR_DATA;
	try_fmt_meta->field = V4L2_FIELD_NONE;

	/* Initialize try_crop */
	try_crop = v4l2_subdev_get_try_crop(sd, fh->state, IMAGE_PAD);
	try_crop->left = ARDUCAM_64MP_PIXEL_ARRAY_LEFT;
	try_crop->top = ARDUCAM_64MP_PIXEL_ARRAY_TOP;
	try_crop->width = ARDUCAM_64MP_PIXEL_ARRAY_WIDTH;
	try_crop->height = ARDUCAM_64MP_PIXEL_ARRAY_HEIGHT;

	mutex_unlock(&arducam_64mp->mutex);

	return 0;
}

static void
arducam_64mp_adjust_exposure_range(struct arducam_64mp *arducam_64mp)
{
	int exposure_max, exposure_def;

	/* Honour the VBLANK limits when setting exposure. */
	exposure_max = arducam_64mp->mode->height + arducam_64mp->vblank->val -
		       ARDUCAM_64MP_EXPOSURE_OFFSET;
	exposure_def = min(exposure_max, arducam_64mp->exposure->val);
	__v4l2_ctrl_modify_range(arducam_64mp->exposure,
				 arducam_64mp->exposure->minimum,
				 exposure_max, arducam_64mp->exposure->step,
				 exposure_def);
}

static int arducam_64mp_set_frame_length(struct arducam_64mp *arducam_64mp,
					 unsigned int vblank)
{
	unsigned int val = vblank + arducam_64mp->mode->height;
	int ret = 0;

	arducam_64mp->long_exp_shift = 0;

	while (val > ARDUCAM_64MP_FRAME_LENGTH_MAX) {
		arducam_64mp->long_exp_shift++;
		val >>= 1;
	}

	ret = arducam_64mp_write_reg(arducam_64mp,
				     ARDUCAM_64MP_REG_FRAME_LENGTH,
				     ARDUCAM_64MP_REG_VALUE_16BIT, val);
	if (ret)
		return ret;

	return arducam_64mp_write_reg(arducam_64mp,
				      ARDUCAM_64MP_LONG_EXP_SHIFT_REG,
				      ARDUCAM_64MP_REG_VALUE_08BIT,
				      arducam_64mp->long_exp_shift);
}

static int arducam_64mp_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct arducam_64mp *arducam_64mp =
		container_of(ctrl->handler, struct arducam_64mp, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&arducam_64mp->sd);
	int ret;
	u32 val;
	/*
	 * The VBLANK control may change the limits of usable exposure, so check
	 * and adjust if necessary.
	 */
	if (ctrl->id == V4L2_CID_VBLANK)
		arducam_64mp_adjust_exposure_range(arducam_64mp);

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming
	 */
	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = arducam_64mp_write_reg(arducam_64mp,
					     ARDUCAM_64MP_REG_ANALOG_GAIN,
					     ARDUCAM_64MP_REG_VALUE_16BIT,
					     ctrl->val);
		break;
	case V4L2_CID_EXPOSURE:
		val = ctrl->val >> arducam_64mp->long_exp_shift;
		ret = arducam_64mp_write_reg(arducam_64mp,
					     ARDUCAM_64MP_REG_EXPOSURE,
					     ARDUCAM_64MP_REG_VALUE_16BIT,
					     val);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = arducam_64mp_write_reg(arducam_64mp,
					     ARDUCAM_64MP_REG_DIGITAL_GAIN,
					     ARDUCAM_64MP_REG_VALUE_16BIT,
					     ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		val = arducam_64mp_test_pattern_val[ctrl->val];
		ret = arducam_64mp_write_reg(arducam_64mp,
					     ARDUCAM_64MP_REG_TEST_PATTERN,
					     ARDUCAM_64MP_REG_VALUE_16BIT,
					     val);
		break;
	case V4L2_CID_TEST_PATTERN_RED:
		ret = arducam_64mp_write_reg(arducam_64mp,
					     ARDUCAM_64MP_REG_TEST_PATTERN_R,
					     ARDUCAM_64MP_REG_VALUE_16BIT,
					     ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN_GREENR:
		ret = arducam_64mp_write_reg(arducam_64mp,
					     ARDUCAM_64MP_REG_TEST_PATTERN_GR,
					     ARDUCAM_64MP_REG_VALUE_16BIT,
					     ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN_BLUE:
		ret = arducam_64mp_write_reg(arducam_64mp,
					     ARDUCAM_64MP_REG_TEST_PATTERN_B,
					     ARDUCAM_64MP_REG_VALUE_16BIT,
					     ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN_GREENB:
		ret = arducam_64mp_write_reg(arducam_64mp,
					     ARDUCAM_64MP_REG_TEST_PATTERN_GB,
					     ARDUCAM_64MP_REG_VALUE_16BIT,
					     ctrl->val);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = arducam_64mp_write_reg(arducam_64mp,
					     ARDUCAM_64MP_REG_ORIENTATION, 1,
					     arducam_64mp->hflip->val |
						arducam_64mp->vflip->val << 1);
		break;
	case V4L2_CID_VBLANK:
		ret = arducam_64mp_set_frame_length(arducam_64mp, ctrl->val);
		break;
	default:
		dev_info(&client->dev,
			 "ctrl(id:0x%x,val:0x%x) is not handled\n",
			 ctrl->id, ctrl->val);
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops arducam_64mp_ctrl_ops = {
	.s_ctrl = arducam_64mp_set_ctrl,
};

static int arducam_64mp_enum_mbus_code(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *sd_state,
				       struct v4l2_subdev_mbus_code_enum *code)
{
	struct arducam_64mp *arducam_64mp = to_arducam_64mp(sd);

	if (code->pad >= NUM_PADS)
		return -EINVAL;

	if (code->pad == IMAGE_PAD) {
		if (code->index > 0)
			return -EINVAL;

		code->code = arducam_64mp_get_format_code(arducam_64mp);
	} else {
		if (code->index > 0)
			return -EINVAL;

		code->code = MEDIA_BUS_FMT_SENSOR_DATA;
	}

	return 0;
}

static int arducam_64mp_enum_frame_size(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *sd_state,
					struct v4l2_subdev_frame_size_enum *fse)
{
	struct arducam_64mp *arducam_64mp = to_arducam_64mp(sd);

	if (fse->pad >= NUM_PADS)
		return -EINVAL;

	if (fse->pad == IMAGE_PAD) {
		if (fse->index >= ARRAY_SIZE(supported_modes))
			return -EINVAL;

		if (fse->code != arducam_64mp_get_format_code(arducam_64mp))
			return -EINVAL;

		fse->min_width = supported_modes[fse->index].width;
		fse->max_width = fse->min_width;
		fse->min_height = supported_modes[fse->index].height;
		fse->max_height = fse->min_height;
	} else {
		if (fse->code != MEDIA_BUS_FMT_SENSOR_DATA || fse->index > 0)
			return -EINVAL;

		fse->min_width = ARDUCAM_64MP_EMBEDDED_LINE_WIDTH;
		fse->max_width = fse->min_width;
		fse->min_height = ARDUCAM_64MP_NUM_EMBEDDED_LINES;
		fse->max_height = fse->min_height;
	}

	return 0;
}

static void arducam_64mp_reset_colorspace(struct v4l2_mbus_framefmt *fmt)
{
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(true,
							  fmt->colorspace,
							  fmt->ycbcr_enc);
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
}

static void
arducam_64mp_update_image_pad_format(struct arducam_64mp *arducam_64mp,
				     const struct arducam_64mp_mode *mode,
				     struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	arducam_64mp_reset_colorspace(&fmt->format);
}

static void
arducam_64mp_update_metadata_pad_format(struct v4l2_subdev_format *fmt)
{
	fmt->format.width = ARDUCAM_64MP_EMBEDDED_LINE_WIDTH;
	fmt->format.height = ARDUCAM_64MP_NUM_EMBEDDED_LINES;
	fmt->format.code = MEDIA_BUS_FMT_SENSOR_DATA;
	fmt->format.field = V4L2_FIELD_NONE;
}

static int arducam_64mp_get_pad_format(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *sd_state,
				       struct v4l2_subdev_format *fmt)
{
	struct arducam_64mp *arducam_64mp = to_arducam_64mp(sd);

	if (fmt->pad >= NUM_PADS)
		return -EINVAL;

	mutex_lock(&arducam_64mp->mutex);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_fmt =
			v4l2_subdev_get_try_format(&arducam_64mp->sd, sd_state,
						   fmt->pad);
		/* update the code which could change due to vflip or hflip: */
		try_fmt->code = fmt->pad == IMAGE_PAD ?
				arducam_64mp_get_format_code(arducam_64mp) :
				MEDIA_BUS_FMT_SENSOR_DATA;
		fmt->format = *try_fmt;
	} else {
		if (fmt->pad == IMAGE_PAD) {
			arducam_64mp_update_image_pad_format(arducam_64mp,
							     arducam_64mp->mode,
							     fmt);
			fmt->format.code =
			       arducam_64mp_get_format_code(arducam_64mp);
		} else {
			arducam_64mp_update_metadata_pad_format(fmt);
		}
	}

	mutex_unlock(&arducam_64mp->mutex);
	return 0;
}

static unsigned int
arducam_64mp_get_frame_length(const struct arducam_64mp_mode *mode,
			      const struct v4l2_fract *timeperframe)
{
	u64 frame_length;

	frame_length = (u64)timeperframe->numerator * ARDUCAM_64MP_PIXEL_RATE;
	do_div(frame_length,
	       (u64)timeperframe->denominator * mode->line_length_pix);

	if (WARN_ON(frame_length > ARDUCAM_64MP_FRAME_LENGTH_MAX))
		frame_length = ARDUCAM_64MP_FRAME_LENGTH_MAX;

	return max_t(unsigned int, frame_length, mode->height);
}

static void arducam_64mp_set_framing_limits(struct arducam_64mp *arducam_64mp)
{
	unsigned int frm_length_min, frm_length_default, hblank;
	const struct arducam_64mp_mode *mode = arducam_64mp->mode;

	/* The default framerate is highest possible framerate. */
	frm_length_min =
		arducam_64mp_get_frame_length(mode,
					      &mode->timeperframe_default);
	frm_length_default =
		arducam_64mp_get_frame_length(mode,
					      &mode->timeperframe_default);

	/* Default to no long exposure multiplier. */
	arducam_64mp->long_exp_shift = 0;

	/* Update limits and set FPS to default */
	__v4l2_ctrl_modify_range(arducam_64mp->vblank,
				 frm_length_min - mode->height,
				 ((1 << ARDUCAM_64MP_LONG_EXP_SHIFT_MAX) *
				  ARDUCAM_64MP_FRAME_LENGTH_MAX) - mode->height,
				 1, frm_length_default - mode->height);

	/* Setting this will adjust the exposure limits as well. */
	__v4l2_ctrl_s_ctrl(arducam_64mp->vblank,
			   frm_length_default - mode->height);

	/*
	 * Currently PPL is fixed to the mode specified value, so hblank
	 * depends on mode->width only, and is not changeable in any
	 * way other than changing the mode.
	 */
	hblank = mode->line_length_pix - mode->width;
	__v4l2_ctrl_modify_range(arducam_64mp->hblank, hblank, hblank, 1,
				 hblank);
}

static int arducam_64mp_set_pad_format(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *sd_state,
				       struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt;
	const struct arducam_64mp_mode *mode;
	struct arducam_64mp *arducam_64mp = to_arducam_64mp(sd);

	if (fmt->pad >= NUM_PADS)
		return -EINVAL;

	mutex_lock(&arducam_64mp->mutex);

	if (fmt->pad == IMAGE_PAD) {
		/* Bayer order varies with flips */
		fmt->format.code = arducam_64mp_get_format_code(arducam_64mp);

		mode = v4l2_find_nearest_size(supported_modes,
					      ARRAY_SIZE(supported_modes),
					      width, height,
					      fmt->format.width,
					      fmt->format.height);
		arducam_64mp_update_image_pad_format(arducam_64mp, mode, fmt);
		if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
			framefmt = v4l2_subdev_get_try_format(sd, sd_state,
							      fmt->pad);
			*framefmt = fmt->format;
		} else {
			arducam_64mp->mode = mode;
			arducam_64mp->fmt_code = fmt->format.code;
			arducam_64mp_set_framing_limits(arducam_64mp);
		}
	} else {
		if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
			framefmt = v4l2_subdev_get_try_format(sd, sd_state,
							      fmt->pad);
			*framefmt = fmt->format;
		} else {
			/* Only one embedded data mode is supported */
			arducam_64mp_update_metadata_pad_format(fmt);
		}
	}

	mutex_unlock(&arducam_64mp->mutex);

	return 0;
}

static const struct v4l2_rect *
__arducam_64mp_get_pad_crop(struct arducam_64mp *arducam_64mp,
			    struct v4l2_subdev_state *sd_state,
			    unsigned int pad,
			    enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_crop(&arducam_64mp->sd, sd_state,
						pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &arducam_64mp->mode->crop;
	}

	return NULL;
}

static int arducam_64mp_get_selection(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *sd_state,
				      struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP: {
		struct arducam_64mp *arducam_64mp = to_arducam_64mp(sd);

		mutex_lock(&arducam_64mp->mutex);
		sel->r = *__arducam_64mp_get_pad_crop(arducam_64mp, sd_state,
						      sel->pad, sel->which);
		mutex_unlock(&arducam_64mp->mutex);

		return 0;
	}

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = ARDUCAM_64MP_NATIVE_WIDTH;
		sel->r.height = ARDUCAM_64MP_NATIVE_HEIGHT;

		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = ARDUCAM_64MP_PIXEL_ARRAY_LEFT;
		sel->r.top = ARDUCAM_64MP_PIXEL_ARRAY_TOP;
		sel->r.width = ARDUCAM_64MP_PIXEL_ARRAY_WIDTH;
		sel->r.height = ARDUCAM_64MP_PIXEL_ARRAY_HEIGHT;

		return 0;
	}

	return -EINVAL;
}

/* Start streaming */
static int arducam_64mp_start_streaming(struct arducam_64mp *arducam_64mp)
{
	struct i2c_client *client = v4l2_get_subdevdata(&arducam_64mp->sd);
	const struct arducam_64mp_reg_list *reg_list;
	int ret;

	if (!arducam_64mp->common_regs_written) {
		ret = arducam_64mp_write_regs(arducam_64mp, mode_common_regs,
					      ARRAY_SIZE(mode_common_regs));

		if (ret) {
			dev_err(&client->dev, "%s failed to set common settings\n",
				__func__);
			return ret;
		}
		arducam_64mp->common_regs_written = true;
	}

	/* Apply default values of current mode */
	reg_list = &arducam_64mp->mode->reg_list;
	ret = arducam_64mp_write_regs(arducam_64mp, reg_list->regs,
				      reg_list->num_of_regs);
	if (ret) {
		dev_err(&client->dev, "%s failed to set mode\n", __func__);
		return ret;
	}

	/* Apply customized values from user */
	ret =  __v4l2_ctrl_handler_setup(arducam_64mp->sd.ctrl_handler);
	if (ret)
		return ret;

	/* set stream on register */
	return arducam_64mp_write_reg(arducam_64mp,
				      ARDUCAM_64MP_REG_MODE_SELECT,
				      ARDUCAM_64MP_REG_VALUE_08BIT,
				      ARDUCAM_64MP_MODE_STREAMING);
}

/* Stop streaming */
static void arducam_64mp_stop_streaming(struct arducam_64mp *arducam_64mp)
{
	struct i2c_client *client = v4l2_get_subdevdata(&arducam_64mp->sd);
	int ret;

	/* set stream off register */
	ret = arducam_64mp_write_reg(arducam_64mp, ARDUCAM_64MP_REG_MODE_SELECT,
				     ARDUCAM_64MP_REG_VALUE_08BIT,
				     ARDUCAM_64MP_MODE_STANDBY);
	if (ret)
		dev_err(&client->dev, "%s failed to set stream\n", __func__);
}

static int arducam_64mp_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct arducam_64mp *arducam_64mp = to_arducam_64mp(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&arducam_64mp->mutex);
	if (arducam_64mp->streaming == enable) {
		mutex_unlock(&arducam_64mp->mutex);
		return 0;
	}

	if (enable) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto err_unlock;
		}

		/*
		 * Apply default & customized values
		 * and then start streaming.
		 */
		ret = arducam_64mp_start_streaming(arducam_64mp);
		if (ret)
			goto err_rpm_put;
	} else {
		arducam_64mp_stop_streaming(arducam_64mp);
		pm_runtime_put(&client->dev);
	}

	arducam_64mp->streaming = enable;

	/* vflip and hflip cannot change during streaming */
	__v4l2_ctrl_grab(arducam_64mp->vflip, enable);
	__v4l2_ctrl_grab(arducam_64mp->hflip, enable);

	mutex_unlock(&arducam_64mp->mutex);

	return ret;

err_rpm_put:
	pm_runtime_put(&client->dev);
err_unlock:
	mutex_unlock(&arducam_64mp->mutex);

	return ret;
}

/* Power/clock management functions */
static int arducam_64mp_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct arducam_64mp *arducam_64mp = to_arducam_64mp(sd);
	int ret;

	ret = regulator_bulk_enable(ARDUCAM_64MP_NUM_SUPPLIES,
				    arducam_64mp->supplies);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable regulators\n",
			__func__);
		return ret;
	}

	ret = clk_prepare_enable(arducam_64mp->xclk);
	if (ret) {
		dev_err(&client->dev, "%s: failed to enable clock\n",
			__func__);
		goto reg_off;
	}

	gpiod_set_value_cansleep(arducam_64mp->reset_gpio, 1);
	usleep_range(ARDUCAM_64MP_XCLR_MIN_DELAY_US,
		     ARDUCAM_64MP_XCLR_MIN_DELAY_US +
					ARDUCAM_64MP_XCLR_DELAY_RANGE_US);

	return 0;

reg_off:
	regulator_bulk_disable(ARDUCAM_64MP_NUM_SUPPLIES,
			       arducam_64mp->supplies);
	return ret;
}

static int arducam_64mp_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct arducam_64mp *arducam_64mp = to_arducam_64mp(sd);

	gpiod_set_value_cansleep(arducam_64mp->reset_gpio, 0);
	regulator_bulk_disable(ARDUCAM_64MP_NUM_SUPPLIES,
			       arducam_64mp->supplies);
	clk_disable_unprepare(arducam_64mp->xclk);

	/* Force reprogramming of the common registers when powered up again. */
	arducam_64mp->common_regs_written = false;

	return 0;
}

static int __maybe_unused arducam_64mp_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct arducam_64mp *arducam_64mp = to_arducam_64mp(sd);

	if (arducam_64mp->streaming)
		arducam_64mp_stop_streaming(arducam_64mp);

	return 0;
}

static int __maybe_unused arducam_64mp_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct arducam_64mp *arducam_64mp = to_arducam_64mp(sd);
	int ret;

	if (arducam_64mp->streaming) {
		ret = arducam_64mp_start_streaming(arducam_64mp);
		if (ret)
			goto error;
	}

	return 0;

error:
	arducam_64mp_stop_streaming(arducam_64mp);
	arducam_64mp->streaming = 0;
	return ret;
}

static int arducam_64mp_get_regulators(struct arducam_64mp *arducam_64mp)
{
	struct i2c_client *client = v4l2_get_subdevdata(&arducam_64mp->sd);
	unsigned int i;

	for (i = 0; i < ARDUCAM_64MP_NUM_SUPPLIES; i++)
		arducam_64mp->supplies[i].supply = arducam_64mp_supply_name[i];

	return devm_regulator_bulk_get(&client->dev,
				       ARDUCAM_64MP_NUM_SUPPLIES,
				       arducam_64mp->supplies);
}

/* Verify chip ID */
static int arducam_64mp_identify_module(struct arducam_64mp *arducam_64mp)
{
	struct i2c_client *client = v4l2_get_subdevdata(&arducam_64mp->sd);
	struct i2c_client *arducam_identifier;
	int ret;
	u32 val;

	arducam_identifier = i2c_new_dummy_device(client->adapter, 0x50);
	if (IS_ERR(arducam_identifier)) {
		dev_err(&client->dev, "failed to create arducam_identifier\n");
		return PTR_ERR(arducam_identifier);
	}

	ret = arducam_64mp_read_reg(arducam_identifier,
				    ARDUCAM_64MP_REG_CHIP_ID,
				    ARDUCAM_64MP_REG_VALUE_16BIT, &val);
	if (ret) {
		dev_err(&client->dev, "failed to read chip id %x, with error %d\n",
			ARDUCAM_64MP_CHIP_ID, ret);
		goto error;
	}

	if (val != ARDUCAM_64MP_CHIP_ID) {
		dev_err(&client->dev, "chip id mismatch: %x!=%x\n",
			ARDUCAM_64MP_CHIP_ID, val);
		ret = -EIO;
		goto error;
	}

	dev_info(&client->dev, "Device found Arducam 64MP.\n");

error:
	i2c_unregister_device(arducam_identifier);

	return ret;
}

static const struct v4l2_subdev_core_ops arducam_64mp_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops arducam_64mp_video_ops = {
	.s_stream = arducam_64mp_set_stream,
};

static const struct v4l2_subdev_pad_ops arducam_64mp_pad_ops = {
	.enum_mbus_code = arducam_64mp_enum_mbus_code,
	.get_fmt = arducam_64mp_get_pad_format,
	.set_fmt = arducam_64mp_set_pad_format,
	.get_selection = arducam_64mp_get_selection,
	.enum_frame_size = arducam_64mp_enum_frame_size,
};

static const struct v4l2_subdev_ops arducam_64mp_subdev_ops = {
	.core = &arducam_64mp_core_ops,
	.video = &arducam_64mp_video_ops,
	.pad = &arducam_64mp_pad_ops,
};

static const struct v4l2_subdev_internal_ops arducam_64mp_internal_ops = {
	.open = arducam_64mp_open,
};

/* Initialize control handlers */
static int arducam_64mp_init_controls(struct arducam_64mp *arducam_64mp)
{
	struct v4l2_ctrl_handler *ctrl_hdlr;
	struct i2c_client *client = v4l2_get_subdevdata(&arducam_64mp->sd);
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *link_freq;
	unsigned int i;
	int ret;
	u8 test_pattern_max;
	u8 link_freq_max;

	ctrl_hdlr = &arducam_64mp->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 16);
	if (ret)
		return ret;

	mutex_init(&arducam_64mp->mutex);
	ctrl_hdlr->lock = &arducam_64mp->mutex;

	/* By default, PIXEL_RATE is read only */
	arducam_64mp->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr,
						     &arducam_64mp_ctrl_ops,
						     V4L2_CID_PIXEL_RATE,
						     ARDUCAM_64MP_PIXEL_RATE,
						     ARDUCAM_64MP_PIXEL_RATE, 1,
						     ARDUCAM_64MP_PIXEL_RATE);

	/* LINK_FREQ is also read only */
	link_freq_max = ARRAY_SIZE(arducam_64mp_link_freq_menu) - 1;
	link_freq =
		v4l2_ctrl_new_int_menu(ctrl_hdlr, &arducam_64mp_ctrl_ops,
				       V4L2_CID_LINK_FREQ,
				       link_freq_max, 0,
				       arducam_64mp_link_freq_menu);
	if (link_freq)
		link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/*
	 * Create the controls here, but mode specific limits are setup
	 * in the arducam_64mp_set_framing_limits() call below.
	 */
	arducam_64mp->vblank = v4l2_ctrl_new_std(ctrl_hdlr,
						 &arducam_64mp_ctrl_ops,
						 V4L2_CID_VBLANK,
						 0, 0xffff, 1, 0);
	arducam_64mp->hblank = v4l2_ctrl_new_std(ctrl_hdlr,
						 &arducam_64mp_ctrl_ops,
						 V4L2_CID_HBLANK,
						 0, 0xffff, 1, 0);

	/* HBLANK is read-only, but does change with mode. */
	if (arducam_64mp->hblank)
		arducam_64mp->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	arducam_64mp->exposure =
		v4l2_ctrl_new_std(ctrl_hdlr,
				  &arducam_64mp_ctrl_ops,
				  V4L2_CID_EXPOSURE,
				  ARDUCAM_64MP_EXPOSURE_MIN,
				  ARDUCAM_64MP_EXPOSURE_MAX,
				  ARDUCAM_64MP_EXPOSURE_STEP,
				  ARDUCAM_64MP_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &arducam_64mp_ctrl_ops,
			  V4L2_CID_ANALOGUE_GAIN, ARDUCAM_64MP_ANA_GAIN_MIN,
			  ARDUCAM_64MP_ANA_GAIN_MAX, ARDUCAM_64MP_ANA_GAIN_STEP,
			  ARDUCAM_64MP_ANA_GAIN_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &arducam_64mp_ctrl_ops,
			  V4L2_CID_DIGITAL_GAIN, ARDUCAM_64MP_DGTL_GAIN_MIN,
			  ARDUCAM_64MP_DGTL_GAIN_MAX,
			  ARDUCAM_64MP_DGTL_GAIN_STEP,
			  ARDUCAM_64MP_DGTL_GAIN_DEFAULT);

	arducam_64mp->hflip = v4l2_ctrl_new_std(ctrl_hdlr,
						&arducam_64mp_ctrl_ops,
						V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (arducam_64mp->hflip)
		arducam_64mp->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	arducam_64mp->vflip = v4l2_ctrl_new_std(ctrl_hdlr,
						&arducam_64mp_ctrl_ops,
						V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (arducam_64mp->vflip)
		arducam_64mp->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	test_pattern_max = ARRAY_SIZE(arducam_64mp_test_pattern_menu) - 1;
	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &arducam_64mp_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     test_pattern_max,
				     0, 0, arducam_64mp_test_pattern_menu);
	for (i = 0; i < 4; i++) {
		/*
		 * The assumption is that
		 * V4L2_CID_TEST_PATTERN_GREENR == V4L2_CID_TEST_PATTERN_RED + 1
		 * V4L2_CID_TEST_PATTERN_BLUE   == V4L2_CID_TEST_PATTERN_RED + 2
		 * V4L2_CID_TEST_PATTERN_GREENB == V4L2_CID_TEST_PATTERN_RED + 3
		 */
		v4l2_ctrl_new_std(ctrl_hdlr, &arducam_64mp_ctrl_ops,
				  V4L2_CID_TEST_PATTERN_RED + i,
				  ARDUCAM_64MP_TEST_PATTERN_COLOUR_MIN,
				  ARDUCAM_64MP_TEST_PATTERN_COLOUR_MAX,
				  ARDUCAM_64MP_TEST_PATTERN_COLOUR_STEP,
				  ARDUCAM_64MP_TEST_PATTERN_COLOUR_MAX);
		/* The "Solid color" pattern is white by default */
	}

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s control init failed (%d)\n",
			__func__, ret);
		goto error;
	}

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &arducam_64mp_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	arducam_64mp->sd.ctrl_handler = ctrl_hdlr;

	/* Setup exposure and frame/line length limits. */
	arducam_64mp_set_framing_limits(arducam_64mp);

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	mutex_destroy(&arducam_64mp->mutex);

	return ret;
}

static void arducam_64mp_free_controls(struct arducam_64mp *arducam_64mp)
{
	v4l2_ctrl_handler_free(arducam_64mp->sd.ctrl_handler);
	mutex_destroy(&arducam_64mp->mutex);
}

static int arducam_64mp_check_hwcfg(struct device *dev)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret = -EINVAL;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	if (v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep_cfg)) {
		dev_err(dev, "could not parse endpoint\n");
		goto error_out;
	}

	/* Check the number of MIPI CSI2 data lanes */
	if (ep_cfg.bus.mipi_csi2.num_data_lanes != 2) {
		dev_err(dev, "only 2 data lanes are currently supported\n");
		goto error_out;
	}

	/* Check the link frequency set in device tree */
	if (!ep_cfg.nr_of_link_frequencies) {
		dev_err(dev, "link-frequency property not found in DT\n");
		goto error_out;
	}

	if (ep_cfg.nr_of_link_frequencies != 1 ||
	    ep_cfg.link_frequencies[0] != ARDUCAM_64MP_DEFAULT_LINK_FREQ) {
		dev_err(dev, "Link frequency not supported: %lld\n",
			ep_cfg.link_frequencies[0]);
		goto error_out;
	}

	ret = 0;

error_out:
	v4l2_fwnode_endpoint_free(&ep_cfg);
	fwnode_handle_put(endpoint);

	return ret;
}

static const struct of_device_id arducam_64mp_dt_ids[] = {
	{ .compatible = "arducam,64mp"},
	{ /* sentinel */ }
};

static int arducam_64mp_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct arducam_64mp *arducam_64mp;
	const struct of_device_id *match;
	u32 xclk_freq;
	int ret;

	arducam_64mp = devm_kzalloc(&client->dev, sizeof(*arducam_64mp),
				    GFP_KERNEL);
	if (!arducam_64mp)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&arducam_64mp->sd, client,
			     &arducam_64mp_subdev_ops);

	match = of_match_device(arducam_64mp_dt_ids, dev);
	if (!match)
		return -ENODEV;

	/* Check the hardware configuration in device tree */
	if (arducam_64mp_check_hwcfg(dev))
		return -EINVAL;

	/* Get system clock (xclk) */
	arducam_64mp->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(arducam_64mp->xclk)) {
		dev_err(dev, "failed to get xclk\n");
		return PTR_ERR(arducam_64mp->xclk);
	}

	xclk_freq = clk_get_rate(arducam_64mp->xclk);
	if (xclk_freq != ARDUCAM_64MP_XCLK_FREQ) {
		dev_err(dev, "xclk frequency not supported: %d Hz\n",
			xclk_freq);
		return -EINVAL;
	}

	ret = arducam_64mp_get_regulators(arducam_64mp);
	if (ret) {
		dev_err(dev, "failed to get regulators\n");
		return ret;
	}

	/* Request optional enable pin */
	arducam_64mp->reset_gpio = devm_gpiod_get_optional(dev, "reset",
							   GPIOD_OUT_HIGH);

	/*
	 * The sensor must be powered for arducam_64mp_identify_module()
	 * to be able to read the CHIP_ID from arducam_identifier.
	 */
	ret = arducam_64mp_power_on(dev);
	if (ret)
		return ret;

	ret = arducam_64mp_identify_module(arducam_64mp);
	if (ret)
		goto error_power_off;

	/* Set default mode to max resolution */
	arducam_64mp->mode = &supported_modes[0];
	arducam_64mp->fmt_code = MEDIA_BUS_FMT_SRGGB10_1X10;

	/* Enable runtime PM and turn off the device */
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	/* This needs the pm runtime to be registered. */
	ret = arducam_64mp_init_controls(arducam_64mp);
	if (ret)
		goto error_power_off;

	/* Initialize subdev */
	arducam_64mp->sd.internal_ops = &arducam_64mp_internal_ops;
	arducam_64mp->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	arducam_64mp->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pads */
	arducam_64mp->pad[IMAGE_PAD].flags = MEDIA_PAD_FL_SOURCE;
	arducam_64mp->pad[METADATA_PAD].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&arducam_64mp->sd.entity, NUM_PADS,
				     arducam_64mp->pad);
	if (ret) {
		dev_err(dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	ret = v4l2_async_register_subdev_sensor(&arducam_64mp->sd);
	if (ret < 0) {
		dev_err(dev, "failed to register sensor sub-device: %d\n", ret);
		goto error_media_entity;
	}

	return 0;

error_media_entity:
	media_entity_cleanup(&arducam_64mp->sd.entity);

error_handler_free:
	arducam_64mp_free_controls(arducam_64mp);

error_power_off:
	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	arducam_64mp_power_off(&client->dev);

	return ret;
}

static void arducam_64mp_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct arducam_64mp *arducam_64mp = to_arducam_64mp(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	arducam_64mp_free_controls(arducam_64mp);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		arducam_64mp_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

MODULE_DEVICE_TABLE(of, arducam_64mp_dt_ids);

static const struct dev_pm_ops arducam_64mp_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(arducam_64mp_suspend, arducam_64mp_resume)
	SET_RUNTIME_PM_OPS(arducam_64mp_power_off, arducam_64mp_power_on, NULL)
};

static struct i2c_driver arducam_64mp_i2c_driver = {
	.driver = {
		.name = "arducam_64mp",
		.of_match_table	= arducam_64mp_dt_ids,
		.pm = &arducam_64mp_pm_ops,
	},
	.probe_new = arducam_64mp_probe,
	.remove = arducam_64mp_remove,
};

module_i2c_driver(arducam_64mp_i2c_driver);

MODULE_AUTHOR("Lee Jackson <info@arducam.com>");
MODULE_DESCRIPTION("Arducam 64MP sensor driver");
MODULE_LICENSE("GPL v2");
