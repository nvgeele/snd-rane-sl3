// SPDX-License-Identifier: GPL-3.0
/*
 * Rane SL3 USB Audio Interface - ALSA Driver
 *
 * Proc filesystem entries for device status and statistics
 */

#include <sound/info.h>

#include "sl3.h"

static const char * const route_names[] = { "Analog", "USB" };

static void sl3_proc_read_status(struct snd_info_entry *entry,
				 struct snd_info_buffer *buffer)
{
	struct sl3_device *dev = entry->private_data;

	snd_iprintf(buffer, "Rane SL3 USB Audio Interface\n");
	snd_iprintf(buffer, "  Sample Rate:    %u Hz\n", dev->current_rate);
	snd_iprintf(buffer, "  Deck A Routing: %s\n",
		     route_names[dev->routing[0] & 1]);
	snd_iprintf(buffer, "  Deck B Routing: %s\n",
		     route_names[dev->routing[1] & 1]);
	snd_iprintf(buffer, "  Deck C Routing: %s\n",
		     route_names[dev->routing[2] & 1]);
	snd_iprintf(buffer, "  Playback:       %s\n",
		     dev->playback.running ? "running" : "stopped");
	snd_iprintf(buffer, "  Capture:        %s\n",
		     dev->capture.running ? "running" : "stopped");
	snd_iprintf(buffer, "  Disconnected:   %s\n",
		     dev->disconnected ? "yes" : "no");
}

static void sl3_proc_read_overload(struct snd_info_entry *entry,
				   struct snd_info_buffer *buffer)
{
	struct sl3_device *dev = entry->private_data;
	static const char * const ch_names[] = {
		"Deck A Left ", "Deck A Right",
		"Deck B Left ", "Deck B Right",
		"Deck C Left ", "Deck C Right",
	};
	int i;

	snd_iprintf(buffer, "Overload Status\n");
	for (i = 0; i < 6; i++)
		snd_iprintf(buffer, "  %s: %s\n", ch_names[i],
			     dev->overload_status[i] ? "OVERLOAD" : "OK");
}

static void sl3_proc_read_phono(struct snd_info_entry *entry,
				struct snd_info_buffer *buffer)
{
	struct sl3_device *dev = entry->private_data;
	static const char * const pair_names[] = {
		"Deck A", "Deck B", "Deck C",
	};
	int i;

	snd_iprintf(buffer, "Phono Switch Status\n");
	for (i = 0; i < 3; i++)
		snd_iprintf(buffer, "  %s: %s\n", pair_names[i],
			     dev->phono_status[i] ? "PHONO" : "LINE");
}

static void sl3_proc_read_usb_port(struct snd_info_entry *entry,
				   struct snd_info_buffer *buffer)
{
	struct sl3_device *dev = entry->private_data;

	snd_iprintf(buffer, "USB Port Status\n");
	snd_iprintf(buffer, "  Byte 0: 0x%02x\n", dev->usb_port_status[0]);
	snd_iprintf(buffer, "  Byte 1: 0x%02x\n", dev->usb_port_status[1]);
	snd_iprintf(buffer, "  Byte 2: 0x%02x\n", dev->usb_port_status[2]);
	snd_iprintf(buffer, "  Byte 3: 0x%02x\n", dev->usb_port_status[3]);
}

static void sl3_proc_read_statistics(struct snd_info_entry *entry,
				     struct snd_info_buffer *buffer)
{
	struct sl3_device *dev = entry->private_data;
	unsigned int fb_samples;
	unsigned long flags;

	spin_lock_irqsave(&dev->feedback_lock, flags);
	fb_samples = dev->feedback_samples;
	spin_unlock_irqrestore(&dev->feedback_lock, flags);

	snd_iprintf(buffer, "Streaming Statistics\n");
	snd_iprintf(buffer, "  Playback URBs Completed: %lld\n",
		     atomic64_read(&dev->play_urbs_completed));
	snd_iprintf(buffer, "  Capture URBs Completed:  %lld\n",
		     atomic64_read(&dev->cap_urbs_completed));
	snd_iprintf(buffer, "  Playback Underruns:      %d\n",
		     atomic_read(&dev->play_underruns));
	snd_iprintf(buffer, "  Capture Overruns:        %d\n",
		     atomic_read(&dev->cap_overruns));
	snd_iprintf(buffer, "  Discontinuities:         %d\n",
		     atomic_read(&dev->discontinuities));
	snd_iprintf(buffer, "  Implicit Feedback Samples: %u\n", fb_samples);
	snd_iprintf(buffer, "  Nominal Rate:            %u Hz\n",
		     dev->current_rate);
}

/* Create proc filesystem entries under /proc/asound/cardN/. */
void sl3_proc_init(struct sl3_device *dev)
{
	snd_card_ro_proc_new(dev->card, "status",
			     dev, sl3_proc_read_status);
	snd_card_ro_proc_new(dev->card, "overload",
			     dev, sl3_proc_read_overload);
	snd_card_ro_proc_new(dev->card, "phono_switches",
			     dev, sl3_proc_read_phono);
	snd_card_ro_proc_new(dev->card, "usb_port",
			     dev, sl3_proc_read_usb_port);
	snd_card_ro_proc_new(dev->card, "statistics",
			     dev, sl3_proc_read_statistics);
}
