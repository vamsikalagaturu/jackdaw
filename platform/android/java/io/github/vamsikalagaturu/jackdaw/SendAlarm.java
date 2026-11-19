package io.github.vamsikalagaturu.jackdaw;

import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/* Wakes the phone at a scheduled draft's time, which JobScheduler's periodic check could miss by 15 minutes or,
   in Doze, by hours. Exact when the user allows exact alarms (Settings, Alarms & reminders), else as close as
   Doze lets an alarm come. */
public class SendAlarm extends BroadcastReceiver {
    static void set(Context ctx, long epochSeconds) {
        AlarmManager am = ctx.getSystemService(AlarmManager.class);
        if (am == null) return;
        PendingIntent pi = PendingIntent.getBroadcast(ctx, 0, new Intent(ctx, SendAlarm.class),
                                                      PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        if (epochSeconds <= 0) {
            am.cancel(pi);
            return;
        }
        long at = epochSeconds * 1000;
        boolean exact = android.os.Build.VERSION.SDK_INT < 31 || am.canScheduleExactAlarms();
        try {
            if (exact) am.setExactAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, at, pi);
            else am.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, at, pi);
        } catch (SecurityException e) {
            am.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, at, pi);
        }
    }

    @Override
    public void onReceive(Context ctx, Intent intent) {
        MailJobService.sendNow(ctx);
    }
}
