package waydroid.appdisplay;

import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Looper;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * Shares Android's media sessions with the host over DIR/media.sock, apart from the display
 * service so either can restart alone. A client sends one line:
 *
 * <ul>
 * <li>{@code watch}: the connection stays open and gets a JSON snapshot line on every change.
 * <li>{@code control ID ACTION [MS]}: play, pause, playpause, stop, next, previous or seek to MS;
 *     the reply is "ok" or "error MESSAGE".
 * </ul>
 */
public final class Media {
    static final String TAG = "waydroid-app-media";
    private static final int READ_TIMEOUT_MS = 2000;

    public static void main(String[] args) throws Exception {
        if (AndroidBridge.prop("persist.waydroid.app_media", "true").equals("false")) {
            Log.i(TAG, "disabled by persist.waydroid.app_media");
            return;
        }
        System.load(Main.DIR + "/libappdisplay.so");
        if (!Native.dropToSystem()) {
            Log.e(TAG, "cannot switch to the system user");
            System.exit(1);
        }
        Looper.prepareMainLooper();
        final MediaSessions sessions = new MediaSessions();
        sessions.start();
        final LocalServerSocket server = listen(new File(Main.DIR, "media.sock"));
        new Thread(new Runnable() {
            @Override
            public void run() {
                for (;;) {
                    try {
                        serve(sessions, server.accept());
                    } catch (IOException e) {
                        Log.w(TAG, "accepting a client failed", e);
                    }
                }
            }
        }, "media-socket").start();
        Log.i(TAG, "ready");
        Looper.loop();
    }

    private static LocalServerSocket listen(File path) throws IOException {
        path.delete();
        LocalSocket socket = new LocalSocket(LocalSocket.SOCKET_STREAM);
        socket.bind(new LocalSocketAddress(path.getPath(), LocalSocketAddress.Namespace.FILESYSTEM));
        return new LocalServerSocket(socket.getFileDescriptor());
    }

    private static void serve(MediaSessions sessions, LocalSocket client) {
        boolean keep = false;
        try {
            client.setSoTimeout(READ_TIMEOUT_MS);
            String line = new BufferedReader(new InputStreamReader(client.getInputStream(), StandardCharsets.UTF_8))
                    .readLine();
            OutputStream out = client.getOutputStream();
            String[] f = line == null ? new String[0] : line.trim().split(" ");
            if (f.length == 1 && f[0].equals("watch")) {
                client.setSoTimeout(0);
                sessions.watch(client);
                keep = true;
                return;
            }
            String reply;
            if (f.length >= 3 && f[0].equals("control")) {
                String err = sessions.control(Integer.parseInt(f[1]), f[2], f.length > 3 ? Long.parseLong(f[3]) : 0);
                reply = err.isEmpty() ? "ok\n" : "error " + err + "\n";
            } else {
                reply = "error unknown command\n";
            }
            out.write(reply.getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            Log.w(TAG, "serving a client failed", e);
        } finally {
            if (!keep) {
                try {
                    client.close();
                } catch (IOException ignored) {
                }
            }
        }
    }
}
