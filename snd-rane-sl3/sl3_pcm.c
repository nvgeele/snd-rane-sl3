// SPDX-License-Identifier: GPL-3.0
/*
 * Rane SL3 USB Audio Interface - ALSA PCM Device
 *
 * Registers an ALSA sound card with a 6-channel PCM device.
 * Implements PCM operations: open, close, hw_params, prepare, trigger, pointer.
 * Also contains the sample rate switching sequence (sl3_set_sample_rate).
 */

#include <linux/slab.h>
#include <linux/delay.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>

#include "sl3.h"

static const struct snd_pcm_hardware sl3_pcm_hw = {
	.info =			SNDRV_PCM_INFO_MMAP |
				SNDRV_PCM_INFO_MMAP_VALID |
				SNDRV_PCM_INFO_INTERLEAVED |
				SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats =		SNDRV_PCM_FMTBIT_S24_3LE,
	.rates =		SNDRV_PCM_RATE_44100 |
				SNDRV_PCM_RATE_48000,
	.rate_min =		44100,
	.rate_max =		48000,
	.channels_min =		SL3_NUM_CHANNELS,
	.channels_max =		SL3_NUM_CHANNELS,
	.buffer_bytes_max =	256 * 1024,
	.period_bytes_min =	SL3_BYTES_PER_FRAME,
	.period_bytes_max =	128 * 1024,
	.periods_min =		2,
	.periods_max =		1024,
};

/*
 * Rate constraint rule: if the other stream is already open and has
 * a rate configured, constrain this stream to the same rate.
 */
static int sl3_pcm_hw_rule_rate(struct snd_pcm_hw_params *params,
				struct snd_pcm_hw_rule *rule)
{
	struct snd_pcm_substream *substream = rule->private;
	struct sl3_device *dev = snd_pcm_substream_chip(substream);
	struct snd_pcm_substream *other_sub;
	struct snd_interval *rate;
	struct snd_interval constraint;

	/* Check if the other direction has an open substream with a rate set */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		other_sub = dev->capture.substream;
	else
		other_sub = dev->playback.substream;

	if (!other_sub || !other_sub->runtime ||
	    !other_sub->runtime->rate)
		return 0;	/* No constraint */

	rate = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);

	constraint.openmin = 0;
	constraint.openmax = 0;
	constraint.min = other_sub->runtime->rate;
	constraint.max = other_sub->runtime->rate;
	constraint.integer = 1;

	return snd_interval_refine(rate, &constraint);
}

static int sl3_pcm_open(struct snd_pcm_substream *substream)
{
	struct sl3_device *dev = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	if (dev->disconnected)
		return -ENODEV;

	runtime->hw = sl3_pcm_hw;

	/* Store substream reference */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		dev->playback.substream = substream;
	else
		dev->capture.substream = substream;

	/* Add rate constraint: both streams must use the same rate */
	snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
			    sl3_pcm_hw_rule_rate, substream,
			    SNDRV_PCM_HW_PARAM_RATE, -1);

	return 0;
}

static int sl3_pcm_close(struct snd_pcm_substream *substream)
{
	struct sl3_device *dev = snd_pcm_substream_chip(substream);
	struct sl3_stream *stream;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		stream = &dev->playback;
	else
		stream = &dev->capture;

	/* Kill any lingering URBs (safe to call even if already stopped) */
	sl3_urb_stop(dev, stream);
	stream->substream = NULL;

	return 0;
}

static int sl3_pcm_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params)
{
	struct sl3_device *dev = snd_pcm_substream_chip(substream);
	unsigned int rate = params_rate(params);

	if (dev->disconnected)
		return -ENODEV;

	/* Use the full rate switching sequence (handles URB stop/restart) */
	return sl3_set_sample_rate(dev, rate);
}

static int sl3_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct sl3_device *dev = snd_pcm_substream_chip(substream);
	struct sl3_stream *stream;

	if (dev->disconnected)
		return -ENODEV;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		stream = &dev->playback;
	else
		stream = &dev->capture;

	stream->hwptr = 0;
	stream->transfer_done = 0;

	return 0;
}

static int sl3_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct sl3_device *dev = snd_pcm_substream_chip(substream);
	struct sl3_stream *stream;
	bool is_playback;

	if (dev->disconnected)
		return -ENODEV;

	is_playback = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK);
	stream = is_playback ? &dev->playback : &dev->capture;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		return sl3_urb_start(dev, stream);
	case SNDRV_PCM_TRIGGER_STOP:
		stream->running = false;
		/* Stop implicit capture if playback no longer needs it */
		if (is_playback && dev->capture.running &&
		    !dev->capture.substream)
			dev->capture.running = false;
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t sl3_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct sl3_device *dev = snd_pcm_substream_chip(substream);
	struct sl3_stream *stream;
	unsigned int hwptr;

	if (dev->disconnected)
		return SNDRV_PCM_POS_XRUN;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		stream = &dev->playback;
	else
		stream = &dev->capture;

	hwptr = stream->hwptr;
	return hwptr % substream->runtime->buffer_size;
}

/*
 * Full sample rate switching sequence per spec section 7.
 * Stops URBs if running, sends HID command, resets accumulator,
 * and restarts URBs. Caller must NOT hold stream_mutex.
 */
int sl3_set_sample_rate(struct sl3_device *dev, unsigned int rate)
{
	int err;

	if (rate != 44100 && rate != 48000)
		return -EINVAL;

	mutex_lock(&dev->stream_mutex);

	if (rate == dev->current_rate) {
		mutex_unlock(&dev->stream_mutex);
		return 0;
	}

	/* Cannot switch while a stream is actively running */
	if (dev->playback.running || dev->capture.running) {
		mutex_unlock(&dev->stream_mutex);
		return -EBUSY;
	}

	/* Send HID rate change command and wait for 0xFF response */
	err = sl3_hid_set_sample_rate(dev, rate);
	if (err) {
		dev_err(&dev->intf->dev,
			"HID set sample rate to %u failed: %d\n", rate, err);
		mutex_unlock(&dev->stream_mutex);
		return err;
	}

	/* Device stabilization delay */
	msleep(100);

	/* Reset fractional sample accumulator for 44.1kHz pattern */
	dev->sample_accumulator = 0;

	dev_info(&dev->intf->dev, "sample rate switched to %u Hz\n", rate);

	mutex_unlock(&dev->stream_mutex);
	return 0;
}

static const struct snd_pcm_ops sl3_playback_ops = {
	.open =		sl3_pcm_open,
	.close =	sl3_pcm_close,
	.hw_params =	sl3_pcm_hw_params,
	.prepare =	sl3_pcm_prepare,
	.trigger =	sl3_pcm_trigger,
	.pointer =	sl3_pcm_pointer,
};

static const struct snd_pcm_ops sl3_capture_ops = {
	.open =		sl3_pcm_open,
	.close =	sl3_pcm_close,
	.hw_params =	sl3_pcm_hw_params,
	.prepare =	sl3_pcm_prepare,
	.trigger =	sl3_pcm_trigger,
	.pointer =	sl3_pcm_pointer,
};

static void sl3_card_private_free(struct snd_card *card)
{
	struct sl3_device *dev = card->private_data;

	kfree(dev);
}

/* Create and register the ALSA sound card and PCM device. */
int sl3_pcm_init(struct sl3_device *dev)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	int err;

	err = snd_card_new(&dev->intf->dev, SNDRV_DEFAULT_IDX1,
			   "RaneSL3", THIS_MODULE, 0, &card);
	if (err < 0)
		return err;

	dev->card = card;
	card->private_data = dev;
	card->private_free = sl3_card_private_free;

	strscpy(card->driver, "snd_rane_sl3", sizeof(card->driver));
	strscpy(card->shortname, "Rane SL3", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "Rane SL3 at %s", dev_name(&dev->udev->dev));

	err = snd_pcm_new(card, "Rane SL3", 0, 1, 1, &pcm);
	if (err < 0)
		goto err_free_card;

	dev->pcm = pcm;
	pcm->private_data = dev;
	strscpy(pcm->name, "Rane SL3", sizeof(pcm->name));

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &sl3_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &sl3_capture_ops);

	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
				       0, 0);

	return 0;

err_free_card:
	snd_card_free(card);
	dev->card = NULL;
	return err;
}
