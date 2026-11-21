package org.lineageos.blackberry.inputdevices;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/**
 * Metadata-only provider for BlackBerry physical-keyboard layouts.
 *
 * KeyboardLayoutManager discovers the receiver through PackageManager
 * and reads android.hardware.input.metadata.KEYBOARD_LAYOUTS. No broadcast
 * processing is required.
 */
public final class InputDeviceReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        // Intentionally empty.
    }
}
