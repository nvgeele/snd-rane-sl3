/* SPDX-License-Identifier: GPL-3.0 */
/*
 * Rane SL3 USB Audio Interface - ALSA Driver
 *
 * Master header: constants, data structures, forward declarations
 */

#ifndef SL3_H
#define SL3_H

#include <linux/usb.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/control.h>

/* USB device identification */
#define SL3_VENDOR_ID		0x1CC5
#define SL3_PRODUCT_ID		0x0001

/* Audio format */
#define SL3_NUM_CHANNELS	6
#define SL3_BYTES_PER_SAMPLE	3	/* 24-bit */
#define SL3_BYTES_PER_FRAME	18	/* 6 * 3 */
#define SL3_MAX_PACKET_SIZE	126	/* 7 * 18 */

/* URB configuration */
#define SL3_NUM_URBS		16
#define SL3_ISO_PACKETS		8	/* packets per URB */
#define SL3_URB_MAX_RETRIES	3	/* max consecutive errors before xrun */

/* USB interface numbers */
#define SL3_INTF_AUDIO_CTRL	0
#define SL3_INTF_AUDIO_OUT	1	/* Playback (host->device) */
#define SL3_INTF_AUDIO_IN	2	/* Capture (device->host) */
#define SL3_INTF_HID		3

/* Endpoint addresses */
#define SL3_EP_AUDIO_OUT	0x06	/* ISO OUT - playback */
#define SL3_EP_AUDIO_IN		0x82	/* ISO IN  - capture + implicit feedback */
#define SL3_EP_HID_OUT		0x01	/* Interrupt OUT */
#define SL3_EP_HID_IN		0x81	/* Interrupt IN */

/* HID command IDs */
#define SL3_HID_CMD_INIT	0x03
#define SL3_HID_CMD_SAMPLE_RATE	0x31
#define SL3_HID_CMD_ROUTING	0x33
#define SL3_HID_CMD_QUERY_PHONO	0x32
#define SL3_HID_CMD_STATUS	0x36

/* Async notification command IDs */
#define SL3_HID_NOTIFY_OVERLOAD	0x34
#define SL3_HID_NOTIFY_PHONO	0x38
#define SL3_HID_NOTIFY_USB_PORT	0x39

/* Channel pair identifiers for routing command */
#define SL3_PAIR_DECK_A		0x08	/* Channels 1/2 */
#define SL3_PAIR_DECK_B		0x0E	/* Channels 3/4 */
#define SL3_PAIR_DECK_C		0x14	/* Channels 5/6 */

/* Routing modes */
#define SL3_ROUTE_ANALOG	0x00
#define SL3_ROUTE_USB		0x01

/* HID report size */
#define SL3_HID_REPORT_SIZE	64

struct sl3_hid_report {
	u8 command;			/* Byte 0 */
	__be16 vendor_id;		/* Bytes 1-2: 0x1CC5 BE */
	__be16 product_id;		/* Bytes 3-4: 0x0001 BE */
	u8 payload[59];			/* Bytes 5-63 */
} __packed;

struct sl3_device;

struct sl3_urb_ctx {
	struct urb		*urb;
	u8			*buffer;
	struct sl3_device	*dev;
	int			index;
	int			error_retries;	/* consecutive error count */
};

struct sl3_stream {
	struct snd_pcm_substream *substream;
	struct sl3_urb_ctx	urbs[SL3_NUM_URBS];
	unsigned int		hwptr;		/* hardware pointer in frames */
	unsigned int		transfer_done;	/* frames since last period_elapsed */
	bool			running;
	spinlock_t		lock;
};

struct sl3_device {
	struct usb_device	*udev;
	struct usb_interface	*intf;
	struct snd_card		*card;
	struct snd_pcm		*pcm;

	/* Audio streams */
	struct sl3_stream	playback;
	struct sl3_stream	capture;

	/* Current configuration */
	unsigned int		current_rate;	/* 44100 or 48000 */
	u8			routing[3];	/* per-pair: 0x00=analog, 0x01=USB */

	/* Implicit feedback tracking */
	unsigned int		feedback_samples;
	spinlock_t		feedback_lock;

	/* 44.1kHz fractional sample accumulator */
	unsigned int		sample_accumulator;

	/* HID subsystem */
	struct urb		*hid_in_urb;
	u8			*hid_in_buf;
	dma_addr_t		hid_in_dma;
	u8			*hid_out_buf;	/* kmalloc'd for DMA safety */
	struct completion	hid_response_complete;
	u8			hid_response_buf[SL3_HID_REPORT_SIZE];
	struct mutex		hid_mutex;

	/* Async device status (updated from HID IN callback) */
	u8			overload_status[6];	/* per-channel (HID 0x34) */
	u8			phono_status[3];	/* per-pair (HID 0x38) */
	u8			usb_port_status[4];	/* (HID 0x39) */

	/* ALSA controls for notification dispatch */
	struct snd_kcontrol	*overload_ctl;
	struct snd_kcontrol	*phono_ctl;

	/* Statistics */
	atomic64_t		play_urbs_completed;
	atomic64_t		cap_urbs_completed;
	atomic_t		play_underruns;
	atomic_t		cap_overruns;
	atomic_t		discontinuities;

	/* Lifecycle */
	bool			disconnected;
	struct mutex		stream_mutex;
};

/* sl3_hid.c */
int sl3_hid_init(struct sl3_device *dev);
void sl3_hid_cleanup(struct sl3_device *dev);
int sl3_hid_send_command(struct sl3_device *dev, u8 cmd,
			 u8 *payload, int payload_len);
int sl3_hid_set_sample_rate(struct sl3_device *dev, unsigned int rate);
int sl3_hid_set_routing(struct sl3_device *dev, int pair, int mode);
int sl3_hid_query_phono(struct sl3_device *dev);

/* sl3_pcm.c */
int sl3_pcm_init(struct sl3_device *dev);
int sl3_set_sample_rate(struct sl3_device *dev, unsigned int rate);

/* sl3_urb.c */
int sl3_urb_alloc(struct sl3_device *dev, struct sl3_stream *stream, int pipe);
void sl3_urb_free(struct sl3_device *dev, struct sl3_stream *stream);
int sl3_urb_start(struct sl3_device *dev, struct sl3_stream *stream);
void sl3_urb_stop(struct sl3_device *dev, struct sl3_stream *stream);

/* sl3_control.c */
int sl3_control_init(struct sl3_device *dev);

/* sl3_proc.c */
void sl3_proc_init(struct sl3_device *dev);

#endif /* SL3_H */
