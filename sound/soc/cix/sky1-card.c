// SPDX-License-Identifier: GPL-2.0
// Copyright 2024 Cix Technology Group Co., Ltd.
#include "card-utils.h"

/*
 * On ACPI boot, audio links do not share one ALSA card. Generic userspace
 * (PipeWire/ACP) only auto-generates per-PCM profiles for HDA cards; a
 * multi-PCM ASoC card collapses to a single sink bound to PCM 0, which
 * may be a disconnected DP port. One card per link is the same topology
 * mainline ASoC HDMI drivers use (e.g. vc4-hdmi) and needs no userspace
 * profile tables.
 *
 * The on-board links (i2s0/i2s3) are registered on the CIXH6070 device
 * itself: their clocks come from audss clkdev entries keyed to an ACPI
 * consumer device's name (devm_clk_hw_register_clkdev(...,
 * dev_name(&adev->dev))), so the card must keep the original consumer
 * identity for clk_get() to resolve mclk0/audio_clk*. The DP links need
 * no clocks at all (mclk_fs == 0 in their link_info), so each gets a
 * "sky1-card-link" child platform device with its own single-link card,
 * its own jack and its own ELD. A DP card deferred while the display
 * stack comes up does not hold back on-board audio; the converse is not
 * true - if the on-board card defers (late audss CLKA or codec), the
 * parent's probe retries and only then registers the DP children.
 *
 * When the on-board card registers, it is the parent's drvdata and the
 * legacy PM behaviour applies to it unchanged. When it does not, the
 * parent carries a zeroed, never-instantiated snd_soc_card stub instead:
 * snd_soc_pm_ops look the card up on every bound device, and
 * snd_soc_card_is_instantiated() turns those callbacks into no-ops for
 * the stub. The DP link children are unregistered via a devm action on
 * the parent.
 */

/*
 * On-board (non-DP) links are the LK_* entries below LK_I2S5_DP0; both
 * link ranges are bound-checked at runtime by
 * cix_card_acpi_i2s_link_present()/cix_card_acpi_dp_link_present().
 */

struct sky1_card_parent {
	/* not a real card; makes snd_soc_pm_ops no-op on the parent */
	struct snd_soc_card card_stub;
	struct platform_device *pdev[SKY1_CARD_DP_LINK_COUNT];
	int num;
};

/* devm action: unregister the DP link children when the parent goes away */
static void sky1_card_unwind_children(void *data)
{
	struct sky1_card_parent *parent = data;
	int i;

	for (i = 0; i < parent->num; i++) {
		if (parent->pdev[i]) {
			platform_device_unregister(parent->pdev[i]);
			parent->pdev[i] = NULL;
		}
	}
}

/*
 * Allocate a card plus its cix_asoc_card wrapper and wire them together.
 * Returns the card, or NULL on -ENOMEM.
 */
static struct snd_soc_card *sky1_card_alloc(struct device *dev,
					    struct cix_asoc_card **privp)
{
	struct cix_asoc_card *priv;
	struct snd_soc_card *card;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return NULL;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return NULL;

	card->owner = THIS_MODULE;
	card->dev = dev;
	priv->card = card;
	*privp = priv;

	return card;
}

/* DP link children: register the single-link card each was created for */
static int sky1_link_card_probe(struct platform_device *pdev)
{
	const unsigned int *dp_idx = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	struct cix_asoc_card *priv;
	struct snd_soc_card *card;
	int ret;

	if (WARN_ON(!dp_idx))
		return -EINVAL;

	card = sky1_card_alloc(dev, &priv);
	if (!card)
		return -ENOMEM;

	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, priv);

	ret = cix_card_parse_acpi_dp(priv, *dp_idx);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "card parse failed (%d)\n", ret);
		return ret;
	}

	ret = devm_snd_soc_register_card(dev, card);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "snd_soc_register_card failed (%d)\n", ret);
		return ret;
	}

	return 0;
}

static int sky1_card_probe_dt(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cix_asoc_card *priv;
	struct snd_soc_card *card;
	int ret;

	/*
	 * DT boot: legacy one-card-per-controller shape, all links in a
	 * single sound card driven by the DT graph.
	 */
	card = sky1_card_alloc(dev, &priv);
	if (!card)
		return -ENOMEM;

	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, priv);

	ret = cix_card_parse_of(priv);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "cix_card_parse_of failed (%d)\n", ret);
		return ret;
	}

	ret = devm_snd_soc_register_card(dev, card);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "snd_soc_register_card failed (%d)\n",
				ret);
		return ret;
	}

	return 0;
}

static int sky1_card_probe_acpi(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sky1_card_parent *parent;
	struct cix_asoc_card *priv;
	struct snd_soc_card *card = NULL;
	unsigned int i;
	int ret;

	parent = devm_kzalloc(dev, sizeof(*parent), GFP_KERNEL);
	if (!parent)
		return -ENOMEM;

	/*
	 * Tie the DP link children to the parent's lifetime; devres is
	 * also released on a failed probe, so this unwinds the children
	 * registered so far if the loop below bails out.
	 */
	ret = devm_add_action_or_reset(dev, sky1_card_unwind_children,
				       parent);
	if (ret)
		return ret;

	/* on-board links: single card on this device, legacy shape */
	for (i = 0; i < LK_I2S5_DP0; i++) {
		if (cix_card_acpi_i2s_link_present(i))
			break;
	}
	if (i < LK_I2S5_DP0) {
		card = sky1_card_alloc(dev, &priv);
		if (!card)
			return -ENOMEM;
		platform_set_drvdata(pdev, card);
		snd_soc_card_set_drvdata(card, priv);

		ret = cix_card_parse_acpi(priv);
		if (ret) {
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "card parse failed (%d)\n", ret);
			return ret;
		}

		ret = devm_snd_soc_register_card(dev, card);
		if (ret) {
			if (ret != -EPROBE_DEFER)
				dev_err(dev,
					"snd_soc_register_card failed (%d)\n",
					ret);
			return ret;
		}
	}

	/* DP link children: one child device per present link */
	for (i = 0; i < SKY1_CARD_DP_LINK_COUNT; i++) {
		unsigned int dp_idx = i;

		if (!cix_card_acpi_dp_link_present(i))
			continue;

		parent->pdev[parent->num] = platform_device_register_data(
			dev, "sky1-card-link", PLATFORM_DEVID_AUTO,
			&dp_idx, sizeof(dp_idx));
		if (IS_ERR(parent->pdev[parent->num]))
			return PTR_ERR(parent->pdev[parent->num]);

		parent->num++;
	}

	if (!card && parent->num == 0)
		return -ENODEV;

	/* if no on-board card registered, keep PM a no-op */
	if (!card)
		platform_set_drvdata(pdev, &parent->card_stub);

	return 0;
}

static int sky1_card_probe(struct platform_device *pdev)
{
	if (!ACPI_COMPANION(&pdev->dev))
		return sky1_card_probe_dt(pdev);

	return sky1_card_probe_acpi(pdev);
}

static const struct acpi_device_id sky1_acpi_match[] = {
	{ "CIXH6070", },
	{ },
};
MODULE_DEVICE_TABLE(acpi, sky1_acpi_match);

static const struct of_device_id sky1_of_match[] = {
	{
		.compatible = "cix,sky1-sound-card",
	}, {}
};
MODULE_DEVICE_TABLE(of, sky1_of_match);

static struct platform_driver sky1_card_pdrv = {
	.driver = {
		.name = "sky1-asoc-card",
		.of_match_table = sky1_of_match,
		.acpi_match_table = sky1_acpi_match,
		.pm = &snd_soc_pm_ops,
	},
	.probe = sky1_card_probe,
};

/* DP link children carry a real snd_soc_card each and need the ASoC pm ops */
static struct platform_driver sky1_link_card_pdrv = {
	.driver = {
		.name = "sky1-card-link",
		.pm = &snd_soc_pm_ops,
	},
	.probe = sky1_link_card_probe,
};

static int __init sky1_card_init(void)
{
	int ret;

	/*
	 * link driver first: children created by the parent's probe must
	 * always find a driver to bind to
	 */
	ret = platform_driver_register(&sky1_link_card_pdrv);
	if (ret)
		return ret;

	ret = platform_driver_register(&sky1_card_pdrv);
	if (ret)
		platform_driver_unregister(&sky1_link_card_pdrv);

	return ret;
}
module_init(sky1_card_init);

static void __exit sky1_card_exit(void)
{
	platform_driver_unregister(&sky1_card_pdrv);
	platform_driver_unregister(&sky1_link_card_pdrv);
}
module_exit(sky1_card_exit);

MODULE_DESCRIPTION("Sky1 ALSA machine driver for Cix Technology");
MODULE_AUTHOR("Xing.Wang <xing.wang@cixtech.com>");
MODULE_LICENSE("GPL v2");
