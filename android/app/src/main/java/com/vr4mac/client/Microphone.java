package com.vr4mac.client;

import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.os.SystemClock;
import android.util.Log;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

// A separate capture thread prevents microphone reads from holding video locks.
final class Microphone implements AutoCloseable {
    interface Sink { void send(byte[] packet) throws Exception; }
    private final AudioRecord recorder;
    private final Thread worker;
    private volatile boolean running=true;
    Microphone(Sink sink) {
        int min=AudioRecord.getMinBufferSize(48000,AudioFormat.CHANNEL_IN_MONO,AudioFormat.ENCODING_PCM_16BIT);
        if(min<=0) throw new IllegalStateException("Microphone format unsupported");
        recorder=new AudioRecord(MediaRecorder.AudioSource.VOICE_COMMUNICATION,48000,
                AudioFormat.CHANNEL_IN_MONO,AudioFormat.ENCODING_PCM_16BIT,Math.max(min,3840));
        if(recorder.getState()!=AudioRecord.STATE_INITIALIZED) { recorder.release();throw new IllegalStateException("Microphone initialization failed"); }
        try { recorder.startRecording(); } catch(RuntimeException e) { recorder.release();throw e; }
        worker=new Thread(() -> {
            byte[] pcm=new byte[960];long frames=0,lastLog=SystemClock.elapsedRealtimeNanos();int peak=0;
            try {
                while(running) {
                    int n=0;
                    while(running && n<pcm.length) {
                        int read=recorder.read(pcm,n,pcm.length-n,AudioRecord.READ_BLOCKING);
                        if(read<=0) throw new IllegalStateException("Microphone read error "+read);
                        n+=read;
                    }
                    if(!running) break;
                    if(n<0) throw new IllegalStateException("Microphone read error "+n);
                    n &= ~1;if(n==0)continue;
                    ByteBuffer b=ByteBuffer.allocate(8+n).order(ByteOrder.LITTLE_ENDIAN);
                    b.putLong(SystemClock.elapsedRealtimeNanos()).put(pcm,0,n);
                    sink.send(b.array());frames+=n/2;
                    for(int i=0;i<n;i+=2) peak=Math.max(peak,Math.abs((short)((pcm[i]&255)|(pcm[i+1]<<8))));
                    long now=SystemClock.elapsedRealtimeNanos();
                    if(now-lastLog>=5000000000L) { Log.i("VR4Mac","Microphone frames="+frames+", peak="+peak+"/32768");frames=0;peak=0;lastLog=now; }
                }
            } catch(Exception e) { if(running) Log.w("VR4Mac","Microphone stopped",e); }
            finally { running=false;try { recorder.stop(); } catch(RuntimeException ignored) {} recorder.release(); }
        },"Quest microphone");worker.start();
    }
    boolean active() { return running; }
    public void close() { running=false;try { recorder.stop(); } catch(RuntimeException ignored) {} }
}
