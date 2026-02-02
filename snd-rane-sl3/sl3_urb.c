// SPDX-License-Identifier: GPL-3.0
/*
 * Rane SL3 USB Audio Interface - URB Management
 *
 * Isochronous URB allocation, submission, completion callbacks,
 * and audio ring buffer copy logic for playback and capture.
 * Playback uses implicit feedback from capture packet sizes.
 */

#include <linux/usb.h>
#include <linux/slab.h>
#include <sound/pcm.h>

#include "sl3.h"

/* Transfer buffer size per URB: 8 ISO packets x 126 bytes max */
#define SL3_URB_BUFFER_SIZE	(SL3_ISO_PACKETS * SL3_MAX_PACKET_SIZE)

/*
 * Packet sizing constants.
 * USB high-speed isochronous runs at 8000 microframes/sec (125 us each).
 *
 * 48 kHz:   48000 / 8000 = 6.0   samples/microframe -> always 6
 * 44.1 kHz: 44100 / 8000 = 5.5125 samples/microframe -> 5 or 6
 *           base 5, fractional remainder 4100/8000
 */
#define SL3_SAMPLES_48K		6
#define SL3_SAMPLES_44K_BASE	5
#define SL3_FRAC_NUM		4100	/* 44100 - 5 * 8000 */
#define SL3_FRAC_DENOM		8000	/* microframes per second */

static void sl3_playback_complete(struct urb *urb);
static void sl3_capture_complete(struct urb *urb);

/*
 * Return samples for the next ISO packet and advance the fractional
 * accumulator.  Must be called with consistent serialization (either
 * before any URBs are submitted, or under stream->lock).
 */
static unsigned int sl3_next_packet_samples(struct sl3_device *dev)
{
	unsigned int samples;

	if (dev->current_rate == 48000)
		return SL3_SAMPLES_48K;

	/* 44.1 kHz: base 5, add 1 when accumulator overflows */
	samples = SL3_SAMPLES_44K_BASE;
	dev->sample_accumulator += SL3_FRAC_NUM;
	if (dev->sample_accumulator >= SL3_FRAC_DENOM) {
		dev->sample_accumulator -= SL3_FRAC_DENOM;
		samples++;
	}
	return samples;
}

/* Prepare a playback URB filled with silence (used for initial submission) */
static void sl3_prepare_playback_urb(struct sl3_device *dev,
				     struct sl3_urb_ctx *ctx)
{
	struct urb *urb = ctx->urb;
	unsigned int offset = 0;
	int i;

	memset(ctx->buffer, 0, SL3_URB_BUFFER_SIZE);

	for (i = 0; i < SL3_ISO_PACKETS; i++) {
		unsigned int samples = sl3_next_packet_samples(dev);
		unsigned int bytes = samples * SL3_BYTES_PER_FRAME;

		urb->iso_frame_desc[i].offset = offset;
		urb->iso_frame_desc[i].length = bytes;
		offset += bytes;
	}
	urb->transfer_buffer_length = offset;
}

/* Prepare a capture URB to receive data (max packet size per slot) */
static void sl3_prepare_capture_urb(struct sl3_urb_ctx *ctx)
{
	struct urb *urb = ctx->urb;
	unsigned int offset = 0;
	int i;

	for (i = 0; i < SL3_ISO_PACKETS; i++) {
		urb->iso_frame_desc[i].offset = offset;
		urb->iso_frame_desc[i].length = SL3_MAX_PACKET_SIZE;
		offset += SL3_MAX_PACKET_SIZE;
	}
	urb->transfer_buffer_length = offset;
}

/*
 * Copy audio from the ALSA playback ring buffer into an URB and set
 * ISO packet descriptors.  Called under stream->lock from the
 * completion callback.
 */
static void sl3_fill_playback_urb(struct sl3_device *dev,
				  struct sl3_urb_ctx *ctx)
{
	struct urb *urb = ctx->urb;
	struct sl3_stream *stream = &dev->playback;
	struct snd_pcm_substream *sub = stream->substream;
	struct snd_pcm_runtime *runtime = sub ? sub->runtime : NULL;
	unsigned int feedback_total;
	unsigned int offset = 0;
	int i;

	/* Read the implicit feedback sample count (IRQs already disabled) */
	spin_lock(&dev->feedback_lock);
	feedback_total = dev->feedback_samples;
	spin_unlock(&dev->feedback_lock);

	for (i = 0; i < SL3_ISO_PACKETS; i++) {
		unsigned int samples;
		unsigned int bytes;

		if (dev->capture.running && feedback_total > 0) {
			/* Distribute feedback evenly across remaining packets */
			unsigned int remaining = SL3_ISO_PACKETS - i;

			samples = (feedback_total + remaining - 1) / remaining;
			if (samples > SL3_MAX_PACKET_SIZE / SL3_BYTES_PER_FRAME)
				samples = SL3_MAX_PACKET_SIZE /
					  SL3_BYTES_PER_FRAME;
			feedback_total -= samples;
		} else {
			samples = sl3_next_packet_samples(dev);
		}

		bytes = samples * SL3_BYTES_PER_FRAME;
		urb->iso_frame_desc[i].offset = offset;
		urb->iso_frame_desc[i].length = bytes;

		if (runtime && runtime->dma_area) {
			unsigned int buf_size = runtime->buffer_size;
			unsigned int hwptr = stream->hwptr % buf_size;
			unsigned int hwptr_bytes = hwptr * SL3_BYTES_PER_FRAME;
			unsigned int buf_bytes =
				snd_pcm_lib_buffer_bytes(sub);

			if (hwptr_bytes + bytes <= buf_bytes) {
				memcpy(ctx->buffer + offset,
				       runtime->dma_area + hwptr_bytes,
				       bytes);
			} else {
				unsigned int c1 = buf_bytes - hwptr_bytes;

				memcpy(ctx->buffer + offset,
				       runtime->dma_area + hwptr_bytes, c1);
				memcpy(ctx->buffer + offset + c1,
				       runtime->dma_area, bytes - c1);
			}
			stream->hwptr += samples;
			stream->transfer_done += samples;
		} else {
			memset(ctx->buffer + offset, 0, bytes);
		}

		offset += bytes;
	}
	urb->transfer_buffer_length = offset;
}

/* Allocate isochronous URBs and DMA buffers for a stream. */
int sl3_urb_alloc(struct sl3_device *dev, struct sl3_stream *stream, int pipe)
{
	bool is_playback = (stream == &dev->playback);
	int i;

	for (i = 0; i < SL3_NUM_URBS; i++) {
		struct sl3_urb_ctx *ctx = &stream->urbs[i];
		struct urb *urb;

		urb = usb_alloc_urb(SL3_ISO_PACKETS, GFP_KERNEL);
		if (!urb)
			goto err_free;

		ctx->buffer = usb_alloc_coherent(dev->udev,
						 SL3_URB_BUFFER_SIZE,
						 GFP_KERNEL,
						 &urb->transfer_dma);
		if (!ctx->buffer) {
			usb_free_urb(urb);
			goto err_free;
		}

		urb->dev = dev->udev;
		urb->pipe = pipe;
		urb->transfer_buffer = ctx->buffer;
		urb->transfer_buffer_length = SL3_URB_BUFFER_SIZE;
		urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP |
				      URB_ISO_ASAP;
		urb->number_of_packets = SL3_ISO_PACKETS;
		urb->interval = 1;
		urb->context = ctx;
		urb->complete = is_playback ? sl3_playback_complete
					    : sl3_capture_complete;

		ctx->urb = urb;
		ctx->dev = dev;
		ctx->index = i;
	}

	return 0;

err_free:
	sl3_urb_free(dev, stream);
	return -ENOMEM;
}

/* Free all URBs and DMA buffers for a stream. */
void sl3_urb_free(struct sl3_device *dev, struct sl3_stream *stream)
{
	int i;

	for (i = 0; i < SL3_NUM_URBS; i++) {
		struct sl3_urb_ctx *ctx = &stream->urbs[i];

		if (!ctx->urb)
			continue;

		if (ctx->buffer) {
			usb_free_coherent(dev->udev, SL3_URB_BUFFER_SIZE,
					  ctx->buffer,
					  ctx->urb->transfer_dma);
			ctx->buffer = NULL;
		}
		usb_free_urb(ctx->urb);
		ctx->urb = NULL;
	}
}

/* Prepare and submit all URBs to start audio streaming. */
int sl3_urb_start(struct sl3_device *dev, struct sl3_stream *stream)
{
	bool is_playback = (stream == &dev->playback);
	int i, err;

	if (dev->disconnected)
		return -ENODEV;

	/* Already running (e.g. implicit capture started by playback) */
	if (stream->running)
		return 0;

	if (is_playback)
		dev->sample_accumulator = 0;

	/* Prepare all URBs before submitting to avoid races with completions */
	for (i = 0; i < SL3_NUM_URBS; i++) {
		if (is_playback)
			sl3_prepare_playback_urb(dev, &stream->urbs[i]);
		else
			sl3_prepare_capture_urb(&stream->urbs[i]);
	}

	stream->running = true;

	/* Playback requires capture for implicit feedback */
	if (is_playback && !dev->capture.running) {
		err = sl3_urb_start(dev, &dev->capture);
		if (err) {
			dev_err(&dev->intf->dev,
				"implicit capture start failed: %d\n", err);
			stream->running = false;
			return err;
		}
	}

	for (i = 0; i < SL3_NUM_URBS; i++) {
		err = usb_submit_urb(stream->urbs[i].urb, GFP_ATOMIC);
		if (err) {
			dev_err(&dev->intf->dev,
				"%s URB[%d] submit failed: %d\n",
				is_playback ? "playback" : "capture",
				i, err);
			stream->running = false;
			return err;
		}
	}

	dev_dbg(&dev->intf->dev, "%s streaming started (%u Hz)\n",
		is_playback ? "playback" : "capture", dev->current_rate);
	return 0;
}

/* Kill all in-flight URBs and stop audio streaming. */
void sl3_urb_stop(struct sl3_device *dev, struct sl3_stream *stream)
{
	bool is_playback = (stream == &dev->playback);
	int i;

	if (!stream->running)
		return;

	stream->running = false;

	for (i = 0; i < SL3_NUM_URBS; i++) {
		if (stream->urbs[i].urb)
			usb_kill_urb(stream->urbs[i].urb);
	}

	/* Stop implicit capture if playback no longer needs it */
	if (is_playback && dev->capture.running && !dev->capture.substream)
		sl3_urb_stop(dev, &dev->capture);

	dev_dbg(&dev->intf->dev, "%s streaming stopped\n",
		is_playback ? "playback" : "capture");
}

/* --- completion callbacks ----------------------------------------- */

static void sl3_playback_complete(struct urb *urb)
{
	struct sl3_urb_ctx *ctx = urb->context;
	struct sl3_device *dev = ctx->dev;
	struct sl3_stream *stream = &dev->playback;
	struct snd_pcm_substream *sub;
	unsigned long flags;
	bool do_elapsed = false;
	int err;

	switch (urb->status) {
	case 0:
		ctx->error_retries = 0;
		break;
	case -ENOENT:
	case -ECONNRESET:
		/* Normal URB kill — do not resubmit */
		return;
	case -ESHUTDOWN:
		/* Device gone */
		dev->disconnected = true;
		return;
	case -EOVERFLOW:
		dev_warn_ratelimited(&dev->intf->dev,
				     "playback URB[%d] overflow\n",
				     ctx->index);
		goto resubmit;
	case -EPIPE:
		dev_warn_ratelimited(&dev->intf->dev,
				     "playback URB[%d] stall, clearing halt\n",
				     ctx->index);
		usb_clear_halt(dev->udev, urb->pipe);
		goto resubmit;
	default:
		dev_warn_ratelimited(&dev->intf->dev,
				     "playback URB[%d] error: %d\n",
				     ctx->index, urb->status);
		if (++ctx->error_retries >= SL3_URB_MAX_RETRIES) {
			dev_err(&dev->intf->dev,
				"playback URB[%d] %d consecutive errors, stopping\n",
				ctx->index, ctx->error_retries);
			sub = stream->substream;
			if (sub) {
				atomic_inc(&dev->play_underruns);
				snd_pcm_stop_xrun(sub);
			}
			return;
		}
		goto resubmit;
	}

	if (!stream->running || dev->disconnected)
		return;

	atomic64_inc(&dev->play_urbs_completed);

	spin_lock_irqsave(&stream->lock, flags);

	sub = stream->substream;
	sl3_fill_playback_urb(dev, ctx);

	if (sub && sub->runtime) {
		while (stream->transfer_done >= sub->runtime->period_size) {
			stream->transfer_done -= sub->runtime->period_size;
			do_elapsed = true;
		}
	}

	spin_unlock_irqrestore(&stream->lock, flags);

	if (do_elapsed)
		snd_pcm_period_elapsed(sub);

resubmit:
	if (stream->running && !dev->disconnected) {
		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (err) {
			if (err == -ENODEV || err == -ENOENT)
				return;
			dev_err_ratelimited(&dev->intf->dev,
					    "playback URB[%d] resubmit: %d\n",
					    ctx->index, err);
		}
	}
}

static void sl3_capture_complete(struct urb *urb)
{
	struct sl3_urb_ctx *ctx = urb->context;
	struct sl3_device *dev = ctx->dev;
	struct sl3_stream *stream = &dev->capture;
	struct snd_pcm_substream *sub;
	struct snd_pcm_runtime *runtime;
	unsigned int total_samples = 0;
	unsigned long flags;
	bool do_elapsed = false;
	int i, err;

	switch (urb->status) {
	case 0:
		ctx->error_retries = 0;
		break;
	case -ENOENT:
	case -ECONNRESET:
		/* Normal URB kill — do not resubmit */
		return;
	case -ESHUTDOWN:
		/* Device gone */
		dev->disconnected = true;
		return;
	case -EOVERFLOW:
		dev_warn_ratelimited(&dev->intf->dev,
				     "capture URB[%d] overflow\n",
				     ctx->index);
		goto resubmit;
	case -EPIPE:
		dev_warn_ratelimited(&dev->intf->dev,
				     "capture URB[%d] stall, clearing halt\n",
				     ctx->index);
		usb_clear_halt(dev->udev, urb->pipe);
		goto resubmit;
	default:
		dev_warn_ratelimited(&dev->intf->dev,
				     "capture URB[%d] error: %d\n",
				     ctx->index, urb->status);
		if (++ctx->error_retries >= SL3_URB_MAX_RETRIES) {
			dev_err(&dev->intf->dev,
				"capture URB[%d] %d consecutive errors, stopping\n",
				ctx->index, ctx->error_retries);
			sub = stream->substream;
			if (sub) {
				atomic_inc(&dev->cap_overruns);
				snd_pcm_stop_xrun(sub);
			}
			return;
		}
		goto resubmit;
	}

	if (!stream->running || dev->disconnected)
		return;

	atomic64_inc(&dev->cap_urbs_completed);

	spin_lock_irqsave(&stream->lock, flags);

	sub = stream->substream;
	runtime = sub ? sub->runtime : NULL;

	for (i = 0; i < SL3_ISO_PACKETS; i++) {
		unsigned int actual = urb->iso_frame_desc[i].actual_length;
		unsigned int samples = actual / SL3_BYTES_PER_FRAME;
		unsigned int bytes = samples * SL3_BYTES_PER_FRAME;
		unsigned int buf_size, hwptr, hwptr_bytes, buf_bytes, pkt_off;

		total_samples += samples;

		if (!runtime || !runtime->dma_area || !bytes)
			continue;

		/* Copy packet data into ring buffer, handling wraparound */
		buf_size = runtime->buffer_size;
		hwptr = stream->hwptr % buf_size;
		hwptr_bytes = hwptr * SL3_BYTES_PER_FRAME;
		buf_bytes = snd_pcm_lib_buffer_bytes(sub);
		pkt_off = urb->iso_frame_desc[i].offset;

		if (hwptr_bytes + bytes <= buf_bytes) {
			memcpy(runtime->dma_area + hwptr_bytes,
			       ctx->buffer + pkt_off, bytes);
		} else {
			unsigned int c1 = buf_bytes - hwptr_bytes;

			memcpy(runtime->dma_area + hwptr_bytes,
			       ctx->buffer + pkt_off, c1);
			memcpy(runtime->dma_area,
			       ctx->buffer + pkt_off + c1,
			       bytes - c1);
		}
		stream->hwptr += samples;
		stream->transfer_done += samples;
	}

	if (sub && runtime) {
		while (stream->transfer_done >= runtime->period_size) {
			stream->transfer_done -= runtime->period_size;
			do_elapsed = true;
		}
	}

	spin_unlock_irqrestore(&stream->lock, flags);

	/* Update implicit feedback for the playback side */
	spin_lock_irqsave(&dev->feedback_lock, flags);
	dev->feedback_samples = total_samples;
	spin_unlock_irqrestore(&dev->feedback_lock, flags);

	if (do_elapsed)
		snd_pcm_period_elapsed(sub);

resubmit:
	/* Prepare for next receive and resubmit */
	if (stream->running && !dev->disconnected) {
		sl3_prepare_capture_urb(ctx);
		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (err) {
			if (err == -ENODEV || err == -ENOENT)
				return;
			dev_err_ratelimited(&dev->intf->dev,
					    "capture URB[%d] resubmit: %d\n",
					    ctx->index, err);
		}
	}
}
