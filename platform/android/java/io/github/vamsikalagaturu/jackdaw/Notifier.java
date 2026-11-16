package io.github.vamsikalagaturu.jackdaw;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;

final class Notifier {
    static final String CHANNEL = "mail";
    static final String EXTRA_MESSAGE = "io.github.vamsikalagaturu.jackdaw.MESSAGE";

    private Notifier() {}

    /* by name: the build makes no R class, and a dev build renames the package, not its resources */
    private static int icon(Context ctx) {
        for (String pkg : new String[] {ctx.getPackageName(), "io.github.vamsikalagaturu.jackdaw"}) {
            int id = ctx.getResources().getIdentifier("ic_notification", "drawable", pkg);
            if (id != 0) return id;
        }
        return android.R.drawable.ic_dialog_email;
    }

    static final String PUSH_CHANNEL = "push";

    static void channel(Context ctx) {
        NotificationManager nm = ctx.getSystemService(NotificationManager.class);
        if (nm == null) return;
        if (nm.getNotificationChannel(CHANNEL) == null) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, "New mail", NotificationManager.IMPORTANCE_DEFAULT);
            ch.setDescription("Mail that arrives in the Inbox");
            nm.createNotificationChannel(ch);
        }
        if (nm.getNotificationChannel(PUSH_CHANNEL) == null) {
            NotificationChannel ch = new NotificationChannel(PUSH_CHANNEL, "Push", NotificationManager.IMPORTANCE_LOW);
            ch.setDescription("Shown while jackdaw listens for new mail. Turn it off to hide it; listening goes on.");
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }
    }

    /* Android requires one for a foreground service */
    static Notification status(Context ctx) {
        Intent open = new Intent(ctx, JackdawActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        PendingIntent tap = PendingIntent.getActivity(ctx, 0, open, PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(ctx, PUSH_CHANNEL)
            .setSmallIcon(icon(ctx))
            .setContentTitle("Listening for new mail")
            .setContentIntent(tap)
            .setOngoing(true)
            .setShowWhen(false)
            .build();
    }

    /* one per message id; false when notifications are turned off */
    static boolean post(Context ctx, String title, String text, long message) {
        NotificationManager nm = ctx.getSystemService(NotificationManager.class);
        if (nm == null || !nm.areNotificationsEnabled()) return false;
        channel(ctx);
        Intent open = new Intent(ctx, JackdawActivity.class)
                          .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP)
                          .putExtra(EXTRA_MESSAGE, message);
        int id = (int) (message ^ (message >>> 32));
        PendingIntent tap = PendingIntent.getActivity(ctx, id, open,
                                                      PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification n = new Notification.Builder(ctx, CHANNEL)
                             .setSmallIcon(icon(ctx))
                             .setContentTitle(title)
                             .setContentText(text)
                             .setStyle(new Notification.BigTextStyle().bigText(text))
                             .setCategory(Notification.CATEGORY_EMAIL)
                             .setContentIntent(tap)
                             .setAutoCancel(true)
                             .build();
        nm.notify(id, n);
        return true;
    }
}
