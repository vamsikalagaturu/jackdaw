package io.github.vamsikalagaturu.jackdaw;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context ctx, Intent intent) {
        String a = intent.getAction();
        if (!Intent.ACTION_BOOT_COMPLETED.equals(a) && !Intent.ACTION_MY_PACKAGE_REPLACED.equals(a)) return;
        if (PushService.wanted(ctx)) PushService.set(ctx, true);
        /* alarms do not outlive a reboot: the check sets SendAlarm again */
        MailJobService.sendNow(ctx);
    }
}
