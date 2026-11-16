package io.github.vamsikalagaturu.jackdaw;

import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;
import android.content.ComponentName;
import android.content.Context;

/* 15 minutes is JobScheduler's shortest period. */
public class MailJobService extends JobService {
    private static final int JOB_ID = 1;
    private static final long PERIOD_MS = 15 * 60 * 1000;

    /* the account's address, from the settings in files; null without an account */
    private static native String nativeAccount(String files);

    static String account(String files) { return nativeAccount(files); }
    /* a line per notification, "id\ttitle\ttext"; null when notifications are off or it failed */
    private static native String nativeCheck(String files, String password);

    private Thread worker;

    static void schedule(Context ctx) {
        try {
            JobScheduler js = ctx.getSystemService(JobScheduler.class);
            if (js == null || js.getPendingJob(JOB_ID) != null) return;
            js.schedule(new JobInfo.Builder(JOB_ID, new ComponentName(ctx, MailJobService.class))
                            .setPeriodic(PERIOD_MS)
                            .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                            .setPersisted(true)
                            .build());
        } catch (RuntimeException e) {
            /* the app still notifies while open */
        }
    }

    @Override
    public boolean onStartJob(JobParameters params) {
        if (JackdawActivity.running || PushService.running) return false;
        worker = new Thread(() -> {
            try {
                System.loadLibrary("jackdaw");
                String files = getFilesDir().getAbsolutePath();
                String account = nativeAccount(files);
                String password = account == null ? null : JackdawActivity.passwordFor(getFilesDir(), account);
                String found = password == null ? null : nativeCheck(files, password);
                if (found != null)
                    for (String line : found.split("\n")) {
                        String[] f = line.split("\t", 3);
                        if (f.length == 3) Notifier.post(this, f[1], f[2], Long.parseLong(f[0]));
                    }
            } catch (Throwable e) {
                /* a failed check waits for the next period */
            }
            jobFinished(params, false);
        }, "jackdaw-check");
        worker.start();
        return true;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        /* the native round ends at its own timeout */
        return false;
    }
}
