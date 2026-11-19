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

    /* every account's address, one a line, from the settings in files; null without an account */
    private static native String nativeAccounts(String files);

    static String[] accounts(String files) {
        String all = nativeAccounts(files);
        return all == null ? new String[0] : all.split("\n");
    }
    /* account's lines, one per notification, "tag\ttitle\ttext"; null when notifications are off or it failed */
    private static native String nativeCheck(String files, String account, String password);
    private static native long nativeNextSend(String files);

    /* a scheduled draft due (SendAlarm), or after a reboot, which forgets alarms */
    private static final int SEND_JOB_ID = 2;

    private Thread worker;

    /* Expedited, as Doze defers other jobs; the alarm's own short window is too little for a sync and a send. */
    static void sendNow(Context ctx) {
        JobScheduler js = ctx.getSystemService(JobScheduler.class);
        if (js == null) return;
        JobInfo.Builder b = new JobInfo.Builder(SEND_JOB_ID, new ComponentName(ctx, MailJobService.class))
                                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY);
        try {
            if (android.os.Build.VERSION.SDK_INT >= 31 && js.schedule(b.setExpedited(true).build()) == JobScheduler.RESULT_SUCCESS)
                return;
            js.schedule(b.setExpedited(false).build());
        } catch (RuntimeException e) {
            /* the open app sends it */
        }
    }

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
        boolean due = params.getJobId() == SEND_JOB_ID;
        /* A due draft is sent beside the app's or push's own sync, which may be frozen or asleep: the claim on
           the server lets only one of them send it. Only an app on screen is sure to be awake. */
        if (due ? PushService.appVisible : JackdawActivity.running || PushService.running) return false;
        /* push died with its process; its own first sync replaces this check */
        if (!due && PushService.wanted(this) && PushService.set(this, true)) return false;
        worker = new Thread(() -> {
            try {
                System.loadLibrary("jackdaw");
                String files = getFilesDir().getAbsolutePath();
                /* every account in turn; the tags of the lines say whose */
                for (String account : accounts(files)) {
                    String password = JackdawActivity.passwordFor(getFilesDir(), account);
                    String found = password == null ? null : nativeCheck(files, account, password);
                    if (found != null)
                        for (String line : found.split("\n")) {
                            String[] f = line.split("\t", 3);
                            if (f.length == 3) Notifier.post(this, f[1], f[2], Long.parseLong(f[0]));
                        }
                }
                SendAlarm.set(this, nativeNextSend(files));
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
