// SPDX-License-Identifier: GPL-2.0-only
/*
 * Himax HX83121A DriverIC panels driver
 * Copyright (c) 2024-2026 Pengyu Luo <mitltlatltl@gmail.com>
 *
 * Multiple panels handling based on panel-novatek-nt36523.c
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

static bool enable_dsc;
module_param(enable_dsc, bool, 0);
MODULE_PARM_DESC(enable_dsc, "enable DSC on the panel (default: false)");

struct himax {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi[2];
	const struct panel_desc *desc;
	struct drm_dsc_config dsc;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data *supplies;
	struct regulator *bl_supply;
	struct backlight_device *backlight;
	bool backlight_enabled;
};

struct panel_desc {
	unsigned int width_mm;
	unsigned int height_mm;
	unsigned int bpc;
	unsigned int lanes;
	enum mipi_dsi_pixel_format format;
	unsigned long mode_flags;
	const struct drm_dsc_config *dsc_cfg;
	const struct drm_display_mode *dsc_modes;
	unsigned int num_dsc_modes;

	const struct drm_display_mode *modes;
	unsigned int num_modes;

	int (*init_sequence_dsc)(struct mipi_dsi_multi_context *dsi_ctx);
	int (*init_sequence)(struct mipi_dsi_multi_context *dsi_ctx);

	bool is_dual_dsi;
	bool has_dcs_backlight;
	bool dsc_required;
	unsigned int reset_assert_us;
	unsigned int reset_deassert_ms;
};

static const struct regulator_bulk_data himax_supplies[] = {
	{ .supply = "vddi" },
	{ .supply = "avdd" },
	{ .supply = "avee" },
};

static inline struct himax *to_himax(struct drm_panel *panel)
{
	return container_of(panel, struct himax, panel);
}

static inline struct mipi_dsi_device *to_primary_dsi(struct himax *ctx)
{
	/* Sync on DSI1 for dual dsi */
	return ctx->desc->is_dual_dsi ? ctx->dsi[1] : ctx->dsi[0];
}

static void himax_reset(struct himax *ctx)
{
	unsigned int assert_us = ctx->desc->reset_assert_us ?: 4000;
	unsigned int deassert_ms = ctx->desc->reset_deassert_ms ?: 20;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(assert_us, assert_us + 100);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(deassert_ms);
}

static int himax_prepare(struct drm_panel *panel)
{
	struct himax *ctx = to_himax(panel);
	struct mipi_dsi_device *dsi = to_primary_dsi(ctx);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(himax_supplies),
				    ctx->supplies);
	if (ret < 0)
		return ret;

	himax_reset(ctx);

	if (enable_dsc || ctx->desc->dsc_required) {
		if (ctx->desc->init_sequence_dsc)
			ret = ctx->desc->init_sequence_dsc(&dsi_ctx);
		else
			ret = ctx->desc->init_sequence(&dsi_ctx);
	} else if (ctx->desc->init_sequence) {
		ret = ctx->desc->init_sequence(&dsi_ctx);
	} else {
		ret = -EOPNOTSUPP;
	}

	if (ret < 0) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(himax_supplies),
				       ctx->supplies);
		return ret;
	}

	if (enable_dsc || ctx->desc->dsc_required) {
		drm_dsc_pps_payload_pack(&pps, &ctx->dsc);
		mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);
		mipi_dsi_compression_mode_multi(&dsi_ctx, true);
	}

	return backlight_enable(ctx->backlight);
}

static int himax_off(struct mipi_dsi_multi_context *dsi_ctx)
{
	mipi_dsi_dcs_enter_sleep_mode_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 120);

	return dsi_ctx->accum_err;
}

static int himax_unprepare(struct drm_panel *panel)
{
	struct himax *ctx = to_himax(panel);
	struct mipi_dsi_device *dsi = to_primary_dsi(ctx);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	struct device *dev = &dsi->dev;
	int ret;

	ret = himax_off(&dsi_ctx);
	if (ret < 0)
		dev_err(dev, "panel failed to off: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(himax_supplies), ctx->supplies);

	return 0;
}

static int himax_get_modes(struct drm_panel *panel,
			   struct drm_connector *connector)
{
	struct himax *ctx = to_himax(panel);
	const struct panel_desc *desc = ctx->desc;
	const struct drm_display_mode *modes;
	int num_modes;
	int i;

	modes = enable_dsc ? desc->dsc_modes : desc->modes;
	num_modes = enable_dsc ? desc->num_dsc_modes : desc->num_modes;

	for (i = 0; i < num_modes; i++) {
		const struct drm_display_mode *m = &modes[i];
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(connector->dev, m);
		if (!mode) {
			dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
				m->hdisplay, m->vdisplay, drm_mode_vrefresh(m));
			return -ENOMEM;
		}

		mode->type = DRM_MODE_TYPE_DRIVER;
		if (i == 0)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = desc->width_mm;
	connector->display_info.height_mm = desc->height_mm;
	connector->display_info.bpc = desc->bpc;

	return num_modes;
}

static const struct drm_panel_funcs himax_panel_funcs = {
	.prepare = himax_prepare,
	.unprepare = himax_unprepare,
	.get_modes = himax_get_modes,
};

static int himax_bl_update_status(struct backlight_device *bl)
{
	struct himax *ctx = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret = 0;

	if (!brightness) {
		if (ctx->backlight_enabled)
			ret = regulator_disable(ctx->bl_supply);
		if (ret)
			return ret;

		ctx->backlight_enabled = false;

		return 0;
	}

	/* TODO: brightness to raw map table */
	if (!ctx->backlight_enabled)
		ret = regulator_enable(ctx->bl_supply);
	if (ret)
		return ret;

	ctx->backlight_enabled = true;

	return mipi_dsi_dcs_set_display_brightness_large(to_primary_dsi(ctx),
							 brightness);
}

static const struct backlight_ops himax_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = himax_bl_update_status,
};

static struct backlight_device *
himax_create_backlight(struct himax *ctx)
{
	struct device *dev = &to_primary_dsi(ctx)->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 512,
		.max_brightness = 4095,
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, ctx,
					      &himax_bl_ops, &props);
}

static int boe_ppc357db1_4_dsc_init_seq(struct mipi_dsi_multi_context *dsi_ctx)
{
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb9, 0x83, 0x12, 0x1a, 0x55, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe2, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe1, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xc7);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2, 0x98);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe7,
				     0x01, 0x07, 0x01, 0x07, 0x01, 0x07, 0x06, 0x06,
				     0x06, 0x16, 0x00, 0x16, 0x81, 0x02, 0x40, 0x00,
				     0x1a, 0x4a, 0x05, 0x04, 0x03, 0x02, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xc6);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd2, 0x00, 0x30);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xc9);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd3, 0x04);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xc6);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe2, 0x42);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xd0);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2, 0xf5);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xcd,
				     0x81, 0x00, 0x80, 0x77, 0x00, 0x01, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe4,
				     0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1,
				     0xc7, 0xb2, 0xa0, 0x90, 0x81, 0x75, 0x69, 0x5f,
				     0x55, 0x4c, 0x44, 0x3d, 0x36, 0x2f, 0x2a, 0x24,
				     0x1e, 0x19, 0x14, 0x10, 0x09, 0x08, 0x07, 0x54,
				     0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe4,
				     0xaa, 0xd4, 0xff, 0x2a, 0x55, 0x7f, 0xaa, 0xd4,
				     0xff, 0xea, 0xff, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xc8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb1, 0x25);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbe, 0x01, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd9, 0x5f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb9, 0x00, 0x00, 0x00);

	mipi_dsi_dcs_exit_sleep_mode_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 140);
	mipi_dsi_dcs_set_display_on_multi(dsi_ctx);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);
	mipi_dsi_msleep(dsi_ctx, 20);

	return dsi_ctx->accum_err;
}

static int boe_ppc357db1_4_init_seq(struct mipi_dsi_multi_context *dsi_ctx)
{
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb9, 0x83, 0x12, 0x1a, 0x55, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd1, 0x37, 0x03, 0x0c, 0xfd);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe2, 0x20);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe1, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xc7);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2, 0xa6);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe7,
				     0x01, 0x07, 0x01, 0x07, 0x01, 0x07, 0x06, 0x06,
				     0x06, 0x16, 0x00, 0x16, 0x81, 0x02, 0x40, 0x00,
				     0x1a, 0x4a, 0x05, 0x04, 0x03, 0x02, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe2,
				     0x02, 0x68, 0x02, 0x68, 0x02, 0x68, 0x02, 0x68,
				     0x02, 0x6f, 0x03, 0x04, 0x2d, 0x09, 0x09, 0x00,
				     0x00, 0x0f, 0x0f, 0x0f, 0x0f, 0x00, 0x00, 0x00,
				     0x01, 0x10, 0x10, 0x1c, 0x25, 0x3c, 0x00, 0x23,
				     0x5d, 0x02, 0x02, 0x00, 0x00, 0x58, 0x01, 0xac,
				     0x0f, 0xa9, 0x10, 0x00, 0x2d, 0x6f, 0x00, 0x70,
				     0x00, 0x0a, 0xcb, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xc6);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd2, 0x09, 0x85);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xc9);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd3, 0x04);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xd0);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2, 0xf5);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe4,
				     0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1,
				     0xc7, 0xb2, 0xa0, 0x90, 0x81, 0x75, 0x69, 0x5f,
				     0x55, 0x4c, 0x44, 0x3d, 0x36, 0x2f, 0x2a, 0x24,
				     0x1e, 0x19, 0x14, 0x10, 0x09, 0x08, 0x07, 0x54,
				     0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe4,
				     0xaa, 0xd4, 0xff, 0x2a, 0x55, 0x7f, 0xaa, 0xd4,
				     0xff, 0xea, 0xff, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xc8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb1, 0x25);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbe, 0x01, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd9, 0x5f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb9, 0x00, 0x00, 0x00);

	mipi_dsi_dcs_exit_sleep_mode_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 140);
	mipi_dsi_dcs_set_display_on_multi(dsi_ctx);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);
	mipi_dsi_msleep(dsi_ctx, 31);

	return dsi_ctx->accum_err;
}

static int csot_ppc357db1_4_dsc_init_seq(struct mipi_dsi_multi_context *dsi_ctx)
{
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb9, 0x83, 0x12, 0x1a, 0x55, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb1,
				     0x1c, 0x6b, 0x6b, 0x27, 0xe7, 0x00, 0x1b, 0x25,
				     0x21, 0x21, 0x2d, 0x2d, 0x17, 0x33, 0x31, 0x40,
				     0xcd, 0xff, 0x1a, 0x05, 0x15, 0x98, 0x00, 0x88,
				     0x7f, 0xff, 0xff, 0xcf, 0x1a, 0xcc, 0x02, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd1, 0x37, 0x03, 0x0c, 0xfd);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2,
				     0x00, 0x6a, 0x40, 0x00, 0x00, 0x14, 0x98, 0x60,
				     0x3c, 0x02, 0x80, 0x21, 0x21, 0x00, 0x00, 0xf0,
				     0x27);
	/*
	 * NOTE: Register 0xE2 configuration (based on downstream reference):
	 * - 0x00: 120Hz with DSC enabled
	 * - 0x10: 60Hz with DSC enabled
	 * - 0x20: 60Hz with DSC disabled
	 *
	 * Both 0x00 and 0x10 are compatible with 60Hz/120Hz when DSC is active.
	 * We use a fixed DSC-on value to remain refresh-rate agnostic.
	 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe2, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc0, 0x23, 0x23, 0xcc, 0x22, 0x99, 0xd8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb4,
				     0x46, 0x06, 0x0c, 0xbe, 0x0c, 0xbe, 0x09, 0x46,
				     0x0f, 0x57, 0x0f, 0x57, 0x03, 0x4a, 0x00, 0x00,
				     0x04, 0x0c, 0x00, 0x18, 0x01, 0x06, 0x08, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0xff, 0x00, 0xff, 0x10, 0x00, 0x02,
				     0x14, 0x14, 0x14, 0x14);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe1, 0x01, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xe2);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe7, 0x49);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd3,
				     0x00, 0xc0, 0x08, 0x08, 0x08, 0x04, 0x04, 0x04,
				     0x16, 0x02, 0x07, 0x07, 0x07, 0x31, 0x13, 0x19,
				     0x12, 0x12, 0x03, 0x03, 0x03, 0x32, 0x10, 0x18,
				     0x00, 0x11, 0x32, 0x10, 0x03, 0x00, 0x03, 0x32,
				     0x10, 0x03, 0x00, 0x03, 0x00, 0x00, 0xff, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe1,
				     0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x0a, 0x00,
				     0x03, 0x20, 0x00, 0x14, 0x03, 0x20, 0x03, 0x20,
				     0x02, 0x00, 0x02, 0x91, 0x00, 0x20, 0x02, 0x47,
				     0x00, 0x0b, 0x00, 0x0c, 0x05, 0x0e, 0x03, 0x68,
				     0x18, 0x00, 0x10, 0xe0, 0x03, 0x0c, 0x20, 0x00,
				     0x06, 0x0b, 0x0b, 0x33, 0x0e, 0x1c, 0x2a, 0x38,
				     0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b,
				     0x7d, 0x7e, 0x01, 0x02, 0x01, 0x00, 0x09);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe7,
				     0x17, 0x08, 0x08, 0x2c, 0x46, 0x1e, 0x02, 0x23,
				     0x5d, 0x02, 0xc9, 0x00, 0x00, 0x00, 0x00, 0x12,
				     0x05, 0x02, 0x02, 0x07, 0x10, 0x10, 0x00, 0x1d,
				     0xb9, 0x23, 0xb9, 0x00, 0x33, 0x02, 0x88);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe7,
				     0x02, 0x00, 0xb2, 0x01, 0x56, 0x07, 0x56, 0x08,
				     0x48, 0x14, 0xfd, 0x26);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe7,
				     0x08, 0x08, 0x01, 0x03, 0x01, 0x03, 0x07, 0x02,
				     0x02, 0x47, 0x00, 0x47, 0x81, 0x02, 0x40, 0x00,
				     0x18, 0x4a, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
				     0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00,
				     0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbf,
				     0xfd, 0x00, 0x80, 0x9c, 0x36, 0x00, 0x81, 0x0c);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xcd,
				     0x81, 0x00, 0x80, 0x77, 0x00, 0x01, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe4,
				     0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1,
				     0xc7, 0xb2, 0xa0, 0x90, 0x81, 0x75, 0x69, 0x5f,
				     0x55, 0x4c, 0x44, 0x3d, 0x36, 0x2f, 0x2a, 0x24,
				     0x1e, 0x19, 0x14, 0x10, 0x09, 0x08, 0x07, 0x54,
				     0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe4,
				     0xaa, 0xd4, 0xff, 0x2a, 0x55, 0x7f, 0xaa, 0xd4,
				     0xff, 0xea, 0xff, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbe, 0x01, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd9, 0x5f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb9, 0x00, 0x00, 0x00);

	mipi_dsi_dcs_exit_sleep_mode_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 140);
	mipi_dsi_dcs_set_display_on_multi(dsi_ctx);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);
	mipi_dsi_msleep(dsi_ctx, 20);

	return dsi_ctx->accum_err;
}

static int csot_ppc357db1_4_init_seq(struct mipi_dsi_multi_context *dsi_ctx)
{
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb9, 0x83, 0x12, 0x1a, 0x55, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb1,
				     0x1c, 0x6b, 0x6b, 0x27, 0xe7, 0x00, 0x1b, 0x11,
				     0x21, 0x21, 0x2d, 0x2d, 0x17, 0x33, 0x31, 0x40,
				     0xcd, 0xff, 0x1a, 0x05, 0x15, 0x98, 0x00, 0x88,
				     0x7f, 0xff, 0xff, 0xcf, 0x1a, 0xcc, 0x02, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd1, 0x37, 0x03, 0x0c, 0xfd);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe2, 0x20);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2,
				     0x00, 0x6a, 0x40, 0x00, 0x00, 0x14, 0x98, 0x60,
				     0x3c, 0x02, 0x80, 0x21, 0x21, 0x00, 0x00, 0x10,
				     0x27);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe1, 0x00, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xe2);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe7, 0x49);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd3,
				     0x00, 0xc0, 0x08, 0x08, 0x08, 0x04, 0x04, 0x04,
				     0x16, 0x02, 0x07, 0x07, 0x07, 0x31, 0x13, 0x16,
				     0x12, 0x12, 0x03, 0x03, 0x03, 0x32, 0x10, 0x15,
				     0x00, 0x11, 0x32, 0x10, 0x03, 0x00, 0x03, 0x32,
				     0x10, 0x03, 0x00, 0x03, 0x00, 0x00, 0xff, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe2,
				     0x80, 0x05, 0x1c, 0xbe, 0x09, 0x8d, 0x0f, 0x57,
				     0x03, 0x87, 0x06, 0x10, 0x32, 0x06, 0x15, 0x00,
				     0x00, 0x14, 0x14, 0x14, 0x14, 0x00, 0x00, 0x00,
				     0x01, 0x10, 0x10, 0x16, 0x28, 0x3c, 0x03, 0x23,
				     0x5d, 0x02, 0x02, 0x00, 0x00, 0x48, 0x01, 0xac,
				     0x0f, 0xab, 0x10, 0x00, 0x32, 0x87, 0x00, 0xa1,
				     0x00, 0x0a, 0xcb, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe7,
				     0x02, 0x00, 0xb2, 0x01, 0x56, 0x07, 0x56, 0x08,
				     0x48, 0x14, 0x00, 0x26);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe7,
				     0x05, 0x05, 0x01, 0x05, 0x01, 0x05, 0x04, 0x04,
				     0x04, 0x24, 0x00, 0x24, 0x81, 0x02, 0x40, 0x00,
				     0x32, 0x87, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xd0);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2, 0xf0);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbf,
				     0xfd, 0x00, 0x80, 0x9c, 0x10, 0x00, 0x81, 0x0c);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe4,
				     0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1,
				     0xc7, 0xb2, 0xa0, 0x90, 0x81, 0x75, 0x69, 0x5f,
				     0x55, 0x4c, 0x44, 0x3d, 0x36, 0x2f, 0x2a, 0x24,
				     0x1e, 0x19, 0x14, 0x10, 0x09, 0x08, 0x07, 0x54,
				     0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe4,
				     0xaa, 0xd4, 0xff, 0x2a, 0x55, 0x7f, 0xaa, 0xd4,
				     0xff, 0xea, 0xff, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xc8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb1, 0x25);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbe, 0x01, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd9, 0x5f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb9, 0x00, 0x00, 0x00);

	mipi_dsi_dcs_exit_sleep_mode_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 140);
	mipi_dsi_dcs_set_display_on_multi(dsi_ctx);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);
	mipi_dsi_msleep(dsi_ctx, 31);

	return dsi_ctx->accum_err;
}

static struct drm_dsc_config ppc357db1_4_dsc_cfg = {
	.dsc_version_major = 1,
	.dsc_version_minor = 1,
	.slice_height = 20,
	.slice_width = 800,
	.slice_count = 1,
	.bits_per_component = 8,
	.bits_per_pixel = 8 << 4,
	.block_pred_enable = true,
};

static const struct drm_display_mode ppc357db1_4_dsc_modes[] = {
	{
		.clock = (800 + 60 + 40 + 40) * 2 * (2560 + 154 + 4 + 18) * 120 / 1000,
		.hdisplay = 800 * 2,
		.hsync_start = (800 + 60) * 2,
		.hsync_end = (800 + 60 + 40) * 2,
		.htotal = (800 + 60 + 40 + 40) * 2,
		.vdisplay = 2560,
		.vsync_start = 2560 + 154,
		.vsync_end = 2560 + 154 + 4,
		.vtotal = 2560 + 154 + 4 + 18,
	},
	{
		.clock = (800 + 60 + 40 + 40) * 2 * (2560 + 2890 + 4 + 18) * 60 / 1000,
		.hdisplay = 800 * 2,
		.hsync_start = (800 + 60) * 2,
		.hsync_end = (800 + 60 + 40) * 2,
		.htotal = (800 + 60 + 40 + 40) * 2,
		.vdisplay = 2560,
		.vsync_start = 2560 + 2890,
		.vsync_end = 2560 + 2890 + 4,
		.vtotal = 2560 + 2890 + 4 + 18,
	},
};

static const struct drm_display_mode ppc357db1_4_modes[] = {
	{
		.clock = (800 + 60 + 20 + 40) * 2 * (2560 + 154 + 4 + 18) * 60 / 1000,
		.hdisplay = 800 * 2,
		.hsync_start = (800 + 60) * 2,
		.hsync_end = (800 + 60 + 20) * 2,
		.htotal = (800 + 60 + 20 + 40) * 2,
		.vdisplay = 2560,
		.vsync_start = 2560 + 168,
		.vsync_end = 2560 + 168 + 4,
		.vtotal = 2560 + 168 + 4 + 18,
	},
};

static int himax_probe(struct mipi_dsi_device *dsi)
{
	struct mipi_dsi_device_info dsi_info = {"dsi-secondary", 0, NULL};
	struct mipi_dsi_host *dsi1_host;
	struct device *dev = &dsi->dev;
	const struct panel_desc *desc;
	struct device_node *dsi1;
	struct himax *ctx;
	int num_dsi = 1;
	int ret, i;

	ctx = devm_drm_panel_alloc(dev, struct himax, panel, &himax_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(&dsi->dev,
					    ARRAY_SIZE(himax_supplies),
					    himax_supplies, &ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	desc = of_device_get_match_data(dev);
	if (!desc)
		return -ENODEV;
	ctx->desc = desc;
	if (desc->dsc_cfg)
		ctx->dsc = *desc->dsc_cfg;

	if (desc->is_dual_dsi) {
		num_dsi = 2;
		dsi1 = of_graph_get_remote_node(dsi->dev.of_node, 1, -1);
		if (!dsi1) {
			dev_err(dev, "cannot get secondary DSI node.\n");
			return -ENODEV;
		}

		dsi1_host = of_find_mipi_dsi_host_by_node(dsi1);
		of_node_put(dsi1);
		if (!dsi1_host)
			return dev_err_probe(dev, -EPROBE_DEFER,
					     "cannot get secondary DSI host\n");

		ctx->dsi[1] = devm_mipi_dsi_device_register_full(dev, dsi1_host,
								 &dsi_info);
		if (IS_ERR(ctx->dsi[1])) {
			dev_err(dev, "cannot get secondary DSI device\n");
			return PTR_ERR(ctx->dsi[1]);
		}

		mipi_dsi_set_drvdata(ctx->dsi[1], ctx);
	}

	ctx->dsi[0] = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->panel.prepare_prev_first = true;

	if (desc->has_dcs_backlight) {
		ctx->bl_supply = devm_regulator_get_optional(dev, "bl");
		if (IS_ERR(ctx->bl_supply))
			return dev_err_probe(dev, PTR_ERR(ctx->bl_supply),
					     "Failed to get backlight supply\n");
		ctx->backlight = himax_create_backlight(ctx);
		if (IS_ERR(ctx->backlight))
			return dev_err_probe(dev, PTR_ERR(ctx->backlight),
					     "Failed to create backlight\n");
	} else {
		ret = drm_panel_of_backlight(&ctx->panel);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to get backlight\n");
	}

	drm_panel_add(&ctx->panel);

	for (i = 0; i < num_dsi; i++) {
		ctx->dsi[i]->lanes = desc->lanes;
		ctx->dsi[i]->format = desc->format;
		ctx->dsi[i]->mode_flags = desc->mode_flags;
		ctx->dsi[i]->dsc = (enable_dsc || desc->dsc_required) ?
				   &ctx->dsc : NULL;
		ret = devm_mipi_dsi_attach(dev, ctx->dsi[i]);
		if (ret < 0) {
			drm_panel_remove(&ctx->panel);
			return dev_err_probe(dev, ret,
					     "Failed to attach to DSI host\n");
		}
	}

	return 0;
}

static void himax_remove(struct mipi_dsi_device *dsi)
{
	struct himax *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

/* Model name: BOE PPC357DB1-4 */
static const struct panel_desc boe_ppc357db1_4_desc = {
	.width_mm = 266,
	.height_mm = 166,
	.lanes = 4,
	.format = MIPI_DSI_FMT_RGB888,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_CLOCK_NON_CONTINUOUS |
		      MIPI_DSI_MODE_LPM,
	.dsc_cfg = &ppc357db1_4_dsc_cfg,
	.dsc_modes = ppc357db1_4_dsc_modes,
	.num_dsc_modes = ARRAY_SIZE(ppc357db1_4_dsc_modes),
	.modes = ppc357db1_4_modes,
	.num_modes = ARRAY_SIZE(ppc357db1_4_modes),
	.init_sequence_dsc = boe_ppc357db1_4_dsc_init_seq,
	.init_sequence = boe_ppc357db1_4_init_seq,
	.is_dual_dsi = true,
	.has_dcs_backlight = true,
};

/* Model name: CSOT PPC357DB1-4 */
static const struct panel_desc csot_ppc357db1_4_desc = {
	.width_mm = 266,
	.height_mm = 166,
	.lanes = 4,
	.format = MIPI_DSI_FMT_RGB888,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_CLOCK_NON_CONTINUOUS |
		      MIPI_DSI_MODE_LPM,
	.dsc_cfg = &ppc357db1_4_dsc_cfg,
	.dsc_modes = ppc357db1_4_dsc_modes,
	.num_dsc_modes = ARRAY_SIZE(ppc357db1_4_dsc_modes),
	.modes = ppc357db1_4_modes,
	.num_modes = ARRAY_SIZE(ppc357db1_4_modes),
	.init_sequence_dsc = csot_ppc357db1_4_dsc_init_seq,
	.init_sequence = csot_ppc357db1_4_init_seq,
	.is_dual_dsi = true,
	.has_dcs_backlight = true,
};

/*
 * CSOT PNC357DB1 (variant used in the FydeTab Duo, RK3588S tablet).
 * Single DSI, 4 lanes, 1600x2560@60 portrait, DSC required.
 *
 * Truth source: the Fyde EDK2 firmware (edk2-rk3588, CsotDsiPanel.c), which
 * is proven on this hardware. The vendor 6.1 kernel shipped the same byte
 * stream through a hacked panel-simple; the EDK2 DSC setup decodes to a
 * valid 1600x2560 8bpp PPS with 1600x40 slices (verified byte-identical
 * against drm_dsc_pps_payload_pack output).
 *
 * The vendor DT's bogus-looking "PPS" record (type 0x0a) is exactly this
 * PPS; my init table below drops it because the driver sends it properly
 * (mipi_dsi_picture_parameter_set_multi) after the DCS init.
 *
 * WARNING: the mainline dw-mipi-dsi2 (bridge and rockchip glue) has no
 * DSC support yet, so until the host side gains it this panel will not
 * produce an image even though the panel-side config is complete.
 *
 * Backlight is a hardware PWM on this board, handled by pwm-backlight.
 */
static const struct drm_dsc_config fydetab_pnc357db1_dsc_cfg = {
	.dsc_version_major = 1,
	.dsc_version_minor = 1,
	.line_buf_depth = 9,
	.bits_per_component = 8,
	.convert_rgb = true,
	.block_pred_enable = true,
	.bits_per_pixel = 8 << 4,
	.pic_width = 1600,
	.pic_height = 2560,
	.slice_width = 1600,
	.slice_height = 40,
	.slice_chunk_size = 1600,
	.initial_xmit_delay = 512,
	.initial_dec_delay = 1057,
	.initial_scale_value = 32,
	.scale_increment_interval = 1488,
	.scale_decrement_interval = 22,
	.first_line_bpg_offset = 12,
	.nfl_bpg_offset = 631,
	.slice_bpg_offset = 218,
	.initial_offset = 6144,
	.final_offset = 4320,
	.flatness_min_qp = 3,
	.flatness_max_qp = 12,
	.rc_model_size = 8192,
	.rc_quant_incr_limit0 = 11,
	.rc_quant_incr_limit1 = 11,
	.rc_buf_thresh = {
		14, 28, 42, 56, 70, 84, 98, 105, 112, 119, 121, 123, 125, 126
	},
	.rc_range_params = {
		{  0,  4,   2 }, {  0,  4,   0 }, {  1,  5,   0 },
		{  1,  6,  62 }, {  3,  7,  60 }, {  3,  7,  58 },
		{  3,  7,  56 }, {  3,  8,  56 }, {  3,  9,  56 },
		{  3, 10,  54 }, {  5, 11,  54 }, {  5, 12,  52 },
		{  5, 13,  52 }, {  7, 13,  52 }, { 13, 13,  52 },
	},
};
static int fydetab_pnc357db1_init_seq(struct mipi_dsi_multi_context *dsi_ctx)
{
	/* page 0 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb9, 0x83, 0x12, 0x1a, 0x55,
				     0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x51, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x53, 0x24);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb1,
				     0x1c, 0x6b, 0x6b, 0x27, 0xe7, 0x00, 0x1b,
				     0x12, 0x20, 0x20, 0x2d, 0x2d, 0x1f, 0x33,
				     0x31, 0x40, 0xcd, 0xff, 0x1a, 0x05, 0x15,
				     0x98, 0x00, 0x88, 0xf9, 0xff, 0xff, 0xcf);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2,
				     0x00, 0x6a, 0x40, 0x00, 0x00, 0x14, 0x6e,
				     0x40, 0x73, 0x02, 0x80, 0x21, 0x21, 0x00,
				     0x00, 0x10, 0x27);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb4,
				     0x64, 0x00, 0x08, 0x7f, 0x08, 0x7f, 0x00,
				     0x62, 0x01, 0x72, 0x01, 0x72, 0x00, 0x60,
				     0x00, 0x00, 0x0a, 0x08, 0x00, 0x29, 0x05,
				     0x05, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
				     0x00, 0xff, 0x14, 0x00, 0x00, 0x0f, 0x0f,
				     0x2d, 0x2d);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb6, 0x8f, 0x8f, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbc, 0x06, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc0, 0x34, 0x34, 0x44, 0x00,
				     0x08, 0xd8);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc9, 0x00, 0x1e, 0x80, 0xa5,
				     0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xcb, 0x00, 0x13, 0x38, 0x00,
				     0x0b, 0x27);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xcc, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd1, 0x07);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd3,
				     0x00, 0xc0, 0x08, 0x08, 0x08, 0x04, 0x04,
				     0x04, 0x14, 0x02, 0x07, 0x07, 0x07, 0x31,
				     0x13, 0x12, 0x12, 0x12, 0x03, 0x03, 0x03,
				     0x32, 0x10, 0x11, 0x00, 0x11, 0x32, 0x10,
				     0x03, 0x00, 0x03, 0x32, 0x10, 0x03, 0x00,
				     0x03, 0x00, 0x00, 0xff, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd5,
				     0x19, 0x19, 0x18, 0x18, 0x02, 0x02, 0x03,
				     0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x06,
				     0x07, 0x07, 0x00, 0x00, 0x01, 0x01, 0x18,
				     0x18, 0x40, 0x40, 0x20, 0x20, 0x18, 0x18,
				     0x18, 0x18, 0x40, 0x40, 0x18, 0x18, 0x2f,
				     0x2f, 0x31, 0x31, 0x2f, 0x2f, 0x31, 0x31,
				     0x18, 0x18, 0x41, 0x41, 0x41, 0x41);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd6,
				     0x40, 0x40, 0x18, 0x18, 0x05, 0x05, 0x04,
				     0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01,
				     0x00, 0x00, 0x07, 0x07, 0x06, 0x06, 0x18,
				     0x18, 0x19, 0x19, 0x20, 0x20, 0x18, 0x18,
				     0x18, 0x18, 0x40, 0x40, 0x18, 0x18, 0x2f,
				     0x2f, 0x31, 0x31, 0x2f, 0x2f, 0x31, 0x31,
				     0x18, 0x18, 0x41, 0x41, 0x41, 0x41);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe1,
				     0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x0a,
				     0x00, 0x06, 0x40, 0x00, 0x28, 0x06, 0x40,
				     0x06, 0x40, 0x02, 0x00, 0x04, 0x21, 0x00,
				     0x20, 0x05, 0xd0, 0x00, 0x16, 0x00, 0x0c,
				     0x02, 0x77, 0x00, 0xda, 0x18, 0x00, 0x10,
				     0xe0, 0x03, 0x0c, 0x20, 0x00, 0x06, 0x0b,
				     0x0b, 0x33, 0x0e, 0x1c, 0x2a, 0x38, 0x46,
				     0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b,
				     0x7d, 0x7e, 0x01, 0x02, 0x01, 0x00, 0x09);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe7, 0x06, 0x14, 0x14, 0x1a,
				     0x23, 0x38, 0x00, 0x23, 0x5d, 0x02, 0x02);

	/* page 1 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb1, 0x01, 0x23, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd8,
				     0x20, 0x00, 0x02, 0x22, 0x00, 0x00, 0x20,
				     0x00, 0x02, 0x22, 0x00, 0x00, 0x20, 0x00,
				     0x02, 0x22, 0x00, 0x00, 0x20, 0x00, 0x02,
				     0x22, 0x00, 0x00, 0x20, 0x00, 0x02, 0x22,
				     0x00, 0x00, 0x20, 0x00, 0x02, 0x22, 0x00,
				     0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe1,
				     0x40, 0x09, 0xbe, 0x19, 0xfc, 0x19, 0xfa,
				     0x19, 0xf8, 0x1a, 0x38, 0x1a, 0x78, 0x1a,
				     0xb6, 0x2a, 0xf6, 0x2b, 0x34, 0x2b, 0x74,
				     0x3b, 0x74, 0x6b, 0xf4);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe7, 0x02, 0x00, 0x40, 0x01,
				     0x84, 0x13, 0xbe, 0x14, 0x48, 0x00, 0x04,
				     0x26);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xcb, 0x1f, 0x55, 0x03, 0x28,
				     0x0d, 0x08, 0x0a);

	/* page 2 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd8, 0xaf, 0xff, 0xfa, 0xfa,
				     0xbf, 0xea, 0xaf, 0xff, 0xfa, 0xfa, 0xbf,
				     0xea);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe7,
				     0x01, 0x05, 0x01, 0x03, 0x01, 0x03, 0x04,
				     0x02, 0x02, 0x24, 0x00, 0x24, 0x81, 0x02,
				     0x40, 0x00, 0x29, 0x60, 0x03, 0x02, 0x01,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00);

	/* page 3 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd8,
				     0xaa, 0xaa, 0xaa, 0xab, 0xbf, 0xea, 0xaa,
				     0xaa, 0xaa, 0xab, 0xbf, 0xea, 0xaf, 0xff,
				     0xfa, 0xfa, 0xbf, 0xea, 0xaf, 0xff, 0xfa,
				     0xfa, 0xbf, 0xea);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe1, 0x01, 0x3f);

	/* back to page 0 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe0,
				     0x00, 0x13, 0x30, 0x36, 0x40, 0x78, 0x8b,
				     0x94, 0x95, 0x97, 0x94, 0x94, 0x91, 0x8f,
				     0x8f, 0x8b, 0x8a, 0x8c, 0x8e, 0xa6, 0xb7,
				     0x4d, 0x7f, 0x00, 0x13, 0x30, 0x36, 0x40,
				     0x78, 0x8b, 0x94, 0x95, 0x97, 0x94, 0x94,
				     0x91, 0x8f, 0x8f, 0x8b, 0x8a, 0x8c, 0x8e,
				     0xa6, 0xb7, 0x4d, 0x7f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xba, 0x70, 0x03, 0xa8, 0x92);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd8,
				     0xea, 0xaa, 0xaa, 0xae, 0xaa, 0xaf, 0xea,
				     0xaa, 0xaa, 0xae, 0xaa, 0xaf, 0xe0, 0x00,
				     0x0a, 0x2e, 0x80, 0x2f, 0xe0, 0x00, 0x0a,
				     0x2e, 0x80, 0x2f, 0xe0, 0x00, 0x0a, 0x2e,
				     0x80, 0x2f, 0xe0, 0x00, 0x0a, 0x2e, 0x80,
				     0x2f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc1, 0x01);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc1,
				     0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18,
				     0x1c, 0x1f, 0x23, 0x27, 0x2b, 0x2f, 0x33,
				     0x37, 0x3b, 0x3f, 0x43, 0x47, 0x4b, 0x52,
				     0x5a, 0x62, 0x69, 0x71, 0x79, 0x81, 0x89,
				     0x91, 0x98, 0xa1, 0xa9, 0xb1, 0xb9, 0xc1,
				     0xca, 0xd2, 0xda, 0xe3, 0xea, 0xf4, 0xf8,
				     0xf9, 0xfb, 0xfd, 0xff, 0x16, 0xa4, 0x44,
				     0x16, 0x90, 0xe7, 0xf9, 0x71, 0xa0, 0xf3,
				     0x1f, 0x40);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc1,
				     0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18,
				     0x1c, 0x20, 0x24, 0x28, 0x2d, 0x31, 0x35,
				     0x39, 0x3d, 0x41, 0x45, 0x49, 0x4d, 0x55,
				     0x5d, 0x65, 0x6d, 0x75, 0x7d, 0x85, 0x8d,
				     0x94, 0x9c, 0xa4, 0xac, 0xb4, 0xbc, 0xc4,
				     0xcc, 0xd4, 0xdc, 0xe4, 0xec, 0xf4, 0xf8,
				     0xfa, 0xfc, 0xfe, 0xff, 0x06, 0xaa, 0xfc,
				     0x5b, 0xff, 0xff, 0xa4, 0xf9, 0x86, 0xf9,
				     0x55, 0x40);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc1,
				     0x00, 0x04, 0x07, 0x0b, 0x0f, 0x13, 0x17,
				     0x1b, 0x1f, 0x23, 0x27, 0x2c, 0x30, 0x33,
				     0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c, 0x53,
				     0x5b, 0x63, 0x6b, 0x72, 0x7a, 0x82, 0x89,
				     0x91, 0x99, 0xa1, 0xa9, 0xb1, 0xb9, 0xc1,
				     0xc9, 0xd1, 0xda, 0xe2, 0xea, 0xf3, 0xf6,
				     0xf9, 0xfa, 0xfe, 0xff, 0x0f, 0x9a, 0xfc,
				     0x31, 0x40, 0xe4, 0xfb, 0xe9, 0xa3, 0xd9,
				     0x77, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbf, 0x72);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbf, 0xfd, 0x00, 0x80, 0x9c,
				     0x10, 0x00, 0x80);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xde);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb1, 0xcc, 0x03, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd0, 0x07, 0xc0, 0x08, 0x03,
				     0x11, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb0, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0xcf);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xba, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x3f);

	mipi_dsi_dcs_exit_sleep_mode_multi(dsi_ctx);
	mipi_dsi_msleep(dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(dsi_ctx);

	return dsi_ctx->accum_err;
}

static const struct drm_display_mode fydetab_pnc357db1_modes[] = {
	{
		/* From the EDK2 panel profile: 275MHz, hbp 40 (not the
		 * vendor-DT hbp 20), 4 hfront/hsync + 112/4/18 vertical. */
		.clock = 275000,
		.hdisplay = 1600,
		.hsync_start = 1600 + 60,
		.hsync_end = 1600 + 60 + 20,
		.htotal = 1600 + 60 + 20 + 40,
		.vdisplay = 2560,
		.vsync_start = 2560 + 112,
		.vsync_end = 2560 + 112 + 4,
		.vtotal = 2560 + 112 + 4 + 18,
	},
};

static const struct panel_desc fydetab_pnc357db1_desc = {
	.width_mm = 166,
	.height_mm = 266,
	.bpc = 8,
	.lanes = 4,
	.format = MIPI_DSI_FMT_RGB888,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
		      MIPI_DSI_MODE_LPM,
	.dsc_cfg = &fydetab_pnc357db1_dsc_cfg,
	.dsc_modes = fydetab_pnc357db1_modes,
	.num_dsc_modes = ARRAY_SIZE(fydetab_pnc357db1_modes),
	.modes = fydetab_pnc357db1_modes,
	.num_modes = ARRAY_SIZE(fydetab_pnc357db1_modes),
	.init_sequence_dsc = fydetab_pnc357db1_init_seq,
	.init_sequence = fydetab_pnc357db1_init_seq,
	.is_dual_dsi = false,
	.has_dcs_backlight = false,
	.dsc_required = true,
	.reset_assert_us = 120000,
	.reset_deassert_ms = 120,
};

/*
 * Known panels with HX83121A:
 * CSOT PNC357DB1-4: on MI Book S 12.4
 * CSOT PNC357DB1 (FydeTab Duo variant): on the FydeTab Duo (RK3588S)
 * CSOT PPC357DB1-1: on SAMSUNG Galaxy Tab S7 FE
 * BOE/CSOT PPC357DB1-4: on HUAWEI Matebook E Go
 * CSOT PPC357DB1-5: on MI Pad 5 Pro 12.4
 */

static const struct of_device_id himax_of_match[] = {
	{ .compatible = "boe,ppc357db1-4", .data = &boe_ppc357db1_4_desc },
	{ .compatible = "csot,ppc357db1-4", .data = &csot_ppc357db1_4_desc },
	{ .compatible = "fydeinnovations,pnc357db1",
	  .data = &fydetab_pnc357db1_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, himax_of_match);

static struct mipi_dsi_driver himax_driver = {
	.probe = himax_probe,
	.remove = himax_remove,
	.driver = {
		.name = "panel-himax-hx83121a",
		.of_match_table = himax_of_match,
	},
};
module_mipi_dsi_driver(himax_driver);

MODULE_AUTHOR("Pengyu Luo <mitltlatltl0@gmail.com>");
MODULE_DESCRIPTION("Himax HX83121A DriverIC panels driver");
MODULE_LICENSE("GPL");
