package com.androidevolved.asphalt5;

import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.media.SoundPool;
import android.util.Log;
import java.io.File;
import java.util.Locale;

/**
 * Static callbacks the engine resolves by name and signature (see
 * native/guest_jni.c). The names and return values mirror the original
 * GLMediaPlayer and Asphalt5 classes. All methods run on the GL thread.
 */
final class GameBridge {
    private static final String TAG = "Asphalt5";
    private static final int MAX_STREAMS = 16;

    static GameActivity activity;
    static String dataDir;

    private static SoundPool pool;
    private static int[] poolIds = new int[0];
    private static int[][] streams = new int[0][];
    private static MediaPlayer[] players = new MediaPlayer[0];
    private static boolean[] prepared = new boolean[0];
    private static int instances = 1;

    private GameBridge() {
    }

    private static String soundPath(int sound) {
        return String.format(Locale.ROOT, "%s/raw_%03d.glsnd", dataDir, sound);
    }

    private static boolean valid(int sound) {
        return sound >= 0 && sound < poolIds.length;
    }

    // ---- setup (called by game_host.c) ----

    static void initAudio(int total, int sameInstance) {
        instances = Math.max(1, sameInstance);
        poolIds = new int[Math.max(0, total)];
        java.util.Arrays.fill(poolIds, -1);
        streams = new int[poolIds.length][instances];
        players = new MediaPlayer[poolIds.length];
        prepared = new boolean[poolIds.length];
        initSoundPoolArray();
    }

    static void initSoundPoolArray() {
        if (pool != null) return;
        pool = new SoundPool.Builder()
                .setMaxStreams(MAX_STREAMS)
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                        .build())
                .build();
    }

    static void destroySoundPool() {
        stopAllSounds();
        if (pool != null) pool.release();
        pool = null;
        java.util.Arrays.fill(poolIds, -1);
    }

    // ---- pooled effects: 0 means loaded, -1 not loaded (original contract) ----

    static int isSoundLoaded(int sound, int instance) {
        return valid(sound) && poolIds[sound] >= 0 ? 0 : -1;
    }

    static void loadSound(int sound, int instance) {
        if (!valid(sound) || pool == null || poolIds[sound] >= 0) return;
        File file = new File(soundPath(sound));
        if (!file.isFile()) {
            Log.w(TAG, "missing sound " + file);
            return;
        }
        poolIds[sound] = pool.load(file.getPath(), 1);
    }

    static void unloadSound(int sound, int instance) {
        if (!valid(sound) || poolIds[sound] < 0) return;
        stopAllPool(sound);
        if (pool != null) pool.unload(poolIds[sound]);
        poolIds[sound] = -1;
    }

    static void playSound(int sound, int instance, float volume) {
        if (!valid(sound) || pool == null) return;
        if (poolIds[sound] < 0) loadSound(sound, instance);
        if (poolIds[sound] < 0) return;
        int slot = Math.floorMod(instance, instances);
        float v = clamp(volume);
        streams[sound][slot] = pool.play(poolIds[sound], v, v, 1, 0, 1f);
    }

    private static int stream(int sound, int instance) {
        return valid(sound) ? streams[sound][Math.floorMod(instance, instances)] : 0;
    }

    static void pauseSound(int sound, int instance) {
        if (pool != null) pool.pause(stream(sound, instance));
    }

    static void resumeSound(int sound, int instance) {
        if (pool != null) pool.resume(stream(sound, instance));
    }

    static void stopSound(int sound, int instance) {
        if (pool != null) pool.stop(stream(sound, instance));
    }

    static void setVolume(int sound, int instance, float volume) {
        float v = clamp(volume);
        if (pool != null) pool.setVolume(stream(sound, instance), v, v);
    }

    static void setPitch(int sound, int instance, float pitch) {
        if (pool != null) pool.setRate(stream(sound, instance), Math.max(0.5f, Math.min(2f, pitch)));
    }

    static void resetSound(int sound) {
        stopAllPool(sound);
    }

    static void stopAllPool(int sound) {
        if (pool == null || !valid(sound)) return;
        for (int id : streams[sound]) pool.stop(id);
    }

    static void stopAllSounds() {
        for (int sound = 0; sound < poolIds.length; sound++) {
            stopAllPool(sound);
            stopSoundBig(sound);
        }
    }

    // ---- streamed music ----

    static int isSoundLoadedBig(int sound) {
        return valid(sound) && players[sound] != null && prepared[sound] ? 0 : -1;
    }

    static void loadSoundBig(int sound) {
        if (!valid(sound) || players[sound] != null) return;
        MediaPlayer player = new MediaPlayer();
        try {
            player.setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build());
            player.setDataSource(soundPath(sound));
            player.prepare();
        } catch (Exception error) {
            Log.w(TAG, "cannot load music " + sound, error);
            player.release();
            return;
        }
        player.setOnCompletionListener(mp -> {
            GameActivity a = activity;
            if (a != null) a.queueGameEvent(() -> GameNative.nativeMusicCompleted(sound));
        });
        players[sound] = player;
        prepared[sound] = true;
    }

    static void unloadSoundBig(int sound) {
        if (!valid(sound) || players[sound] == null) return;
        players[sound].release();
        players[sound] = null;
        prepared[sound] = false;
    }

    static void playSoundBig(int sound, float volume) {
        if (!valid(sound)) return;
        if (players[sound] == null) loadSoundBig(sound);
        MediaPlayer player = players[sound];
        if (player == null) return;
        try {
            if (!prepared[sound]) {
                player.prepare();
                prepared[sound] = true;
            }
            float v = clamp(volume);
            player.setVolume(v, v);
            player.start();
        } catch (Exception error) {
            Log.w(TAG, "cannot play music " + sound, error);
        }
    }

    static void pauseSoundBig(int sound) {
        if (valid(sound) && players[sound] != null && players[sound].isPlaying()) players[sound].pause();
    }

    static void resumeSoundBig(int sound) {
        if (valid(sound) && players[sound] != null && prepared[sound]) players[sound].start();
    }

    static void stopSoundBig(int sound) {
        if (!valid(sound) || players[sound] == null || !prepared[sound]) return;
        players[sound].stop();
        prepared[sound] = false;   // MediaPlayer needs prepare() before the next start()
    }

    static void stopAllBig(int unused) {
        for (int sound = 0; sound < players.length; sound++) stopSoundBig(sound);
    }

    static void setVolumeBig(int sound, float volume) {
        float v = clamp(volume);
        if (valid(sound) && players[sound] != null) players[sound].setVolume(v, v);
    }

    static boolean isMediaPlaying(int sound) {
        return valid(sound) && players[sound] != null && prepared[sound] && players[sound].isPlaying();
    }

    static void pauseAllAudio() {
        if (pool != null) pool.autoPause();
        for (MediaPlayer player : players) {
            if (player != null && player.isPlaying()) player.pause();
        }
    }

    static void resumeAllAudio() {
        if (pool != null) pool.autoResume();
    }

    private static float clamp(float volume) {
        return Float.isNaN(volume) ? 0f : Math.max(0f, Math.min(1f, volume));
    }

    // ---- Asphalt5 activity callbacks ----

    static void sendAppToBackground() {
        GameActivity a = activity;
        if (a != null) a.runOnUiThread(() -> a.moveTaskToBack(true));
    }

    static void Exit() {
        GameActivity a = activity;
        if (a != null) a.runOnUiThread(a::finishAndRemoveTask);
    }

    static void LaunchBilling() {
    }

    static void ReleaseBillingContext() {
    }

    static int IsDemo() {
        return 0;
    }

    static int IsDoubleOption() {
        return 0;
    }

    static byte[] GetDoubleOptionText1() {
        return new byte[0];
    }

    static byte[] GetDoubleOptionText2() {
        return new byte[0];
    }

    static byte[] GetDoubleOptionText3() {
        return new byte[0];
    }

    static void launchGetGames(int value) {
    }

    static void OpenGLive(int value) {
    }

    static void NotifyTrophy(int trophy) {
        Log.i(TAG, "trophy " + trophy);
    }

    static void SetLoadingValuable(int value) {
    }
}
