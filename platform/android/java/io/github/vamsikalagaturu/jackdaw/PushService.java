package io.github.vamsikalagaturu.jackdaw;

import android.app.Notification;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.IBinder;

/* Turning off the required "Push" notification channel hides it; the service keeps running. */
public class PushService extends Service {
    static final int STATUS_ID = 0x6a640001;
    static final String PREFS = "jackdaw", PREF_PUSH = "push";

    /* the app is on screen (JackdawActivity.postNewMail) */
    static volatile boolean appVisible;
    static volatile boolean running;

    private static native void nativePush(String files, String account, String password);
    private static native void nativePushStop();
    private static Context context;
    private Thread worker;

    /* kept where the boot receiver can read it without the native side */
    /* false when Android refuses the start: from the background only an app exempt from battery optimisation may */
    static boolean set(Context ctx, boolean on) {
        ctx.getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_PUSH, on).apply();
        Intent i = new Intent(ctx, PushService.class);
        try {
            if (on) ctx.startForegroundService(i);
            else ctx.stopService(i);
            return true;
        } catch (RuntimeException e) {
            return false;
        }
    }

    static boolean wanted(Context ctx) {
        return ctx.getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(PREF_PUSH, false);
    }

    /* called from nativePush */
    static void posted(String title, String text, long message) {
        Context ctx = context;
        if (ctx != null) Notifier.post(ctx, title, text, message);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        context = getApplicationContext();
        Notifier.channel(this);
        Notification status = Notifier.status(this);
        /* a START_STICKY restart after the process was killed counts as a background start */
        try {
            if (android.os.Build.VERSION.SDK_INT >= 34)
                startForeground(STATUS_ID, status, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            else
                startForeground(STATUS_ID, status);
        } catch (RuntimeException e) {
            stopSelf();
            return;
        }
        running = true;
        worker = new Thread(() -> {
            try {
                System.loadLibrary("jackdaw");
                String files = getFilesDir().getAbsolutePath();
                /* one IDLE per account, each on a thread of its own, until nativePushStop */
                java.util.List<Thread> pushes = new java.util.ArrayList<>();
                for (String account : MailJobService.accounts(files)) {
                    String password = JackdawActivity.passwordFor(getFilesDir(), account);
                    if (password == null) continue;
                    Thread t = new Thread(() -> nativePush(files, account, password), "jackdaw-push-" + account);
                    t.start();
                    pushes.add(t);
                }
                for (Thread t : pushes) t.join();
            } catch (Throwable e) {
                /* without an account or a library the service has nothing to do */
            }
            running = false;
            stopSelf();
        }, "jackdaw-push");
        worker.start();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        running = false;
        try {
            nativePushStop();
        } catch (Throwable e) {
            /* the library never loaded */
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
