package io.github.vamsikalagaturu.jackdaw;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;
import java.io.File;
import java.io.FileNotFoundException;
import java.util.Locale;

/* Read-only access to an attachment JackdawActivity.openFile() hands to another app:
 * content://<package>.files/<absolute path>. Not exported: only the app an intent grants the URI to can read it,
 * and only files of this app's own storage. Apps targeting Android 7+ may not pass file:// URIs. */
public class OpenProvider extends ContentProvider {
    static Uri uriFor(String pkg, String path) {
        return new Uri.Builder().scheme("content").authority(pkg + ".files").path(path).build();
    }

    static String mimeOf(String path) {
        String name = new File(path).getName();
        int dot = name.lastIndexOf('.');
        String mime = dot > 0
            ? MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(dot + 1).toLowerCase(Locale.ROOT))
            : null;
        return mime != null ? mime : "*/*";
    }

    @Override
    public boolean onCreate() { return true; }

    private File file(Uri uri) throws FileNotFoundException {
        String path = uri.getPath();
        if (path == null) throw new FileNotFoundException(uri.toString());
        File f = new File(path);
        try {
            String own = getContext().getDataDir().getCanonicalPath();
            if (!f.getCanonicalPath().startsWith(own + "/")) throw new FileNotFoundException(uri.toString());
        } catch (java.io.IOException e) {
            throw new FileNotFoundException(uri.toString());
        }
        if (!f.isFile() || !f.canRead()) throw new FileNotFoundException(uri.toString());
        return f;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new SecurityException("read-only");
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        File f;
        try {
            f = file(uri);
        } catch (FileNotFoundException e) {
            return null;
        }
        String[] cols = projection != null ? projection : new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) row[i] = f.getName();
            else if (OpenableColumns.SIZE.equals(cols[i])) row[i] = f.length();
        }
        MatrixCursor c = new MatrixCursor(cols, 1);
        c.addRow(row);
        return c;
    }

    @Override
    public String getType(Uri uri) {
        String path = uri.getPath();
        return path == null ? null : mimeOf(path);
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }

    @Override
    public int delete(Uri uri, String selection, String[] args) { throw new UnsupportedOperationException(); }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) { throw new UnsupportedOperationException(); }
}
