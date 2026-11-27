package io.github.vamsikalagaturu.jackdaw;

import android.app.NativeActivity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.database.Cursor;
import android.graphics.Insets;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.print.PageRange;
import android.print.PrintAttributes;
import android.print.PrintDocumentAdapter;
import android.print.PrintDocumentInfo;
import android.print.PrintManager;
import android.provider.OpenableColumns;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.KeyStore;
import java.util.Properties;
import java.util.concurrent.Callable;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/* The native window plus what only Java reaches: the soft keyboard's text, the clipboard, the system bar insets,
 * opening links and files in other apps, picking files to attach, and the account password (sealed under an
 * Android Keystore key). Called from the native thread (android_main.c); every public method takes and returns
 * strings, null meaning "no" or "nothing". */
public class JackdawActivity extends NativeActivity {
    private static native void nativeText(int[] codes);
    private static native void nativePicked(String paths);

    private TextSink sink;
    /* the activity exists, on screen or not: its own sync runs and the background ones stand down */
    static volatile boolean running;
    private static final int REQUEST_NOTIFY = 2;

    @Override
    protected void onCreate(Bundle state) {
        running = true;
        super.onCreate(state);
        Notifier.channel(this);
        if (android.os.Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission("android.permission.POST_NOTIFICATIONS") != android.content.pm.PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] {"android.permission.POST_NOTIFICATIONS"}, REQUEST_NOTIFY);
        MailJobService.schedule(this);
        /* the window reaches into the camera cutout; the native side keeps clear of it through systemInsets() */
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(lp);
        getWindow().setStatusBarColor(0xFF2E3440);
        getWindow().setNavigationBarColor(0xFF2E3440);
        sink = new TextSink();
        addContentView(sink, new ViewGroup.LayoutParams(1, 1));
    }

    @Override
    protected void onDestroy() {
        running = false;
        super.onDestroy();
    }

    @Override
    protected void onStart() {
        PushService.appVisible = true;
        super.onStart();
    }

    @Override
    protected void onStop() {
        PushService.appVisible = false;
        super.onStop();
    }

    /* plat_wake_at; at in epoch seconds, "0" for none */
    public String wakeAt(String at) {
        try {
            SendAlarm.set(this, Long.parseLong(at));
        } catch (NumberFormatException e) {
            return null;
        }
        return "1";
    }

    /* plat_reminders; spec as ReminderAlarm.set takes it */
    public String setReminders(String spec) {
        ReminderAlarm.set(this, spec);
        return "1";
    }

    /* plat_push; on is "1" or "0" */
    public String setPush(String on) {
        PushService.set(this, "1".equals(on));
        if ("1".equals(on)) askBatteryExemption();
        return "1";
    }

    private static final String PREF_ASKED_BATTERY = "asked_battery";

    /* Once: the exemption lets the 15-minute job restart push after Android killed the process
       (background foreground-service starts are refused otherwise). */
    private void askBatteryExemption() {
        android.content.SharedPreferences prefs = getSharedPreferences(PushService.PREFS, MODE_PRIVATE);
        android.os.PowerManager pm = getSystemService(android.os.PowerManager.class);
        if (pm == null || pm.isIgnoringBatteryOptimizations(getPackageName()) || prefs.getBoolean(PREF_ASKED_BATTERY, false))
            return;
        prefs.edit().putBoolean(PREF_ASKED_BATTERY, true).apply();
        try {
            startActivity(new Intent(android.provider.Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                                     Uri.parse("package:" + getPackageName())));
        } catch (ActivityNotFoundException e) {
            /* push still runs while the process lives */
        }
    }

    /* NativeActivity drops intents delivered to a running instance (launchMode singleTask) */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
    }

    /* ---- notifications ---- */

    /* on_new_mail, on the sync thread; whenVisible "0": not while the app is on screen */
    public String postNewMail(String title, String text, String body, String message, String whenVisible) {
        if (PushService.appVisible && "0".equals(whenVisible)) return null;
        return post(title, text, body, message);
    }

    public String postNotification(String title, String text, String message) {
        return post(title, text, "", message);
    }

    private String post(String title, String text, String body, String message) {
        try {
            return Notifier.post(this, title, text, body, Long.parseLong(message)) ? "1" : null;
        } catch (NumberFormatException e) {
            return null;
        }
    }

    /* plat_notify_poll: a tapped notification's message id, once */
    public String takeOpened() {
        Intent i = getIntent();
        long id = i == null ? 0 : i.getLongExtra(Notifier.EXTRA_MESSAGE, 0);
        if (id == 0) return null;
        i.removeExtra(Notifier.EXTRA_MESSAGE);
        return Long.toString(id);
    }

    private String onUi(Callable<String> job) {
        FutureTask<String> task = new FutureTask<>(job);
        runOnUiThread(task);
        try {
            return task.get(5, TimeUnit.SECONDS);
        } catch (Exception e) {
            return null;
        }
    }

    /* "left top right bottom" in pixels: the system bars, the display cutout and the soft keyboard (the window does
     * not resize for it: adjustNothing) */
    public String systemInsets() {
        WindowInsets w = getWindow().getDecorView().getRootWindowInsets();
        if (w == null) return null;
        Insets i = w.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
        return i.left + " " + i.top + " " + i.right + " " + i.bottom;
    }

    /* ---- links and files ---- */

    public String openUri(String uri) {
        Intent i = new Intent(Intent.ACTION_VIEW, Uri.parse(uri));
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        return onUi(() -> {
            try {
                startActivity(i);
                return "1";
            } catch (ActivityNotFoundException e) {
                return null;
            }
        });
    }

    public String shareText(String text) {
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_TEXT, text);
        Intent i = Intent.createChooser(send, null);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        return onUi(() -> {
            try {
                startActivity(i);
                return "1";
            } catch (ActivityNotFoundException e) {
                return null;
            }
        });
    }

    private java.util.List<ResolveInfo> textActions = new java.util.ArrayList<>();

    /* the apps acting on selected text (ACTION_PROCESS_TEXT), their labels one per line; null when none */
    public String textActions() {
        Intent q = new Intent(Intent.ACTION_PROCESS_TEXT).setType("text/plain");
        PackageManager pm = getPackageManager();
        textActions = pm.queryIntentActivities(q, 0);
        if (textActions.isEmpty()) return null;
        StringBuilder b = new StringBuilder();
        for (ResolveInfo r : textActions) b.append(r.loadLabel(pm).toString().replace('\n', ' ')).append('\n');
        return b.toString();
    }

    /* index: a line of what textActions() returned */
    public String textAction(String index, String text) {
        int i = Integer.parseInt(index);
        if (i < 0 || i >= textActions.size()) return null;
        ActivityInfo a = textActions.get(i).activityInfo;
        Intent intent = new Intent(Intent.ACTION_PROCESS_TEXT).setType("text/plain")
            .setClassName(a.packageName, a.name)
            .putExtra(Intent.EXTRA_PROCESS_TEXT, text)
            .putExtra(Intent.EXTRA_PROCESS_TEXT_READONLY, true);
        return onUi(() -> {
            try {
                startActivity(intent);
                return "1";
            } catch (ActivityNotFoundException | SecurityException e) {
                return null;
            }
        });
    }

    /* path in the app the system picks for its type, through OpenProvider; null when no app takes it */
    public String openFile(String path) {
        Uri uri = OpenProvider.uriFor(getPackageName(), path);
        Intent i = new Intent(Intent.ACTION_VIEW);
        i.setDataAndType(uri, OpenProvider.mimeOf(path));
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        return onUi(() -> {
            try {
                startActivity(Intent.createChooser(i, null));
                return "1";
            } catch (ActivityNotFoundException e) {
                return null;
            }
        });
    }

    public String refreshRate() {
        return Float.toString(getWindowManager().getDefaultDisplay().getRefreshRate());
    }

    /* an update's APK to Android's installer; the first time Android asks to allow installs from jackdaw */
    public String installPackage(String path) {
        Intent i = new Intent(Intent.ACTION_VIEW)
                       .setDataAndType(OpenProvider.uriFor(getPackageName(), path), "application/vnd.android.package-archive")
                       .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
        return onUi(() -> {
            try {
                startActivity(i);
                return "1";
            } catch (ActivityNotFoundException e) {
                return null;
            }
        });
    }

    /* ---- printing ---- */

    /* path is an already laid-out PDF: the adapter passes it through as it is */
    public String print(String path, String title) {
        PrintManager pm = (PrintManager) getSystemService(PRINT_SERVICE);
        if (pm == null) return null;
        PrintDocumentAdapter adapter = new PrintDocumentAdapter() {
            @Override
            public void onLayout(PrintAttributes old, PrintAttributes now, CancellationSignal cancel,
                                 LayoutResultCallback cb, Bundle extras) {
                if (cancel.isCanceled()) {
                    cb.onLayoutCancelled();
                    return;
                }
                cb.onLayoutFinished(new PrintDocumentInfo.Builder(new File(path).getName())
                                        .setContentType(PrintDocumentInfo.CONTENT_TYPE_DOCUMENT)
                                        .build(), true);
            }

            @Override
            public void onWrite(PageRange[] pages, ParcelFileDescriptor dest, CancellationSignal cancel,
                                WriteResultCallback cb) {
                try (InputStream in = new FileInputStream(path);
                     OutputStream out = new FileOutputStream(dest.getFileDescriptor())) {
                    byte[] buf = new byte[65536];
                    for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
                    cb.onWriteFinished(new PageRange[] {PageRange.ALL_PAGES});
                } catch (Exception e) {
                    cb.onWriteFailed(e.getMessage());
                }
            }
        };
        return onUi(() -> {
            pm.print(title == null || title.isEmpty() ? "jackdaw" : title, adapter, null);
            return "1";
        });
    }

    /* ---- picking files to attach ---- */

    private static final int REQUEST_PICK = 1;

    public String pickFiles() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        return onUi(() -> {
            try {
                startActivityForResult(i, REQUEST_PICK);
                return "1";
            } catch (ActivityNotFoundException e) {
                return null;
            }
        });
    }

    /* the picked documents copied into the cache (the native side reads paths), handed over newline-separated */
    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != REQUEST_PICK) return;
        StringBuilder paths = new StringBuilder();
        if (result == RESULT_OK && data != null) {
            if (data.getClipData() != null) {
                ClipData clip = data.getClipData();
                for (int k = 0; k < clip.getItemCount(); k++) copyPicked(clip.getItemAt(k).getUri(), paths);
            } else if (data.getData() != null) {
                copyPicked(data.getData(), paths);
            }
        }
        nativePicked(paths.toString());
    }

    private void copyPicked(Uri uri, StringBuilder paths) {
        String name = "attachment";
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) name = c.getString(0);
        } catch (Exception e) {
            /* keeps the plain name */
        }
        File dir = new File(getCacheDir(), "picked/" + System.nanoTime());
        if (!dir.mkdirs()) return;
        File out = new File(dir, name.replace('/', '_'));
        try (InputStream in = getContentResolver().openInputStream(uri); OutputStream o = new FileOutputStream(out)) {
            if (in == null) return;
            byte[] buf = new byte[65536];
            for (int n; (n = in.read(buf)) > 0;) o.write(buf, 0, n);
        } catch (Exception e) {
            return;
        }
        if (paths.length() > 0) paths.append('\n');
        paths.append(out.getAbsolutePath());
    }

    /* ---- the account password ---- */

    /* One file in the app's private storage maps account -> password, sealed with AES-GCM under a key that lives in
     * the Android Keystore and never leaves it. */
    private static final String SECRET_KEY = "jackdaw-accounts";

    private File secretsFile() { return new File(getFilesDir(), "accounts.sealed"); }

    /* static: the background check (MailJobService) has no activity */
    static synchronized String passwordFor(File files, String account) {
        try {
            return readSecrets(new File(files, "accounts.sealed")).getProperty(account);
        } catch (Exception e) {
            return null;
        }
    }

    private static SecretKey secretKey() throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        if (ks.containsAlias(SECRET_KEY)) return (SecretKey) ks.getKey(SECRET_KEY, null);
        KeyGenerator g = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        g.init(new KeyGenParameterSpec.Builder(SECRET_KEY, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .build());
        return g.generateKey();
    }

    /* the file is the 12-byte IV, then the ciphertext of the Properties text */
    private Properties readSecrets() throws Exception { return readSecrets(secretsFile()); }

    private static Properties readSecrets(File f) throws Exception {
        Properties p = new Properties();
        if (!f.exists()) return p;
        byte[] all = java.nio.file.Files.readAllBytes(f.toPath());
        if (all.length < 12) return p;
        Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
        c.init(Cipher.DECRYPT_MODE, secretKey(), new GCMParameterSpec(128, all, 0, 12));
        p.load(new ByteArrayInputStream(c.doFinal(all, 12, all.length - 12)));
        return p;
    }

    private void writeSecrets(Properties p) throws Exception {
        ByteArrayOutputStream text = new ByteArrayOutputStream();
        p.store(text, null);
        Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
        c.init(Cipher.ENCRYPT_MODE, secretKey());
        byte[] iv = c.getIV(), sealed = c.doFinal(text.toByteArray());
        File tmp = new File(getFilesDir(), "accounts.sealed.tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(iv);
            out.write(sealed);
            out.getFD().sync();
        }
        if (!tmp.renameTo(secretsFile())) throw new java.io.IOException("rename");
    }

    public synchronized String secretGet(String key) {
        try {
            return readSecrets().getProperty(key);
        } catch (Exception e) {
            return null;
        }
    }

    public synchronized String secretPut(String key, String value) {
        try {
            Properties p = readSecrets();
            p.setProperty(key, value);
            writeSecrets(p);
            return "1";
        } catch (Exception e) {
            return null;
        }
    }

    public synchronized String secretDelete(String key) {
        try {
            Properties p = readSecrets();
            p.remove(key);
            writeSecrets(p);
            return "1";
        } catch (Exception e) {
            return null;
        }
    }

    /* ---- clipboard ---- */

    public String setClipboard(String text) {
        return onUi(() -> {
            ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            cm.setPrimaryClip(ClipData.newPlainText("jackdaw", text));
            return "1";
        });
    }

    public String clipboard() {
        return onUi(() -> {
            ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            ClipData clip = cm.getPrimaryClip();
            if (clip == null || clip.getItemCount() == 0) return null;
            CharSequence s = clip.getItemAt(0).coerceToText(this);
            return s == null ? null : s.toString();
        });
    }

    /* ---- soft keyboard ---- */

    private static final int IME_BACKSPACE = -1, IME_ENTER = -2, IME_DELETE = -3;

    public String showKeyboard() {
        runOnUiThread(() -> {
            sink.requestFocus();
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            imm.restartInput(sink);
            imm.showSoftInput(sink, 0);
        });
        return null;
    }

    public String hideKeyboard() {
        runOnUiThread(() -> {
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            imm.hideSoftInputFromWindow(sink.getWindowToken(), 0);
        });
        return null;
    }

    /* NativeActivity's own view takes key events only, so composed text (swipe, autocorrect) would never arrive.
     * What the keyboard is composing is typed as it goes and replaced (backspaces, then the new text) when it
     * changes or is committed. */
    private final class TextSink extends View {
        TextSink() {
            super(JackdawActivity.this);
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean onCheckIsTextEditor() { return true; }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo info) {
            info.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE;
            info.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
            return new SinkConnection(this);
        }
    }

    private static final class SinkConnection extends BaseInputConnection {
        private String composing = "";

        SinkConnection(View view) { super(view, false); }

        private static void send(int backspaces, CharSequence text) {
            String s = text == null ? "" : text.toString();
            int[] codes = new int[backspaces + s.codePointCount(0, s.length())];
            int k = 0;
            for (int i = 0; i < backspaces; i++) codes[k++] = IME_BACKSPACE;
            for (int i = 0; i < s.length(); i += Character.charCount(s.codePointAt(i))) {
                int c = s.codePointAt(i);
                codes[k++] = c == '\n' ? IME_ENTER : c;
            }
            if (codes.length > 0) nativeText(codes);
        }

        private void replaceComposing(CharSequence text) {
            send(composing.codePointCount(0, composing.length()), text);
        }

        @Override
        public boolean setComposingText(CharSequence text, int cursor) {
            replaceComposing(text);
            composing = text == null ? "" : text.toString();
            return true;
        }

        @Override
        public boolean commitText(CharSequence text, int cursor) {
            replaceComposing(text);
            composing = "";
            return true;
        }

        @Override
        public boolean finishComposingText() {
            composing = "";
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int before, int after) {
            int[] codes = new int[before + after];
            for (int i = 0; i < before; i++) codes[i] = IME_BACKSPACE;
            for (int i = 0; i < after; i++) codes[before + i] = IME_DELETE;
            if (codes.length > 0) nativeText(codes);
            return true;
        }

        @Override
        public boolean sendKeyEvent(android.view.KeyEvent event) {
            /* some keyboards send Backspace and Enter as key events instead */
            if (event.getAction() == android.view.KeyEvent.ACTION_DOWN) {
                if (event.getKeyCode() == android.view.KeyEvent.KEYCODE_DEL) nativeText(new int[] {IME_BACKSPACE});
                else if (event.getKeyCode() == android.view.KeyEvent.KEYCODE_ENTER) nativeText(new int[] {IME_ENTER});
                else if (event.getUnicodeChar() != 0) nativeText(new int[] {event.getUnicodeChar()});
            }
            return true;
        }

        @Override
        public boolean performEditorAction(int action) {
            nativeText(new int[] {IME_ENTER});
            return true;
        }
    }
}
