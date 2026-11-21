#!/vendor/bin/sh
# Apply the hardware family once, after persistent properties load and before
# Android opens the input device. Changes made in Settings take effect on reboot.

REQUEST=$(getprop persist.vendor.bb.keyboard.layout)
case "$REQUEST" in
    qwerty) NUM=1 ;;
    azerty) NUM=2 ;;
    qwertz) NUM=3 ;;
    *)
        REQUEST=auto
        TRACE=$(/vendor/bin/trace_util r 8 2>/dev/null)
        NUM=${TRACE%% *}
        case "$NUM" in
            1|2|3) ;;
            *) NUM=1 ;;
        esac
        ;;
esac

case "$NUM" in
    2) FAMILY=azerty ;;
    3) FAMILY=qwertz ;;
    *) FAMILY=qwerty ;;
esac

# Existing aliases cover both kernel tracks; the final pattern also handles
# differing I2C bus numbers. Only the STMPE address/control is considered.
KL_NODE=
for node in \
    /sys/devices/keypad/kl \
    /sys/devices/platform/keypad/kl \
    /sys/bus/i2c/devices/1-0040/kl \
    /sys/bus/i2c/devices/*-0040/kl
do
    if [ -w "$node" ]; then
        KL_NODE="$node"
        break
    fi
done

if [ -z "$KL_NODE" ] || ! printf '%s\n' "$NUM" > "$KL_NODE"; then
    setprop vendor.bb.keyboard.layout.applied error
    exit 1
fi

# Keep the legacy BlackBerry property consistent with the effective hardware.
if ! setprop ro.hwf.keypadlanguage "$NUM $FAMILY"; then
    setprop vendor.bb.keyboard.layout.applied error
    exit 1
fi

# Record the boot request as well as the resulting family. Settings can then
# distinguish a saved pending change from the family actually loaded at boot.
setprop vendor.bb.keyboard.layout.applied "$REQUEST:$FAMILY"
