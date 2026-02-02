// SPDX-License-Identifier: GPL-3.0
/*
 * Rane SL3 USB Audio Interface - HID Command Interface
 *
 * Implements HID control communication for device configuration:
 * sending commands and receiving responses/notifications.
 */

#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include "sl3.h"

/* Timeout for USB interrupt messages (ms) */
#define SL3_HID_USB_TIMEOUT_MS	1000

/* Timeout for waiting on HID response from device (ms) */
#define SL3_HID_RESP_TIMEOUT_MS	500

/* Build a 64-byte HID report with command, VID/PID header, and payload */
static void sl3_hid_build_report(u8 *buf, u8 cmd,
				 const u8 *payload, int payload_len)
{
	memset(buf, 0, SL3_HID_REPORT_SIZE);
	buf[0] = cmd;
	/* Bytes 1-4: VID/PID in big-endian byte order (per USB captures) */
	buf[1] = (SL3_VENDOR_ID >> 8) & 0xff;
	buf[2] = SL3_VENDOR_ID & 0xff;
	buf[3] = (SL3_PRODUCT_ID >> 8) & 0xff;
	buf[4] = SL3_PRODUCT_ID & 0xff;
	if (payload && payload_len > 0)
		memcpy(&buf[5], payload,
		       min_t(int, payload_len, SL3_HID_REPORT_SIZE - 5));
}

/* HID IN URB completion callback - dispatches responses and notifications */
static void sl3_hid_in_complete(struct urb *urb)
{
	struct sl3_device *dev = urb->context;
	u8 *data = dev->hid_in_buf;
	int err;

	switch (urb->status) {
	case 0:
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
				     "HID IN URB overflow\n");
		goto resubmit;
	case -EPIPE:
		dev_warn_ratelimited(&dev->intf->dev,
				     "HID IN URB stall, clearing halt\n");
		usb_clear_halt(dev->udev, urb->pipe);
		goto resubmit;
	default:
		dev_warn_ratelimited(&dev->intf->dev,
				     "HID IN URB error: %d\n", urb->status);
		goto resubmit;
	}

	if (urb->actual_length < 1)
		goto resubmit;

	/* Dispatch based on command byte */
	switch (data[0]) {
	case SL3_HID_NOTIFY_OVERLOAD:
		if (urb->actual_length >= 11) {
			memcpy(dev->overload_status, &data[5], 6);
			if (dev->card && dev->overload_ctl)
				snd_ctl_notify(dev->card,
					       SNDRV_CTL_EVENT_MASK_VALUE,
					       &dev->overload_ctl->id);
		}
		break;
	case SL3_HID_NOTIFY_PHONO:
		if (urb->actual_length >= 8) {
			memcpy(dev->phono_status, &data[5], 3);
			if (dev->card && dev->phono_ctl)
				snd_ctl_notify(dev->card,
					       SNDRV_CTL_EVENT_MASK_VALUE,
					       &dev->phono_ctl->id);
		}
		break;
	case SL3_HID_NOTIFY_USB_PORT:
		if (urb->actual_length >= 9)
			memcpy(dev->usb_port_status, &data[5], 4);
		break;
	default:
		/* Command response: copy to response buffer and wake waiter */
		memcpy(dev->hid_response_buf, data,
		       min_t(int, urb->actual_length, SL3_HID_REPORT_SIZE));
		complete(&dev->hid_response_complete);
		break;
	}

resubmit:
	if (!dev->disconnected) {
		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (err && err != -ENODEV)
			dev_err(&dev->intf->dev,
				"HID IN URB resubmit failed: %d\n", err);
	}
}

/*
 * Send a HID command. Caller must hold hid_mutex.
 * If wait_response is true, blocks until a response arrives or timeout.
 */
static int sl3_hid_send_cmd_locked(struct sl3_device *dev, u8 cmd,
				   const u8 *payload, int payload_len,
				   bool wait_response)
{
	int actual_len;
	unsigned long remaining;
	int err;

	if (dev->disconnected)
		return -ENODEV;

	/* Use the kmalloc'd output buffer (DMA-safe, unlike stack memory) */
	sl3_hid_build_report(dev->hid_out_buf, cmd, payload, payload_len);

	if (wait_response)
		reinit_completion(&dev->hid_response_complete);

	err = usb_interrupt_msg(dev->udev,
				usb_sndintpipe(dev->udev, SL3_EP_HID_OUT),
				dev->hid_out_buf, SL3_HID_REPORT_SIZE,
				&actual_len, SL3_HID_USB_TIMEOUT_MS);
	if (err) {
		dev_err(&dev->intf->dev,
			"HID send cmd 0x%02x failed: %d\n", cmd, err);
		return err;
	}

	if (wait_response) {
		remaining = wait_for_completion_timeout(
				&dev->hid_response_complete,
				msecs_to_jiffies(SL3_HID_RESP_TIMEOUT_MS));
		if (!remaining) {
			dev_warn(&dev->intf->dev,
				 "HID cmd 0x%02x response timeout\n", cmd);
			return -ETIMEDOUT;
		}
	}

	return 0;
}

/* Send a HID command and wait for the device response. */
int sl3_hid_send_command(struct sl3_device *dev, u8 cmd,
			 u8 *payload, int payload_len)
{
	int err;

	mutex_lock(&dev->hid_mutex);
	err = sl3_hid_send_cmd_locked(dev, cmd, payload, payload_len, true);
	mutex_unlock(&dev->hid_mutex);

	return err;
}

/* Send the HID command to switch the device sample rate. */
int sl3_hid_set_sample_rate(struct sl3_device *dev, unsigned int rate)
{
	u8 payload[2];
	int err;

	if (rate != 44100 && rate != 48000)
		return -EINVAL;

	/* Rate encoded big-endian (confirmed by assembly analysis) */
	payload[0] = (rate >> 8) & 0xff;
	payload[1] = rate & 0xff;

	mutex_lock(&dev->hid_mutex);
	err = sl3_hid_send_cmd_locked(dev, SL3_HID_CMD_SAMPLE_RATE,
				      payload, sizeof(payload), true);
	if (!err)
		dev->current_rate = rate;
	mutex_unlock(&dev->hid_mutex);

	return err;
}

/* Send the HID command to set output routing for a channel pair. */
int sl3_hid_set_routing(struct sl3_device *dev, int pair, int mode)
{
	u8 payload[3];
	int err;

	payload[0] = pair;	/* Channel pair ID: 0x08, 0x0E, or 0x14 */
	payload[1] = 0x01;	/* Sub-command type (observed constant) */
	payload[2] = mode;	/* 0x00 = analog, 0x01 = USB */

	mutex_lock(&dev->hid_mutex);
	err = sl3_hid_send_cmd_locked(dev, SL3_HID_CMD_ROUTING,
				      payload, sizeof(payload), false);
	mutex_unlock(&dev->hid_mutex);

	return err;
}

/* Query phono/line switch state for all three channel pairs. */
int sl3_hid_query_phono(struct sl3_device *dev)
{
	int err;

	mutex_lock(&dev->hid_mutex);
	err = sl3_hid_send_cmd_locked(dev, SL3_HID_CMD_QUERY_PHONO,
				      NULL, 0, true);
	if (!err)
		memcpy(dev->phono_status, &dev->hid_response_buf[5], 3);
	mutex_unlock(&dev->hid_mutex);

	return err;
}

/* Initialize the HID subsystem: allocate URBs, run the init handshake. */
int sl3_hid_init(struct sl3_device *dev)
{
	u8 payload[2];
	int err;

	/* Allocate DMA-safe buffer for HID OUT (send) transfers */
	dev->hid_out_buf = kmalloc(SL3_HID_REPORT_SIZE, GFP_KERNEL);
	if (!dev->hid_out_buf)
		return -ENOMEM;

	/* Allocate HID IN URB */
	dev->hid_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->hid_in_urb) {
		err = -ENOMEM;
		goto err_free_out_buf;
	}

	/* Allocate DMA-coherent buffer for HID IN data */
	dev->hid_in_buf = usb_alloc_coherent(dev->udev, SL3_HID_REPORT_SIZE,
					     GFP_KERNEL, &dev->hid_in_dma);
	if (!dev->hid_in_buf) {
		err = -ENOMEM;
		goto err_free_urb;
	}

	/* Fill HID IN URB as interrupt URB targeting EP 0x81 */
	usb_fill_int_urb(dev->hid_in_urb, dev->udev,
			 usb_rcvintpipe(dev->udev,
					SL3_EP_HID_IN & USB_ENDPOINT_NUMBER_MASK),
			 dev->hid_in_buf, SL3_HID_REPORT_SIZE,
			 sl3_hid_in_complete, dev, 1);
	dev->hid_in_urb->transfer_dma = dev->hid_in_dma;
	dev->hid_in_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	/* Submit the persistent HID IN URB to receive responses/notifications */
	err = usb_submit_urb(dev->hid_in_urb, GFP_KERNEL);
	if (err) {
		dev_err(&dev->intf->dev,
			"failed to submit HID IN URB: %d\n", err);
		goto err_free_buf;
	}

	mutex_lock(&dev->hid_mutex);

	/* Step 1: Send CMD_INIT_QUERY (0x03), payload byte 5 = 0x00 */
	payload[0] = 0x00;
	err = sl3_hid_send_cmd_locked(dev, SL3_HID_CMD_INIT,
				      payload, 1, true);
	if (err)
		dev_warn(&dev->intf->dev,
			 "HID init query failed: %d (continuing)\n", err);

	/* Step 2: Send CMD_STATUS_QUERY (0x36), payload byte 5 = 0x01 */
	payload[0] = 0x01;
	err = sl3_hid_send_cmd_locked(dev, SL3_HID_CMD_STATUS,
				      payload, 1, true);
	if (err)
		dev_warn(&dev->intf->dev,
			 "HID status query failed: %d (continuing)\n", err);

	/* Step 3: Send CMD_SET_SAMPLE_RATE (0x31) with current rate */
	payload[0] = (dev->current_rate >> 8) & 0xff;
	payload[1] = dev->current_rate & 0xff;
	err = sl3_hid_send_cmd_locked(dev, SL3_HID_CMD_SAMPLE_RATE,
				      payload, 2, true);
	if (err)
		dev_warn(&dev->intf->dev,
			 "HID set sample rate failed: %d (continuing)\n", err);

	/* Step 4: Query initial phono/line switch positions (0x32) */
	err = sl3_hid_send_cmd_locked(dev, SL3_HID_CMD_QUERY_PHONO,
				      NULL, 0, true);
	if (!err)
		memcpy(dev->phono_status, &dev->hid_response_buf[5], 3);
	else
		dev_warn(&dev->intf->dev,
			 "HID phono query failed: %d (continuing)\n", err);

	mutex_unlock(&dev->hid_mutex);

	/* Wait for device stabilization */
	msleep(100);

	dev_info(&dev->intf->dev, "HID interface initialized\n");
	return 0;

err_free_buf:
	usb_free_coherent(dev->udev, SL3_HID_REPORT_SIZE,
			  dev->hid_in_buf, dev->hid_in_dma);
	dev->hid_in_buf = NULL;
err_free_urb:
	usb_free_urb(dev->hid_in_urb);
	dev->hid_in_urb = NULL;
err_free_out_buf:
	kfree(dev->hid_out_buf);
	dev->hid_out_buf = NULL;
	return err;
}

/* Tear down the HID subsystem: kill URBs and free buffers. */
void sl3_hid_cleanup(struct sl3_device *dev)
{
	if (dev->hid_in_urb) {
		usb_kill_urb(dev->hid_in_urb);
		usb_free_coherent(dev->udev, SL3_HID_REPORT_SIZE,
				  dev->hid_in_buf, dev->hid_in_dma);
		usb_free_urb(dev->hid_in_urb);
		dev->hid_in_urb = NULL;
		dev->hid_in_buf = NULL;
	}
	kfree(dev->hid_out_buf);
	dev->hid_out_buf = NULL;
}
