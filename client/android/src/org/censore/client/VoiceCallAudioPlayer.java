package org.patronus.client;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.util.Log;

import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * AudioTrack-based player using USAGE_VOICE_COMMUNICATION so audio is routed
 * to the earpiece (or loudspeaker when speakerphone is on) in
 * MODE_IN_COMMUNICATION. QAudioSink uses STREAM_MUSIC which Android silences
 * or misroutes in communication mode.
 */
public class VoiceCallAudioPlayer {
    private static final String TAG = "VoiceAudioPlayer";
    private static final int SAMPLE_RATE = 48000;

    private AudioTrack audioTrack;
    private Thread writeThread;
    private volatile boolean running = false;
    private final LinkedBlockingQueue<byte[]> frameQueue = new LinkedBlockingQueue<>(100);

    public boolean init() {
        release();
        int minBuf = AudioTrack.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        );
        // 200ms buffer
        int bufSize = Math.max(minBuf, SAMPLE_RATE * 2 * 200 / 1000);
        try {
            audioTrack = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build())
                .setAudioFormat(new AudioFormat.Builder()
                    .setSampleRate(SAMPLE_RATE)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build())
                .setBufferSizeInBytes(bufSize)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build();

            boolean ok = audioTrack.getState() == AudioTrack.STATE_INITIALIZED;
            Log.d(TAG, "init: minBuf=" + minBuf + " bufSize=" + bufSize
                    + " state=" + audioTrack.getState() + " ok=" + ok);
            if (!ok) {
                audioTrack.release();
                audioTrack = null;
            }
            return ok;
        } catch (Exception e) {
            Log.e(TAG, "init failed: " + e);
            return false;
        }
    }

    public void start() {
        if (audioTrack == null || running) return;
        frameQueue.clear();
        running = true;
        audioTrack.play();
        writeThread = new Thread(() -> {
            Log.d(TAG, "write thread started");
            while (running) {
                try {
                    byte[] frame = frameQueue.poll(50, TimeUnit.MILLISECONDS);
                    if (frame == null) continue;
                    int written = 0;
                    while (written < frame.length && running) {
                        int result = audioTrack.write(frame, written, frame.length - written);
                        if (result > 0) {
                            written += result;
                        } else if (result < 0) {
                            Log.e(TAG, "AudioTrack.write error: " + result);
                            break;
                        }
                    }
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
            Log.d(TAG, "write thread stopped");
        }, "VoiceAudioWriter");
        writeThread.start();
        Log.d(TAG, "start: playState=" + audioTrack.getPlayState());
    }

    public void write(byte[] data) {
        if (!running || data == null) return;
        if (!frameQueue.offer(data)) {
            Log.w(TAG, "queue full, dropping frame");
        }
    }

    public void stop() {
        if (!running) return;
        running = false;
        if (writeThread != null) {
            writeThread.interrupt();
            try { writeThread.join(500); } catch (InterruptedException ignored) {}
            writeThread = null;
        }
        if (audioTrack != null) {
            try { audioTrack.pause(); audioTrack.flush(); } catch (Exception ignored) {}
        }
        frameQueue.clear();
        Log.d(TAG, "stopped");
    }

    public void release() {
        stop();
        if (audioTrack != null) {
            try { audioTrack.release(); } catch (Exception ignored) {}
            audioTrack = null;
        }
        Log.d(TAG, "released");
    }
}
