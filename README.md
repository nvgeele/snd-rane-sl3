# snd-rane-sl3

A Linux ALSA kernel driver for the Rane SL3 USB audio interface: `snd-rane-sl3`.

This project provides a native Linux kernel module that enables the Rane SL3 to work as a standard ALSA audio device, supporting 6 channels (3 stereo pairs) of 24-bit PCM audio at 44.1kHz and 48kHz sample rates.

## Hardware Details

- **Device:** Rane SL3 USB Audio Interface
- **Vendor ID:** `0x1CC5`
- **Product ID:** `0x0001`
- **Audio:** 6 channels (3 stereo pairs), 24-bit PCM, 44.1/48kHz
- **USB Interfaces:** Audio Control, Audio Streaming In/Out, HID Control

## Building the Kernel Module

### Prerequisites

- Linux kernel headers for your running kernel
- Build tools (gcc, make)

Install prerequisites on Debian/Ubuntu:
```bash
sudo apt install build-essential linux-headers-$(uname -r)
```

Install prerequisites on Fedora/RHEL:
```bash
sudo dnf install kernel-devel kernel-headers gcc make
```

Install prerequisites on Arch Linux:
```bash
sudo pacman -S linux-headers base-devel
```

### Building

Navigate to the kernel module directory and build:

```bash
cd snd-rane-sl3
make
```

This will generate `snd-rane-sl3.ko` - the compiled kernel module.

To clean build artifacts:
```bash
make clean
```

## Installing the Kernel Module

### Manual Installation (for testing)

Load the module immediately without installing:
```bash
sudo insmod snd-rane-sl3.ko
```

Unload the module:
```bash
sudo rmmod snd-rane-sl3
```

### Permanent Installation (loads at boot)

#### 1. Install the module to the kernel modules directory

```bash
# Copy the compiled .ko file to the appropriate location
sudo cp snd-rane-sl3.ko /lib/modules/$(uname -r)/kernel/sound/usb/

# Update module dependencies
sudo depmod -a
```

#### 2. Configure automatic loading at boot

**Option A: Using modules-load.d (recommended for modern systems)**
```bash
echo "snd-rane-sl3" | sudo tee /etc/modules-load.d/snd-rane-sl3.conf
```

**Option B: Using /etc/modules (traditional method)**
```bash
echo "snd-rane-sl3" | sudo tee -a /etc/modules
```

#### 3. (Optional) Set module parameters

If you need to pass parameters to the module:
```bash
sudo nano /etc/modprobe.d/snd-rane-sl3.conf

# Add options, for example:
# options snd-rane-sl3 index=0 enable=1
```

#### 4. Load the module immediately (without rebooting)

```bash
sudo modprobe snd-rane-sl3
```

#### 5. Verify the module is loaded

```bash
# Check if module is loaded
lsmod | grep snd_rane_sl3

# Check kernel logs for messages
dmesg | tail -20

# List ALSA devices
aplay -l
arecord -l
```

### DKMS Installation (Recommended for production)

DKMS (Dynamic Kernel Module Support) automatically rebuilds the module when the kernel is updated:

```bash
# Install DKMS
sudo apt install dkms  # Debian/Ubuntu
# or
sudo dnf install dkms  # Fedora

# Copy source to DKMS tree
sudo mkdir -p /usr/src/snd-rane-sl3-1.0
sudo cp -r snd-rane-sl3/* /usr/src/snd-rane-sl3-1.0/

# Create dkms.conf file
cat << 'EOF' | sudo tee /usr/src/snd-rane-sl3-1.0/dkms.conf
PACKAGE_NAME="snd-rane-sl3"
PACKAGE_VERSION="1.0"
BUILT_MODULE_NAME[0]="snd-rane-sl3"
DEST_MODULE_LOCATION[0]="/kernel/sound/usb"
AUTOINSTALL="yes"
EOF

# Add, build, and install the module with DKMS
sudo dkms add -m snd-rane-sl3 -v 1.0
sudo dkms build -m snd-rane-sl3 -v 1.0
sudo dkms install -m snd-rane-sl3 -v 1.0
```

## Usage

Once the module is loaded and the Rane SL3 is connected, it will appear as a standard ALSA audio device:

```bash
# List playback devices
aplay -l

# List capture devices
arecord -l

# Play audio to the device
aplay -D hw:CARD=SL3,DEV=0 audiofile.wav

# Record audio from the device
arecord -D hw:CARD=SL3,DEV=0 -f S24_3LE -r 44100 -c 6 output.wav
```

## Troubleshooting

If the device isn't recognized:

1. Check if the module is loaded: `lsmod | grep snd_rane_sl3`
2. Check kernel logs for errors: `dmesg | grep -i rane`
3. Verify USB device is detected: `lsusb | grep 1CC5`
4. Try unloading and reloading: `sudo rmmod snd-rane-sl3 && sudo modprobe snd-rane-sl3`

## License

This project is licensed under the GNU General Public License v3.0 - see the [LICENSE](LICENSE) file for details.

This means:
- You can use, modify, and distribute this software freely
- Any derivative works must also be licensed under GPL-3.0
- The source code must be made available to users
- There is no warranty provided with this software
