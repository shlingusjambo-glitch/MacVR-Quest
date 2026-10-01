package com.vr4mac.client;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.os.Process;
import android.util.Log;
import java.util.ArrayDeque;
import java.util.Arrays;

/** Network never waits on the audio device. Queue is bounded by PCM duration. */
final class AudioPlayer implements AutoCloseable {
    private static final int MAX_QUEUED = 48000 * 4 * 40 / 1000;
    private final ArrayDeque<byte[]> queue = new ArrayDeque<>();
    private final AudioTrack track;
    private final Thread worker;
    private volatile boolean running = true;
    private int queuedBytes;
    private boolean reset;
    AudioPlayer() {
        int min = AudioTrack.getMinBufferSize(48000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        if (min <= 0) throw new IllegalStateException("No stereo PCM audio output");
        track = new AudioTrack.Builder()
            .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(new AudioFormat.Builder().setSampleRate(48000)
                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
            .setTransferMode(AudioTrack.MODE_STREAM).setBufferSizeInBytes(Math.max(min,3840))
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY).build();
        if (track.getState() != AudioTrack.STATE_INITIALIZED) { track.release(); throw new IllegalStateException("AudioTrack initialization failed"); }
        worker = new Thread(this::play, "VR4Mac audio"); worker.start();
    }
    boolean available() { return running; }
    void offer(byte[] packet) {
        int bytes=packet.length-8;
        if (bytes <= 0 || bytes % 4 != 0 || bytes > MAX_QUEUED) return;
        byte[] pcm=Arrays.copyOfRange(packet,8,packet.length);
        synchronized(queue) {
            if (!running) return;
            while (queuedBytes+bytes > MAX_QUEUED && !queue.isEmpty()) queuedBytes-=queue.removeFirst().length;
            queue.addLast(pcm); queuedBytes+=bytes; queue.notifyAll();
        }
    }
    void reset() { synchronized(queue) { queue.clear(); queuedBytes=0; reset=true; queue.notifyAll(); } }
    private void play() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
        try {
            track.play();
            long reportAt=System.nanoTime(), writtenFrames=0;
            int peak=0;
            while(running) {
                byte[] pcm; boolean flush;
                synchronized(queue) {
                    while(running && queue.isEmpty() && !reset) queue.wait();
                    if (!running) break;
                    flush=reset; reset=false;
                    pcm=queue.pollFirst(); if(pcm != null) queuedBytes-=pcm.length;
                }
                if(flush) { track.pause(); track.flush(); track.play(); }
                if(pcm == null) continue;
                for(int i=0;i+1<pcm.length;i+=2) {
                    int sample=(short)((pcm[i]&255)|(pcm[i+1]<<8));
                    peak=Math.max(peak,Math.abs(sample));
                }
                int offset=0;
                while(running && offset<pcm.length) {
                    int written=track.write(pcm,offset,Math.min(1920,pcm.length-offset),AudioTrack.WRITE_BLOCKING);
                    if(written <= 0) throw new IllegalStateException("AudioTrack write failed: "+written);
                    offset+=written;
                    writtenFrames+=written/4;
                }
                long now=System.nanoTime();
                if(now-reportAt>=5_000_000_000L) {
                    Log.i("VR4Mac", "Audio PCM peak="+peak+"/32768, written frames="+writtenFrames+
                        ", playback head="+Integer.toUnsignedLong(track.getPlaybackHeadPosition()));
                    reportAt=now; writtenFrames=0; peak=0;
                }
            }
        } catch(Exception e) { if(running) Log.w("VR4Mac","Audio playback stopped",e); }
        finally { running=false; track.release(); }
    }
    public void close() {
        running=false;
        synchronized(queue) { queue.clear(); queuedBytes=0; queue.notifyAll(); }
        try { track.pause(); } catch(IllegalStateException ignored) {}
        worker.interrupt();
        try { worker.join(1000); } catch(InterruptedException e) { Thread.currentThread().interrupt(); }
    }
}
