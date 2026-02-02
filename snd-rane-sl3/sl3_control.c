// SPDX-License-Identifier: GPL-3.0
/*
 * Rane SL3 USB Audio Interface - ALSA Mixer Controls
 *
 * Exposes sample rate, channel routing, and device status
 * as ALSA mixer controls.
 */

#include <sound/core.h>
#include <sound/control.h>

#include "sl3.h"

/* Sample Rate enumerated control */

static const char * const sl3_rate_texts[] = { "44100 Hz", "48000 Hz" };

static int sl3_rate_info(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(sl3_rate_texts),
				 sl3_rate_texts);
}

static int sl3_rate_get(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_value *uval)
{
	struct sl3_device *dev = snd_kcontrol_chip(kctl);

	uval->value.enumerated.item[0] =
		(dev->current_rate == 48000) ? 1 : 0;
	return 0;
}

static int sl3_rate_put(struct snd_kcontrol *kctl,
			struct snd_ctl_elem_value *uval)
{
	struct sl3_device *dev = snd_kcontrol_chip(kctl);
	unsigned int new_rate;
	int err;

	new_rate = uval->value.enumerated.item[0] ? 48000 : 44100;

	if (new_rate == dev->current_rate)
		return 0;

	/* Use the full rate switching sequence (handles URB stop/restart) */
	err = sl3_set_sample_rate(dev, new_rate);
	if (err)
		return err;

	return 1; /* value changed */
}

static const struct snd_kcontrol_new sl3_rate_ctl = {
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
	.name	= "Sample Rate",
	.info	= sl3_rate_info,
	.get	= sl3_rate_get,
	.put	= sl3_rate_put,
};

/* Output Source (routing) enumerated controls */

static const char * const sl3_route_texts[] = { "Analog", "USB" };

static int sl3_route_info(struct snd_kcontrol *kctl,
			  struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(sl3_route_texts),
				 sl3_route_texts);
}

static int sl3_route_get(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_value *uval)
{
	struct sl3_device *dev = snd_kcontrol_chip(kctl);
	int idx = kctl->private_value;

	uval->value.enumerated.item[0] = dev->routing[idx];
	return 0;
}

static int sl3_route_put(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_value *uval)
{
	struct sl3_device *dev = snd_kcontrol_chip(kctl);
	int idx = kctl->private_value;
	static const int pair_ids[] = {
		SL3_PAIR_DECK_A, SL3_PAIR_DECK_B, SL3_PAIR_DECK_C
	};
	unsigned int val;
	int err;

	val = uval->value.enumerated.item[0];
	if (val > 1)
		return -EINVAL;

	if (val == dev->routing[idx])
		return 0;

	err = sl3_hid_set_routing(dev, pair_ids[idx], val);
	if (err)
		return err;

	dev->routing[idx] = val;
	return 1; /* value changed */
}

static const struct snd_kcontrol_new sl3_route_ctls[] = {
	{
		.iface		= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name		= "Deck A Output Source",
		.info		= sl3_route_info,
		.get		= sl3_route_get,
		.put		= sl3_route_put,
		.private_value	= 0,
	},
	{
		.iface		= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name		= "Deck B Output Source",
		.info		= sl3_route_info,
		.get		= sl3_route_get,
		.put		= sl3_route_put,
		.private_value	= 1,
	},
	{
		.iface		= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name		= "Deck C Output Source",
		.info		= sl3_route_info,
		.get		= sl3_route_get,
		.put		= sl3_route_put,
		.private_value	= 2,
	},
};

/* Overload Status boolean array (6 channels, read-only, volatile) */

static int sl3_overload_info(struct snd_kcontrol *kctl,
			     struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 6;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int sl3_overload_get(struct snd_kcontrol *kctl,
			    struct snd_ctl_elem_value *uval)
{
	struct sl3_device *dev = snd_kcontrol_chip(kctl);
	int i;

	for (i = 0; i < 6; i++)
		uval->value.integer.value[i] = dev->overload_status[i];
	return 0;
}

static const struct snd_kcontrol_new sl3_overload_ctl = {
	.iface	= SNDRV_CTL_ELEM_IFACE_CARD,
	.name	= "Overload Status",
	.access	= SNDRV_CTL_ELEM_ACCESS_READ |
		  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info	= sl3_overload_info,
	.get	= sl3_overload_get,
};

/* Phono Switch Status boolean array (3 pairs, read-only, volatile) */

static int sl3_phono_info(struct snd_kcontrol *kctl,
			  struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 3;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int sl3_phono_get(struct snd_kcontrol *kctl,
			 struct snd_ctl_elem_value *uval)
{
	struct sl3_device *dev = snd_kcontrol_chip(kctl);
	int i;

	for (i = 0; i < 3; i++)
		uval->value.integer.value[i] = dev->phono_status[i];
	return 0;
}

static const struct snd_kcontrol_new sl3_phono_ctl = {
	.iface	= SNDRV_CTL_ELEM_IFACE_CARD,
	.name	= "Phono Switch Status",
	.access	= SNDRV_CTL_ELEM_ACCESS_READ |
		  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info	= sl3_phono_info,
	.get	= sl3_phono_get,
};

/* Create and register all ALSA mixer controls. */
int sl3_control_init(struct sl3_device *dev)
{
	struct snd_card *card = dev->card;
	struct snd_kcontrol *kctl;
	int i, err;

	/* Sample Rate */
	kctl = snd_ctl_new1(&sl3_rate_ctl, dev);
	if (!kctl)
		return -ENOMEM;
	err = snd_ctl_add(card, kctl);
	if (err)
		return err;

	/* Deck routing controls */
	for (i = 0; i < ARRAY_SIZE(sl3_route_ctls); i++) {
		kctl = snd_ctl_new1(&sl3_route_ctls[i], dev);
		if (!kctl)
			return -ENOMEM;
		err = snd_ctl_add(card, kctl);
		if (err)
			return err;
	}

	/* Overload Status */
	kctl = snd_ctl_new1(&sl3_overload_ctl, dev);
	if (!kctl)
		return -ENOMEM;
	err = snd_ctl_add(card, kctl);
	if (err)
		return err;
	dev->overload_ctl = kctl;

	/* Phono Switch Status */
	kctl = snd_ctl_new1(&sl3_phono_ctl, dev);
	if (!kctl)
		return -ENOMEM;
	err = snd_ctl_add(card, kctl);
	if (err)
		return err;
	dev->phono_ctl = kctl;

	return 0;
}
