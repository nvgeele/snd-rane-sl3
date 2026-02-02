obj-m := snd-rane-sl3.o
snd-rane-sl3-objs := sl3_usb.o sl3_hid.o sl3_pcm.o sl3_urb.o sl3_control.o sl3_proc.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
