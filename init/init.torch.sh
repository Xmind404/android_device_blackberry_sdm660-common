#!/vendor/bin/sh
#
# SPDX-FileCopyrightText: 2026 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# Apply the dual-tone torch mix chosen in Quick Settings. SystemUI changes
# vendor.torch.apply only while the torch is on, after the camera HAL has
# switched it on, so this overrides the HAL's per-LED currents.
#
# Levels are 0-100 % of each LED's max_brightness: torch_0 is the white LED,
# torch_1 the warm LED. led:switch_0 gates both rear torch LEDs.

level() {
    case "$1" in
        ''|*[!0-9]*) echo 50 ;;
        *) if [ "$1" -gt 100 ]; then echo 100; else echo "$1"; fi ;;
    esac
}

set_led() {
    led=/sys/class/leds/$1
    [ -w "$led/brightness" ] || return 0
    max=$(cat "$led/max_brightness")
    echo $((max * $2 / 100)) > "$led/brightness"
}

WHITE=$(level "$(getprop persist.vendor.torch.white_level)")
WARM=$(level "$(getprop persist.vendor.torch.warm_level)")

set_led led:torch_0 "$WHITE"
set_led led:torch_1 "$WARM"

if [ "$WHITE" -gt 0 ] || [ "$WARM" -gt 0 ]; then
    echo 1 > /sys/class/leds/led:switch_0/brightness
else
    echo 0 > /sys/class/leds/led:switch_0/brightness
fi
