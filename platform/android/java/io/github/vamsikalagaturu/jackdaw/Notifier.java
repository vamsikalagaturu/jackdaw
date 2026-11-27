package io.github.vamsikalagaturu.jackdaw;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.graphics.Typeface;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.StyleSpan;

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
    static final String REMINDER_CHANNEL = "reminders";

    /* a calendar reminder (ReminderAlarm); a tap opens jackdaw */
    static void remind(Context ctx, String title, String text) {
        NotificationManager nm = ctx.getSystemService(NotificationManager.class);
        if (nm == null || !nm.areNotificationsEnabled()) return;
        if (nm.getNotificationChannel(REMINDER_CHANNEL) == null) {
            NotificationChannel ch = new NotificationChannel(REMINDER_CHANNEL, "Calendar reminders", NotificationManager.IMPORTANCE_HIGH);
            ch.setDescription("Events of your calendars about to start");
            nm.createNotificationChannel(ch);
        }
        Intent open = new Intent(ctx, JackdawActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent tap = PendingIntent.getActivity(ctx, 2, open, PendingIntent.FLAG_IMMUTABLE);
        Notification n = new Notification.Builder(ctx, REMINDER_CHANNEL)
                             .setSmallIcon(icon(ctx))
                             .setContentTitle(title)
                             .setContentText(text)
                             .setCategory(Notification.CATEGORY_REMINDER)
                             .setContentIntent(tap)
                             .setAutoCancel(true)
                             .build();
        nm.notify((title + "\n" + text).hashCode(), n);
    }

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

    /* one per message id; false when notifications are turned off. Expanded, the text (the subject) in bold over the
       body's opening, when there is one. */
    static boolean post(Context ctx, String title, String text, String body, long message) {
        NotificationManager nm = ctx.getSystemService(NotificationManager.class);
        if (nm == null || !nm.areNotificationsEnabled()) return false;
        channel(ctx);
        Intent open = new Intent(ctx, JackdawActivity.class)
                          .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP)
                          .putExtra(EXTRA_MESSAGE, message);
        int id = (int) (message ^ (message >>> 32));
        PendingIntent tap = PendingIntent.getActivity(ctx, id, open,
                                                      PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        CharSequence expanded = text;
        if (body != null && !body.isEmpty()) {
            SpannableStringBuilder b = new SpannableStringBuilder(text);
            b.setSpan(new StyleSpan(Typeface.BOLD), 0, b.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            expanded = b.append("\n").append(body);
        }
        Notification n = new Notification.Builder(ctx, CHANNEL)
                             .setSmallIcon(icon(ctx))
                             .setContentTitle(title)
                             .setContentText(text)
                             .setStyle(new Notification.BigTextStyle().bigText(expanded))
                             .setCategory(Notification.CATEGORY_EMAIL)
                             .setContentIntent(tap)
                             .setAutoCancel(true)
                             .build();
        nm.notify(id, n);
        return true;
    }
}
