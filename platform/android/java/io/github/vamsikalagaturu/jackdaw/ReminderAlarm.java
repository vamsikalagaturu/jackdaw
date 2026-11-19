package io.github.vamsikalagaturu.jackdaw;

import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;

/* Calendar reminders (plat_reminders): one alarm per coming reminder, each posting its notification when it goes off,
   with the app running or not. The list is kept, so BootReceiver sets the alarms again after a reboot. */
public class ReminderAlarm extends BroadcastReceiver {
    static final String PREFS = "reminders";
    private static final String KEY_SPEC = "spec", KEY_COUNT = "count";
    private static final String EXTRA_TITLE = "title", EXTRA_TEXT = "text";

    /* spec: a line per reminder, "epochSeconds\ttitle\ttext" */
    static void set(Context ctx, String spec) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(KEY_SPEC, spec).apply();
        arm(ctx);
    }

    private static PendingIntent pending(Context ctx, int i, String title, String text) {
        Intent it = new Intent(ctx, ReminderAlarm.class).setAction("io.github.vamsikalagaturu.jackdaw.REMINDER." + i);
        if (title != null) it.putExtra(EXTRA_TITLE, title).putExtra(EXTRA_TEXT, text);
        return PendingIntent.getBroadcast(ctx, 1000 + i, it, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    static void arm(Context ctx) {
        AlarmManager am = ctx.getSystemService(AlarmManager.class);
        if (am == null) return;
        SharedPreferences p = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        int had = p.getInt(KEY_COUNT, 0);
        for (int i = 0; i < had; i++) am.cancel(pending(ctx, i, null, null));
        String spec = p.getString(KEY_SPEC, "");
        long now = System.currentTimeMillis();
        boolean exact = android.os.Build.VERSION.SDK_INT < 31 || am.canScheduleExactAlarms();
        int n = 0;
        for (String line : spec.split("\n")) {
            String[] f = line.split("\t", 3);
            if (f.length < 3) continue;
            long at;
            try {
                at = Long.parseLong(f[0]) * 1000;
            } catch (NumberFormatException e) {
                continue;
            }
            if (at < now - 60_000) continue;
            PendingIntent pi = pending(ctx, n++, f[1], f[2]);
            try {
                if (exact) am.setExactAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, at, pi);
                else am.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, at, pi);
            } catch (SecurityException e) {
                am.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, at, pi);
            }
        }
        p.edit().putInt(KEY_COUNT, n).apply();
    }

    @Override
    public void onReceive(Context ctx, Intent intent) {
        String title = intent.getStringExtra(EXTRA_TITLE);
        if (title != null) Notifier.remind(ctx, title, intent.getStringExtra(EXTRA_TEXT));
    }
}
