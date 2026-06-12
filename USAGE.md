# Usage

**DISCLAIMER:** I recommend reading this entire document so you are able to use this kernel driver correctly. Incorrect usage may damage your machine.

## Where are the nodes?

All available nodes are found at the following path:
```
/sys/devices/platform/aorus_laptop
```
You can write to these nodes using `echo` and `tee`. Keep in mind that you must be logged in as `root` or using `sudo` for this.

## Fan modes

Aero/AORUS laptops currently support six fan modes. They are implemented in the kernel driver and recognized in the following order, starting from zero:
- Normal mode
- Silent mode
- Gaming mode
- Custom mode
- Auto mode
- Fixed mode

The last two modes will enable custom mode automatically, as they are considered "custom modes". Custom mode will be automatically disabled if the first three modes are enabled.

**Node:** `/sys/devices/platform/aorus_laptop/fan_mode`

**Example:** To switch to gaming fan mode:
```
echo '2' | sudo tee /sys/devices/platform/aorus_laptop/fan_mode
```

## Custom fan speed

Aero/AORUS laptops support setting a custom fan speed. However, this only takes effect if either auto or fixed mode is enabled.

The kernel driver only supports numbers with the following requirements:
- It is between 25 and 100.
- It is divisible by 5.

**Node:** `/sys/devices/platform/aorus_laptop/fan_custom_speed`

**Example:** To set the custom fan speed to 50 percent:
```
echo '50' | sudo tee /sys/devices/platform/aorus_laptop/fan_custom_speed
```

## Charging mode

**Disclaimer:** Charging mode (and limit) is not supported on the following models:
* [Aero 14-W/K](https://www.gigabyte.com/Laptop/AERO-14--GTX-970M-965M)
* [Aero 14-W6](https://www.gigabyte.com/Laptop/AERO-14--GTX-1060)
* [Aero 14-W7](https://www.gigabyte.com/Laptop/AERO-14--i7-7700HQ)

Aero/AORUS laptops support two charging modes: Normal (0) and custom (1). The custom charging mode simply stops the laptop from passing its charging limit.

**Node:** `/sys/devices/platform/aorus_laptop/charge_mode`

## Charging limit

Aero/AORUS laptops support a charging limit. Charging mode must be set to custom for it to take effect. It will only accept numbers between 60 and 100.

**Node:** `/sys/devices/platform/aorus_laptop/charge_limit`

**Example:** To set the charging limit to 80 percent:
```
echo '80' | sudo tee /sys/devices/platform/aorus_laptop/charge_limit
```

## Fan curve data (added in version 0.1.0)

Aero/AORUS laptops support setting a custom fan curve. Custom fan mode must be enabled for it to take effect.

**Nodes:**
```
/sys/devices/platform/aorus_laptop/fan_curve_index
/sys/devices/platform/aorus_laptop/fan_curve_data
```

The index node specifies which index in the fan curve can be modified, while the data node holds the temperature (in Celsius) and fan speed for that index, printed in that order (for readability). There are 255 indices available, but only 15 can be modified. This is to ensure compatibility with Gigabyte's Control Center software on dual-boot systems.

Each index should contain both temperature and fan speed in strictly non-decreasing order. The temperature can be any number between 0 and 100, and the fan speed can be any number between 0 and 255.

You must set the index node to the index you wish to modify first. The data node can only take a single 16-bit number, so you must combine your specified temperature and fan speed first, in reverse order. An easy way to do this is to multiply the fan speed by 256, and then add the temperature to the result.

**Example:** To set index 2 to half fan speed at 55 degrees Celsius:
```
echo '2' | sudo tee /sys/devices/platform/aorus_laptop/fan_curve_index
# 127*256+55 = 32512+55 = 32567
echo '32567' | sudo tee /sys/devices/platform/aorus_laptop/fan_curve_data
```

## Battery cycle (added in version 0.1.0)

Aero/AORUS laptops support battery cycles, but are only accessible through the embedded controller. Older models are likely to read 0 due to older Gigabyte firmware. Because there are two different battery cycle numbers, only the highest one is printed. This node is read-only.

**Node:** `/sys/devices/platform/aorus_laptop/battery_cycle`

## GPU boost (added in version 0.1.0)

**Disclaimer:** Models older than the [Aero 15 X9 Series](https://www.gigabyte.com/Laptop/AERO-15--RTX-20-Series) do not support this, as it requires NVIDIA's Dynamic Boost from their Max-Q technologies.

Aero/AORUS laptops support boosting the discrete GPU's power limit. Even though this is controlled by `nvidia-powerd`, the embedded controller can control this as well.

Laptops with RTX 3000 series GPUs or newer have three different modes, while older models have one.

**Node:** `/sys/devices/platform/aorus_laptop/gpu_boost`

**Example:** To enable GPU boost:
```
echo '1' | sudo tee /sys/devices/platform/aorus_laptop/gpu_boost
```

## Performance mode (Gigabyte Gaming models only)

Gigabyte Gaming models (e.g. GAMING A16 CWH) expose the Control Center
performance modes, which program the NVIDIA platform controller (NPCF) and set
the discrete GPU's power limit (TGP). Modes:
- 0 = eco (lowest TGP)
- 1 = balanced
- 2 = boost (highest TGP)

On the A16 CWH (RTX 5070), boost mode with GPU boost enabled raises the GPU
power limit from 55W to 70W, and dynamic boost can add up to 15W more under
load (85W max).

**Node:** `/sys/devices/platform/aorus_laptop/perf_mode`

**Example:** To unlock the maximum GPU TGP:
```
echo '2' | sudo tee /sys/devices/platform/aorus_laptop/perf_mode
echo '1' | sudo tee /sys/devices/platform/aorus_laptop/gpu_boost
echo '1' | sudo tee /sys/devices/platform/aorus_laptop/dynamic_boost
```
Note: the firmware resets to its default mode on reboot, so re-apply after boot
(or use a systemd unit/udev rule).

## Dynamic boost (Gigabyte Gaming models only)

Toggles NVIDIA Dynamic Boost (the EC's `DBAC` flag), letting the GPU driver
temporarily shift extra power to the GPU under load. 0 = off, 1 = on.

**Node:** `/sys/devices/platform/aorus_laptop/dynamic_boost`

## Turbo fan (Gigabyte Gaming models only)

Runs the fans at maximum speed (~6500 RPM on the A16 CWH). The driver
engages custom fan mode at 100% duty (WMI `TENF` + max duty `0xE5`).
Disabling restores the fan mode that was active before turbo was enabled.
0 = off, 1 = on.

**Node:** `/sys/devices/platform/aorus_laptop/fan_turbo`

**Example:**
```
echo '1' | sudo tee /sys/devices/platform/aorus_laptop/fan_turbo
```

## Gaming thermal boost (Gigabyte Gaming models only)

Addresses inadequate gaming-mode fan curves on models like the A16 CWH,
where GPU power can drop from 70W toward 55–60W under load because fans
stay near ~3000 RPM. While `fan_mode` is gaming (2), the driver polls GPU
temperature and fan RPM every two seconds and escalates custom fan duty
in steps (70% / 80% / 90% / 100%). If WMI GPU temperature reads low but
fans are not spinning fast enough, duty is raised anyway. When the GPU
cools below 45°C, gaming mode is restored. Mutually exclusive with
`fan_turbo`. 0 = off, 1 = on.

Requires gaming fan mode before enabling:

**Node:** `/sys/devices/platform/aorus_laptop/fan_gaming_boost`

**Example:**
```
echo '2' | sudo tee /sys/devices/platform/aorus_laptop/fan_mode
echo '1' | sudo tee /sys/devices/platform/aorus_laptop/fan_gaming_boost
```

## GPU boost on Gigabyte Gaming models

On Gigabyte Gaming models, `gpu_boost` only accepts 0 or 1 (CTGP boost off/on).
Values 2 and 3 are rejected by the driver because the firmware maps them to
dGPU eject and power-off requests on these models.

## USB toggles (added in version 0.1.0)

Aero/AORUS laptops support USB power output when they are asleep (S3) or in hibernation (S4). Newer models have dropped the latter, and will return 0 by default. These toggles are currently read-only.

**Nodes:**
```
/sys/devices/platform/aorus_laptop/usb_charge_s3_toggle
/sys/devices/platform/aorus_laptop/usb_charge_s4_toggle
```
