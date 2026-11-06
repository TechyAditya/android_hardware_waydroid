package waydroid.appdisplay;

import android.content.ComponentName;
import android.content.Context;
import android.graphics.Bitmap;
import android.media.MediaMetadata;
import android.media.session.MediaController;
import android.media.session.MediaSession;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.net.LocalSocket;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Callable;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

/**
 * Android's active media sessions, as JSON snapshots for watchers on the host, which publish them
 * over MPRIS. Everything but the watcher writes runs on the main looper.
 */
final class MediaSessions {
    private static final String TAG = Media.TAG;
    static final File ART_DIR = new File(Main.DIR, "media");
    private static final int ART_MAX_SIZE = 512;
    private static final int ART_QUALITY = 90;
    // A change often comes as several callbacks; one snapshot covers them.
    private static final long PUBLISH_DELAY_MS = 50;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final MediaSessionManager manager;
    private final Map<MediaSession.Token, Entry> entries = new LinkedHashMap<>();
    private final List<LocalSocket> watchers = new CopyOnWriteArrayList<>();
    // Writes block on slow watchers, so they stay off the main looper, in order.
    private final ExecutorService writer = Executors.newSingleThreadExecutor();
    private int nextId = 1, nextArt = 1;
    private String snapshot = "{\"sessions\":[]}\n";
    private boolean publishPending;

    private final class Entry extends MediaController.Callback {
        final int id = nextId++;
        final MediaController controller;
        String artKey;
        File artFile;

        Entry(MediaController controller) {
            this.controller = controller;
        }

        @Override
        public void onMetadataChanged(MediaMetadata metadata) {
            schedulePublish();
        }

        @Override
        public void onPlaybackStateChanged(PlaybackState state) {
            schedulePublish();
        }

        @Override
        public void onSessionDestroyed() {
            refresh();
        }
    }

    MediaSessions() throws ReflectiveOperationException {
        // app_process skips the setup that gives media managers their service binders.
        try {
            Class.forName("android.app.ActivityThread").getMethod("initializeMainlineModules").invoke(null);
        } catch (Throwable t) {
            Log.w(TAG, "initializeMainlineModules failed", t);
        }
        manager = MediaSessionManager.class.getDeclaredConstructor(Context.class)
                .newInstance(new AndroidBridge.SystemContext());
    }

    void start() {
        if (ART_DIR.isDirectory()) {
            File[] old = ART_DIR.listFiles();
            if (old != null)
                for (File f : old)
                    f.delete();
        } else if (!ART_DIR.mkdirs()) {
            Log.w(TAG, "can't create " + ART_DIR + "; no album art");
        }
        // A null listener component needs MEDIA_CONTENT_CONTROL, which the system user has.
        manager.addOnActiveSessionsChangedListener(new MediaSessionManager.OnActiveSessionsChangedListener() {
            @Override
            public void onActiveSessionsChanged(List<MediaController> controllers) {
                refresh();
            }
        }, (ComponentName) null, handler);
        refresh();
    }

    private void refresh() {
        List<MediaController> controllers = manager.getActiveSessions(null);
        Map<MediaSession.Token, Entry> next = new LinkedHashMap<>();
        for (MediaController c : controllers) {
            Entry e = entries.remove(c.getSessionToken());
            if (e == null) {
                e = new Entry(c);
                c.registerCallback(e, handler);
            }
            next.put(c.getSessionToken(), e);
        }
        for (Entry gone : entries.values()) {
            gone.controller.unregisterCallback(gone);
            if (gone.artFile != null)
                gone.artFile.delete();
        }
        entries.clear();
        entries.putAll(next);
        schedulePublish();
    }

    private void schedulePublish() {
        if (publishPending)
            return;
        publishPending = true;
        handler.postDelayed(new Runnable() {
            @Override
            public void run() {
                publishPending = false;
                publish();
            }
        }, PUBLISH_DELAY_MS);
    }

    private void publish() {
        JSONArray list = new JSONArray();
        for (Entry e : entries.values()) {
            try {
                list.put(describe(e));
            } catch (Throwable t) {
                Log.w(TAG, "describing " + e.controller.getPackageName() + " failed", t);
            }
        }
        final String line;
        try {
            line = new JSONObject().put("sessions", list).toString() + "\n";
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
        snapshot = line;
        writer.execute(new Runnable() {
            @Override
            public void run() {
                for (LocalSocket client : watchers)
                    send(client, line);
            }
        });
    }

    private JSONObject describe(Entry e) throws JSONException {
        MediaController c = e.controller;
        JSONObject o = new JSONObject().put("id", e.id).put("package", c.getPackageName());
        MediaMetadata m = c.getMetadata();
        if (m != null) {
            o.put("title", text(m, MediaMetadata.METADATA_KEY_TITLE, MediaMetadata.METADATA_KEY_DISPLAY_TITLE));
            o.put("artist", text(m, MediaMetadata.METADATA_KEY_ARTIST, MediaMetadata.METADATA_KEY_ALBUM_ARTIST,
                    MediaMetadata.METADATA_KEY_DISPLAY_SUBTITLE));
            o.put("album", text(m, MediaMetadata.METADATA_KEY_ALBUM));
            o.put("duration_ms", m.getLong(MediaMetadata.METADATA_KEY_DURATION));
            updateArt(e, m);
            if (e.artFile != null)
                o.put("art", e.artFile.getPath());
            String uri = text(m, MediaMetadata.METADATA_KEY_ALBUM_ART_URI, MediaMetadata.METADATA_KEY_ART_URI,
                    MediaMetadata.METADATA_KEY_DISPLAY_ICON_URI);
            if (uri.startsWith("http://") || uri.startsWith("https://"))
                o.put("art_url", uri);
        }
        PlaybackState s = c.getPlaybackState();
        String status = "stopped";
        long position = 0, actions = 0;
        double speed = 0;
        if (s != null) {
            switch (s.getState()) {
                case PlaybackState.STATE_PLAYING:
                case PlaybackState.STATE_BUFFERING:
                case PlaybackState.STATE_CONNECTING:
                case PlaybackState.STATE_FAST_FORWARDING:
                case PlaybackState.STATE_REWINDING:
                case PlaybackState.STATE_SKIPPING_TO_NEXT:
                case PlaybackState.STATE_SKIPPING_TO_PREVIOUS:
                case PlaybackState.STATE_SKIPPING_TO_QUEUE_ITEM:
                    status = "playing";
                    break;
                case PlaybackState.STATE_PAUSED:
                    status = "paused";
                    break;
                default:
                    break;
            }
            position = s.getPosition();
            if (s.getState() == PlaybackState.STATE_PLAYING) {
                speed = s.getPlaybackSpeed();
                position += (long) ((SystemClock.elapsedRealtime() - s.getLastPositionUpdateTime()) * speed);
            }
            actions = s.getActions();
        }
        return o.put("status", status).put("position_ms", Math.max(0, position)).put("speed", speed)
                .put("actions", actions);
    }

    private static String text(MediaMetadata m, String... keys) {
        for (String key : keys) {
            CharSequence v = m.getText(key);
            if (v != null && v.length() > 0)
                return v.toString();
        }
        return "";
    }

    /** Saves the track's picture as a JPEG for the host, under a new name for each picture. */
    private void updateArt(Entry e, MediaMetadata m) {
        Bitmap art = m.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART);
        if (art == null)
            art = m.getBitmap(MediaMetadata.METADATA_KEY_ART);
        if (art == null)
            art = m.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON);
        String key = art == null ? null
                : art.getGenerationId() + ":" + art.getWidth() + "x" + art.getHeight() + ":"
                        + text(m, MediaMetadata.METADATA_KEY_TITLE) + ":" + text(m, MediaMetadata.METADATA_KEY_ALBUM);
        if (key == null ? e.artKey == null : key.equals(e.artKey))
            return;
        File old = e.artFile;
        e.artKey = key;
        e.artFile = null;
        if (art != null) {
            float scale = Math.min(1f, (float) ART_MAX_SIZE / Math.max(art.getWidth(), art.getHeight()));
            Bitmap scaled = scale < 1f
                    ? Bitmap.createScaledBitmap(art, Math.round(art.getWidth() * scale),
                            Math.round(art.getHeight() * scale), true)
                    : art;
            File file = new File(ART_DIR, e.id + "-" + nextArt++ + ".jpg");
            File part = new File(ART_DIR, "." + file.getName() + ".part");
            try (FileOutputStream out = new FileOutputStream(part)) {
                if (scaled.compress(Bitmap.CompressFormat.JPEG, ART_QUALITY, out) && part.renameTo(file))
                    e.artFile = file;
            } catch (IOException ex) {
                Log.w(TAG, "saving album art failed", ex);
            }
            if (scaled != art)
                scaled.recycle();
        }
        // The host may still be loading the previous picture; it has the time this takes to change.
        if (old != null) {
            final File stale = old;
            handler.postDelayed(new Runnable() {
                @Override
                public void run() {
                    stale.delete();
                }
            }, 10000);
        }
    }

    /** Sends the current snapshot, then every new one, until the watcher goes away. */
    void watch(final LocalSocket client) {
        handler.post(new Runnable() {
            @Override
            public void run() {
                final String line = snapshot;
                writer.execute(new Runnable() {
                    @Override
                    public void run() {
                        if (send(client, line))
                            watchers.add(client);
                    }
                });
            }
        });
    }

    private boolean send(LocalSocket client, String line) {
        try {
            OutputStream out = client.getOutputStream();
            out.write(line.getBytes(StandardCharsets.UTF_8));
            out.flush();
            return true;
        } catch (IOException e) {
            watchers.remove(client);
            try {
                client.close();
            } catch (IOException ignored) {
            }
            return false;
        }
    }

    /** Runs a transport control on the session; returns "" or an error message. */
    String control(final int id, final String action, final long arg) throws Exception {
        FutureTask<String> task = new FutureTask<>(new Callable<String>() {
            @Override
            public String call() {
                for (Entry e : entries.values()) {
                    if (e.id != id)
                        continue;
                    MediaController.TransportControls t = e.controller.getTransportControls();
                    PlaybackState s = e.controller.getPlaybackState();
                    switch (action) {
                        case "play": t.play(); break;
                        case "pause": t.pause(); break;
                        case "playpause":
                            if (s != null && s.getState() == PlaybackState.STATE_PLAYING)
                                t.pause();
                            else
                                t.play();
                            break;
                        case "stop": t.stop(); break;
                        case "next": t.skipToNext(); break;
                        case "previous": t.skipToPrevious(); break;
                        case "seek": t.seekTo(Math.max(0, arg)); break;
                        default: return "unknown action " + action;
                    }
                    return "";
                }
                return "no session " + id;
            }
        });
        handler.post(task);
        return task.get(2, TimeUnit.SECONDS);
    }
}
