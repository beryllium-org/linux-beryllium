/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2024 Cix Technology Group Co., Ltd. */

#ifndef __CIX_CARD_H__
#define __CIX_CARD_H__
#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <sound/soc.h>
#include <linux/workqueue.h>
#include <linux/gpio/consumer.h>

enum jack_det {
	JACK_DPIN = 0x0,
	JACK_DPOUT,
	JACK_HP, /* headphone or headset */

	JACK_CNT
};

#define JACK_MASK_DPIN   (0x1 << JACK_DPIN)
#define JACK_MASK_DPOUT  (0x1 << JACK_DPOUT)
#define JACK_MASK_HP     (0x1 << JACK_HP)

enum {
	AUDIO_CLK0,
	AUDIO_CLK2,
	AUDIO_CLK_NUM,
};

enum {
	LK_I2S0_SC_ALC5682_CODEC = 0,
	LK_I2S3_MC_ALC1019_PA,
	LK_I2S5_DP0,
	LK_I2S6_DP1,
	LK_I2S7_DP2,
	LK_I2S8_DP3,
	LK_I2S9_DP4,

	LK_MAX
};

/*
 * On-board links are the entries below LK_I2S5_DP0; the ACPI-enumerated
 * DP audio links (\_SB.DP0x) are the rest. Runtime bounds checks live in
 * cix_card_acpi_i2s_link_present()/cix_card_acpi_dp_link_present().
 */
#define SKY1_CARD_DP_LINK_COUNT		(LK_MAX - LK_I2S5_DP0)

struct dai_link_info {
	struct clk *clk_mclk;
	struct clk *clks[AUDIO_CLK_NUM];
	u8 mclk_idx;
	bool mclk_enabled;

	unsigned int mclk_fs;

	unsigned int tx_mask;
	unsigned int rx_mask;
	unsigned int slots;
	unsigned int slot_width;

	struct snd_soc_jack	jack[JACK_CNT];
	struct snd_soc_jack_pin	jack_pin[JACK_CNT];
	unsigned int jack_det_mask;
};

struct cix_asoc_card {
	struct snd_soc_card *card;
	struct dai_link_info *link_info;

	struct regmap *cru_regmap;

	/* pa, now support for alc1019 */
	struct gpio_desc *pdb0_gpiod; /* for chip power */
	struct gpio_desc *pdb1_gpiod;
	struct gpio_desc *pdb2_gpiod;
	struct gpio_desc *pdb3_gpiod;
};

int cix_card_parse_of(struct cix_asoc_card *priv);
int cix_card_parse_acpi(struct cix_asoc_card *priv);
int cix_card_parse_acpi_dp(struct cix_asoc_card *priv, unsigned int dp_idx);
bool cix_card_acpi_i2s_link_present(unsigned int link_idx);
bool cix_card_acpi_dp_link_present(unsigned int dp_idx);
#endif
