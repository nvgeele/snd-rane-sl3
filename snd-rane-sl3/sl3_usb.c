// SPDX-License-Identifier: GPL-3.0
/*
 * Rane SL3 USB Audio Interface - ALSA Driver
 *
 * USB driver registration, probe, and disconnect
 */

#include <linux/module.h>
#include <linux/usb.h>

#include "sl3.h"

static int default_sample_rate = 48000;
module_param(default_sample_rate, int, 0444);
MODULE_PARM_DESC(default_sample_rate,
		 "Default sample rate (44100 or 48000, default 48000)");

static struct usb_driver sl3_usb_driver;

static const struct usb_device_id sl3_id_table[] = {
	{ USB_DEVICE(SL3_VENDOR_ID, SL3_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, sl3_id_table);

static int sl3_probe(struct usb_interface *intf,
		     const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct sl3_device *dev;
	struct usb_interface *iface;
	int intf_num;
	int err;

	intf_num = intf->cur_altsetting->desc.bInterfaceNumber;

	/* Only bind once via interface 0 (audio control) */
	if (intf_num != SL3_INTF_AUDIO_CTRL)
		return -ENODEV;

	dev_info(&intf->dev,
		 "Rane SL3 probe: VID=%04x PID=%04x\n",
		 le16_to_cpu(udev->descriptor.idVendor),
		 le16_to_cpu(udev->descriptor.idProduct));

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->udev = usb_get_dev(udev);
	dev->intf = intf;

	/* Initialize synchronization primitives */
	mutex_init(&dev->hid_mutex);
	mutex_init(&dev->stream_mutex);
	spin_lock_init(&dev->feedback_lock);
	spin_lock_init(&dev->playback.lock);
	spin_lock_init(&dev->capture.lock);
	init_completion(&dev->hid_response_complete);
	atomic64_set(&dev->play_urbs_completed, 0);
	atomic64_set(&dev->cap_urbs_completed, 0);
	atomic_set(&dev->play_underruns, 0);
	atomic_set(&dev->cap_overruns, 0);
	atomic_set(&dev->discontinuities, 0);

	/* Claim interfaces 1 (audio out), 2 (audio in), 3 (HID) */
	iface = usb_ifnum_to_if(udev, SL3_INTF_AUDIO_OUT);
	if (!iface) {
		dev_err(&intf->dev, "interface %d not found\n",
			SL3_INTF_AUDIO_OUT);
		err = -ENODEV;
		goto err_put_dev;
	}
	err = usb_driver_claim_interface(&sl3_usb_driver, iface, dev);
	if (err) {
		dev_err(&intf->dev, "failed to claim interface %d: %d\n",
			SL3_INTF_AUDIO_OUT, err);
		goto err_put_dev;
	}

	iface = usb_ifnum_to_if(udev, SL3_INTF_AUDIO_IN);
	if (!iface) {
		dev_err(&intf->dev, "interface %d not found\n",
			SL3_INTF_AUDIO_IN);
		err = -ENODEV;
		goto err_release_intf1;
	}
	err = usb_driver_claim_interface(&sl3_usb_driver, iface, dev);
	if (err) {
		dev_err(&intf->dev, "failed to claim interface %d: %d\n",
			SL3_INTF_AUDIO_IN, err);
		goto err_release_intf1;
	}

	iface = usb_ifnum_to_if(udev, SL3_INTF_HID);
	if (!iface) {
		dev_err(&intf->dev, "interface %d not found\n",
			SL3_INTF_HID);
		err = -ENODEV;
		goto err_release_intf2;
	}
	err = usb_driver_claim_interface(&sl3_usb_driver, iface, dev);
	if (err) {
		dev_err(&intf->dev, "failed to claim interface %d: %d\n",
			SL3_INTF_HID, err);
		goto err_release_intf2;
	}

	/* Set alt settings for audio streaming interfaces */
	err = usb_set_interface(udev, SL3_INTF_AUDIO_OUT, 1);
	if (err) {
		dev_err(&intf->dev,
			"failed to set interface %d alt setting 1: %d\n",
			SL3_INTF_AUDIO_OUT, err);
		goto err_release_intf3;
	}

	err = usb_set_interface(udev, SL3_INTF_AUDIO_IN, 1);
	if (err) {
		dev_err(&intf->dev,
			"failed to set interface %d alt setting 1: %d\n",
			SL3_INTF_AUDIO_IN, err);
		goto err_reset_intf1;
	}

	/* Set default configuration */
	dev->current_rate = default_sample_rate;
	dev->routing[0] = SL3_ROUTE_USB;
	dev->routing[1] = SL3_ROUTE_USB;
	dev->routing[2] = SL3_ROUTE_USB;

	usb_set_intfdata(intf, dev);

	/* Initialize HID command interface */
	err = sl3_hid_init(dev);
	if (err) {
		dev_err(&intf->dev, "HID init failed: %d\n", err);
		goto err_clear_intfdata;
	}

	/* Allocate isochronous URBs for audio streaming */
	err = sl3_urb_alloc(dev, &dev->playback,
			    usb_sndisocpipe(udev, SL3_EP_AUDIO_OUT));
	if (err) {
		dev_err(&intf->dev, "playback URB alloc failed: %d\n", err);
		goto err_hid_cleanup;
	}

	err = sl3_urb_alloc(dev, &dev->capture,
			    usb_rcvisocpipe(udev,
					    SL3_EP_AUDIO_IN &
					    USB_ENDPOINT_NUMBER_MASK));
	if (err) {
		dev_err(&intf->dev, "capture URB alloc failed: %d\n", err);
		goto err_free_play_urbs;
	}

	/* Register ALSA sound card and PCM device */
	err = sl3_pcm_init(dev);
	if (err) {
		dev_err(&intf->dev, "PCM init failed: %d\n", err);
		goto err_free_cap_urbs;
	}

	/* Register ALSA mixer controls */
	err = sl3_control_init(dev);
	if (err) {
		dev_err(&intf->dev, "control init failed: %d\n", err);
		goto err_card_free;
	}

	/* Create proc filesystem entries */
	sl3_proc_init(dev);

	err = snd_card_register(dev->card);
	if (err) {
		dev_err(&intf->dev, "card register failed: %d\n", err);
		goto err_card_free;
	}

	dev_info(&intf->dev,
		 "Rane SL3 probe successful (rate=%u)\n",
		 dev->current_rate);
	return 0;

err_card_free:
	/* Prevent private_free from kfree'ing dev; we do it in err_put_dev */
	dev->card->private_free = NULL;
	snd_card_free(dev->card);
	dev->card = NULL;
err_free_cap_urbs:
	sl3_urb_free(dev, &dev->capture);
err_free_play_urbs:
	sl3_urb_free(dev, &dev->playback);
err_hid_cleanup:
	sl3_hid_cleanup(dev);
err_clear_intfdata:
	usb_set_intfdata(intf, NULL);
	usb_set_interface(udev, SL3_INTF_AUDIO_IN, 0);
err_reset_intf1:
	usb_set_interface(udev, SL3_INTF_AUDIO_OUT, 0);
err_release_intf3:
	iface = usb_ifnum_to_if(udev, SL3_INTF_HID);
	if (iface)
		usb_driver_release_interface(&sl3_usb_driver, iface);
err_release_intf2:
	iface = usb_ifnum_to_if(udev, SL3_INTF_AUDIO_IN);
	if (iface)
		usb_driver_release_interface(&sl3_usb_driver, iface);
err_release_intf1:
	iface = usb_ifnum_to_if(udev, SL3_INTF_AUDIO_OUT);
	if (iface)
		usb_driver_release_interface(&sl3_usb_driver, iface);
err_put_dev:
	usb_put_dev(dev->udev);
	kfree(dev);
	return err;
}

static void sl3_disconnect(struct usb_interface *intf)
{
	struct sl3_device *dev = usb_get_intfdata(intf);
	struct usb_interface *iface;
	int intf_num;

	if (!dev)
		return;

	intf_num = intf->cur_altsetting->desc.bInterfaceNumber;

	/* Only handle full teardown from interface 0 */
	if (intf_num != SL3_INTF_AUDIO_CTRL)
		return;

	dev_info(&intf->dev, "Rane SL3 disconnecting\n");

	dev->disconnected = true;

	/* Disconnect the ALSA card (makes it inaccessible to userspace) */
	if (dev->card)
		snd_card_disconnect(dev->card);

	/* Stop and free audio URBs */
	sl3_urb_stop(dev, &dev->playback);
	sl3_urb_stop(dev, &dev->capture);
	sl3_urb_free(dev, &dev->playback);
	sl3_urb_free(dev, &dev->capture);

	/* Clean up HID interface before releasing USB interfaces */
	sl3_hid_cleanup(dev);

	/* Reset alt settings on audio streaming interfaces */
	usb_set_interface(dev->udev, SL3_INTF_AUDIO_OUT, 0);
	usb_set_interface(dev->udev, SL3_INTF_AUDIO_IN, 0);

	/* Release claimed interfaces */
	iface = usb_ifnum_to_if(dev->udev, SL3_INTF_HID);
	if (iface)
		usb_driver_release_interface(&sl3_usb_driver, iface);

	iface = usb_ifnum_to_if(dev->udev, SL3_INTF_AUDIO_IN);
	if (iface)
		usb_driver_release_interface(&sl3_usb_driver, iface);

	iface = usb_ifnum_to_if(dev->udev, SL3_INTF_AUDIO_OUT);
	if (iface)
		usb_driver_release_interface(&sl3_usb_driver, iface);

	usb_set_intfdata(intf, NULL);
	usb_put_dev(dev->udev);

	/* Free card when userspace closes all handles.
	 * card->private_free will kfree(dev). */
	if (dev->card)
		snd_card_free_when_closed(dev->card);
	else
		kfree(dev);

	dev_info(&intf->dev, "Rane SL3 disconnected\n");
}

static struct usb_driver sl3_usb_driver = {
	.name		= "snd_rane_sl3",
	.id_table	= sl3_id_table,
	.probe		= sl3_probe,
	.disconnect	= sl3_disconnect,
};

module_usb_driver(sl3_usb_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nils Van Geele");
MODULE_DESCRIPTION("ALSA driver for the Rane SL3 USB audio interface");
