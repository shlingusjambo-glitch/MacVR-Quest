package com.vr4mac.client;

import android.app.Activity;
import android.os.Bundle;
import android.graphics.SurfaceTexture;
import android.view.Surface;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.util.Log;
import org.json.JSONObject;
import java.io.*;
import java.net.*;
import java.nio.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

public final class MainActivity extends Activity {
    static { System.loadLibrary("vr4mac"); }
    private native void runXR();
    private native void stopXR();
    private native void haptic(int hand, float amplitude, float duration, float frequency);
    private volatile boolean alive = true;
    private volatile int eyeWidth, eyeHeight;
    private boolean stageSpace;
    private volatile Socket socket;
    private volatile DataOutputStream output;
    private SurfaceTexture texture;
    private Surface surface;
    private final AtomicBoolean imageReady = new AtomicBoolean();
    private MediaCodec codec;
    private volatile String codecDescription = "none";
    private boolean hevcDisabled;
    private volatile AudioPlayer audio;
    private long idrWaitSince, lastIdrRequest;
    private final Object codecLock = new Object();
    private final android.util.LongSparseArray<long[]> enqueueTimes = new android.util.LongSparseArray<>();
    private final java.util.concurrent.atomic.AtomicLong receivedBytes = new java.util.concurrent.atomic.AtomicLong();
    private final java.util.concurrent.atomic.AtomicLong decodedFrames = new java.util.concurrent.atomic.AtomicLong();
    private final java.util.concurrent.atomic.AtomicLong droppedFrames = new java.util.concurrent.atomic.AtomicLong();
    private final java.util.concurrent.atomic.AtomicLong decodeNanos = new java.util.concurrent.atomic.AtomicLong();
    private final java.util.concurrent.atomic.AtomicLong inputNanos = new java.util.concurrent.atomic.AtomicLong();
    private final java.util.concurrent.atomic.AtomicLong holdNanos = new java.util.concurrent.atomic.AtomicLong();
    private final java.util.concurrent.atomic.AtomicLong drainNanos = new java.util.concurrent.atomic.AtomicLong();
    private Thread decoderOutputThread;
    private Thread xrThread, networkThread, trackingThread;
    private final ArrayBlockingQueue<byte[]> tracking = new ArrayBlockingQueue<>(1);
    private volatile boolean needIdr = true;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        xrThread = new Thread(this::runXR, "OpenXR"); xrThread.start();
    }
    @Override public void onDestroy() {
        alive = false; stopXR(); disconnect();
        super.onDestroy();
    }
    // Called with the GLES context current on the OpenXR thread.
    public void prepareVideo(int textureId, int width, int height, boolean stage) {
        if (networkThread != null && networkThread.isAlive()) return;
        if (decoderOutputThread != null && decoderOutputThread.isAlive()) return;
        if (trackingThread != null && trackingThread.isAlive()) return;
        alive = true; imageReady.set(false); tracking.clear();
        eyeWidth = width; eyeHeight = height; stageSpace = stage;
        texture = new SurfaceTexture(textureId);
        texture.setOnFrameAvailableListener(t -> imageReady.set(true));
        surface = new Surface(texture);
        try { audio=new AudioPlayer(); } catch(RuntimeException e) { Log.w("VR4Mac","Audio unavailable",e); audio=null; }
        decoderOutputThread = new Thread(this::outputLoop, "Decoder output"); decoderOutputThread.start();
        networkThread = new Thread(this::networkLoop, "VR4Mac network"); networkThread.start();
        trackingThread = new Thread(() -> {
            while (alive) {
                try { byte[] packet = tracking.poll(500, TimeUnit.MILLISECONDS); if (packet != null) send(3, packet); }
                catch (Exception e) { disconnect(); }
            }
        }, "Tracking sender"); trackingThread.start();
    }
    public long updateVideo(float[] transform) {
        if (texture == null || !imageReady.getAndSet(false)) return 0;
        texture.updateTexImage(); texture.getTransformMatrix(transform);
        return texture.getTimestamp();
    }
    private volatile float displayRefreshRate = 72;
    private final java.util.concurrent.CountDownLatch refreshReady = new java.util.concurrent.CountDownLatch(1);
    // Only advertise the current mode: CONFIG cannot switch the XR display yet.
    public void reportRefreshRate(float rate) {
        if (Float.isFinite(rate) && rate >= 30 && rate <= 240) {
            displayRefreshRate = rate;
            refreshReady.countDown();
        }
    }
    public void sendTracking(byte[] packet) { tracking.poll(); tracking.offer(packet); }
    public void reportError(String error) { Log.e("VR4Mac", error); runOnUiThread(() -> android.widget.Toast.makeText(this, error, android.widget.Toast.LENGTH_LONG).show()); }
    public void releaseVideo() {
        alive = false; disconnect();
        AudioPlayer oldAudio=audio; audio=null; if (oldAudio != null) oldAudio.close();
        if (networkThread != null) try { networkThread.join(2000); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        if (decoderOutputThread != null) try { decoderOutputThread.join(2000); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        if (trackingThread != null) try { trackingThread.join(1000); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        if (surface != null) { surface.release(); surface = null; } if (texture != null) { texture.release(); texture = null; }
    }
    private synchronized void send(int type, byte[] payload) throws IOException {
        DataOutputStream destination = output;
        if (destination == null) return;
        destination.writeByte(type); destination.writeInt(Integer.reverseBytes(payload.length)); destination.write(payload); destination.flush();
    }
    private void disconnect() {
        try { if (socket != null) socket.close(); } catch (IOException ignored) {}
        socket = null; output = null;
    }
    private Socket connect() throws IOException {
        String host = getIntent().getStringExtra("host");
        Socket candidate = new Socket(); candidate.setReceiveBufferSize(2*1024*1024);
        try { candidate.connect(new InetSocketAddress(host == null ? "127.0.0.1" : host, 9945), 800); return candidate; }
        catch (IOException e) { candidate.close(); if (host != null) throw e; }
        try (DatagramSocket discovery = new DatagramSocket(9944)) {
            discovery.setSoTimeout(1500);
            byte[] buf = new byte[128]; DatagramPacket packet = new DatagramPacket(buf, buf.length);
            discovery.receive(packet);
            if (!new String(buf, 0, packet.getLength(), java.nio.charset.StandardCharsets.US_ASCII).trim().equals("VR4MAC 9945")) throw new IOException("Unknown discovery packet");
            candidate = new Socket(); candidate.setReceiveBufferSize(2*1024*1024);
            try { candidate.connect(new InetSocketAddress(packet.getAddress(), 9945), 1500); return candidate; }
            catch (IOException e) { candidate.close(); throw e; }
        }
    }
    private void networkLoop() {
        while (alive) {
            try {
                Socket connected = connect(); connected.setTcpNoDelay(true); connected.setReceiveBufferSize(2*1024*1024); connected.setSoTimeout(5000);
                synchronized (this) { if (!alive) { connected.close(); break; } socket = connected; output = new DataOutputStream(connected.getOutputStream()); }
                if (!refreshReady.await(2, TimeUnit.SECONDS)) Log.w("VR4Mac", "XR refresh unavailable; using 72 Hz handshake fallback");
                JSONObject hello = new JSONObject().put("device", android.os.Build.MODEL).put("eye_w", eyeWidth).put("eye_h", eyeHeight)
                    .put("reference_space", stageSpace ? "stage" : "local").put("refresh_rates", new org.json.JSONArray().put((double)displayRefreshRate)).put("codecs", advertisedCodecs()).put("audio", audio != null && audio.available());
                int[] hevcSize = supportedHevcSize();
                if (hevcSize != null) hello.put("hevc_max_eye_w",hevcSize[0]).put("hevc_max_eye_h",hevcSize[1]);
                send(1, hello.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
                DataInputStream input = new DataInputStream(connected.getInputStream());
                needIdr = true;
                while (alive) {
                    int type = input.readUnsignedByte(); int length = Integer.reverseBytes(input.readInt());
                    if (length < 0 || length > 8*1024*1024) throw new IOException("Invalid packet size");
                    byte[] payload = new byte[length]; input.readFully(payload);
                    if (type == 2) {
                        try { configure(payload); }
                        catch (Exception failure) {
                            if ("hevc".equals(new JSONObject(new String(payload, java.nio.charset.StandardCharsets.UTF_8)).optString("codec"))) {
                                hevcDisabled = true;
                                Log.w("VR4Mac", "HEVC configuration failed; reconnecting with H264 only", failure);
                            }
                            throw failure;
                        }
                    }
                    else if (type == 4) { receivedBytes.addAndGet(length+5); decode(payload); }
                    else if (type == 6) { AudioPlayer player=audio; if(player != null) player.offer(payload); }
                    else if (type == 5 && length == 13) {
                        ByteBuffer b = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
                        int hand = b.get() & 255; float a=b.getFloat(), d=b.getFloat(), f=b.getFloat();
                        if (hand < 2 && Float.isFinite(a) && Float.isFinite(d) && Float.isFinite(f)) haptic(hand, Math.max(0,Math.min(1,a)), Math.max(0,Math.min(5,d)), Math.max(0,f));
                    }
                }
            } catch (Exception e) { if (alive) Log.w("VR4Mac", "Reconnecting: " + e); }
            finally { disconnect(); AudioPlayer player=audio; if(player != null) player.reset(); synchronized (codecLock) { releaseCodec(); } }
            if (alive) try { Thread.sleep(500); } catch (InterruptedException e) { break; }
        }
    }
    // Codec input/output calls and reconfiguration share a short lock. Network sends stay outside it.
    private void releaseCodec() {
        if (codec != null) {
            try { codec.stop(); } catch (Exception ignored) {}
            codec.release(); codec = null;
        }
        enqueueTimes.clear(); codecDescription = "none";
    }
    // Advertise only a hardware decoder that can sustain the requested stereo size/rate.
    private String hardwareHevcDecoder(int width, int height, int fps) {
        if (hevcDisabled) return null;
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (info.isEncoder() || !info.isHardwareAccelerated()) continue;
            try {
                MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType("video/hevc");
                boolean main = false;
                for (MediaCodecInfo.CodecProfileLevel profile : caps.profileLevels)
                    if (profile.profile == MediaCodecInfo.CodecProfileLevel.HEVCProfileMain) main = true;
                if (main && caps.getVideoCapabilities().areSizeAndRateSupported(width, height, fps)) return info.getName();
            } catch (IllegalArgumentException unsupported) { /* This decoder does not support HEVC. */ }
        }
        return null;
    }
    private int[] supportedHevcSize() {
        for (double scale : new double[] {1.5, 1.25, 1.0}) {
            int w=Math.min(2048,(int)(eyeWidth*scale)/32*32);
            int h=Math.min(2048,(int)(eyeHeight*scale)/32*32);
            if (w > 0 && h > 0 && hardwareHevcDecoder(w*2,h,72) != null) return new int[] {w,h};
        }
        return null;
    }
    private org.json.JSONArray advertisedCodecs() {
        org.json.JSONArray codecs = new org.json.JSONArray();
        if (supportedHevcSize() != null) codecs.put("hevc");
        return codecs.put("h264");
    }
    private void configure(byte[] payload) throws Exception {
        JSONObject config = new JSONObject(new String(payload, java.nio.charset.StandardCharsets.UTF_8));
        int width=config.getInt("eye_w"), height=config.getInt("eye_h");
        String selected = config.getString("codec");
        if (width < 1 || width > 2048 || height < 1 || height > 2048 || !("h264".equals(selected) || "hevc".equals(selected))) throw new IOException("Unsupported CONFIG");
        String mime = "hevc".equals(selected) ? "video/hevc" : "video/avc";
        String hevcName = "hevc".equals(selected) ? hardwareHevcDecoder(width*2, height, config.optInt("fps",72)) : null;
        if ("hevc".equals(selected) && hevcName == null) throw new IOException("No hardware HEVC decoder for CONFIG size/rate");
        synchronized (codecLock) {
            releaseCodec();
            codec = hevcName != null ? MediaCodec.createByCodecName(hevcName) : MediaCodec.createDecoderByType(mime);
            MediaFormat format = MediaFormat.createVideoFormat(mime, width*2, height);
            format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 8*1024*1024);
            if (android.os.Build.VERSION.SDK_INT >= 30) format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
            format.setInteger(MediaFormat.KEY_PRIORITY, 0);
            format.setFloat(MediaFormat.KEY_OPERATING_RATE, Math.min(144, Math.max(72, config.optInt("fps",72)*2)));
            // Older Quest firmware is API 29: use Qualcomm's vendor option used by ALVR.
            String decoderName = codec.getName();
            boolean qualcomm = decoderName.startsWith("OMX.qcom.") || decoderName.startsWith("c2.qti.");
            String vendorLowLatency = "vendor.qti-ext-dec-low-latency.enable";
            if (qualcomm) format.setInteger(vendorLowLatency, 1);
            try { codec.configure(format, surface, null, 0); }
            catch (RuntimeException unsupported) {
                if (!qualcomm) throw unsupported;
                Log.w("VR4Mac", "Vendor low-latency option rejected; retrying standard decoder", unsupported);
                codec.release(); codec = hevcName != null ? MediaCodec.createByCodecName(hevcName) : MediaCodec.createDecoderByType(mime);
                format.removeKey(vendorLowLatency); qualcomm=false;
                codec.configure(format, surface, null, 0);
            }
            codec.start(); needIdr = true; idrWaitSince=System.nanoTime(); lastIdrRequest=idrWaitSince;
            codecDescription = codec.getName() + ", codec=" + selected + ", qti-low-latency-requested=" + qualcomm;
            Log.i("VR4Mac", codecDescription);
        }
        Log.i("VR4Mac", "Decoder configured: " + width*2 + "x" + height + " " + selected + ", low latency");
        send(7, new byte[0]);
    }
    private void decode(byte[] payload) throws Exception {
        long receivedAt = System.nanoTime();
        if (payload.length < 18) return;
        ByteBuffer b=ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        b.getLong(); long predicted=b.getLong(); boolean idr=(b.get() & 1)!=0;
        boolean requestIdr = false;
        synchronized (codecLock) {
            if (codec == null) return;
            if (needIdr && !idr) {
                droppedFrames.incrementAndGet();
            } else {
            int index=codec.dequeueInputBuffer(1000);
            if (index < 0) {
                if (!needIdr) idrWaitSince=receivedAt;
                needIdr=true; droppedFrames.incrementAndGet();
            }
            else {
                ByteBuffer target=codec.getInputBuffer(index);
                if (target == null || target.capacity() < b.remaining()) throw new IOException("Video packet exceeds decoder buffer");
                target.clear(); target.put(b);
                long pts=predicted/1000;
                codec.queueInputBuffer(index,0,payload.length-17,pts,0);
                enqueueTimes.put(pts,new long[] {receivedAt, System.nanoTime()});
                while (enqueueTimes.size() > 64) enqueueTimes.removeAt(0);
                needIdr=false; idrWaitSince=0;
            }
            }
            if (needIdr) {
                if (idrWaitSince == 0) idrWaitSince=receivedAt;
                if (receivedAt-idrWaitSince > 2_000_000_000L) {
                    if (codecDescription.contains("codec=hevc")) hevcDisabled=true;
                    throw new IOException("Decoder recovery timed out; resetting connection and decoder (HEVC falls back to H264)");
                }
                if (receivedAt-lastIdrRequest >= 250_000_000L) { lastIdrRequest=receivedAt; requestIdr=true; }
            }
        }
        if (requestIdr) send(7,new byte[0]);
    }
    private void outputLoop() {
        long reportTime = System.nanoTime();
        while (alive) {
            try {
                synchronized (codecLock) { if (codec != null) drain(); }
            } catch (Exception error) { Log.w("VR4Mac", "Decoder output failed", error); disconnect(); }
            long now=System.nanoTime();
            if (now-reportTime >= 5_000_000_000L) {
                long count=decodedFrames.getAndSet(0), nanos=decodeNanos.getAndSet(0);
                double seconds=(now-reportTime)/1e9;
                Log.i("VR4Mac", String.format(java.util.Locale.US,
                    "Stream %.1f Mbps, decode %.1f fps, receive-to-release %.1f ms (input %.2f, decoder-hold %.2f, drain %.2f), dropped %d / %.1fs; %s",
                    receivedBytes.getAndSet(0)*8/seconds/1e6, count/seconds,
                    count > 0 ? nanos/(double)count/1e6 : 0,
                    averageMillis(inputNanos.getAndSet(0),count), averageMillis(holdNanos.getAndSet(0),count),
                    averageMillis(drainNanos.getAndSet(0),count), droppedFrames.getAndSet(0), seconds, codecDescription));
                reportTime=now;
            }
            try { Thread.sleep(2); } catch (InterruptedException e) { break; }
        }
    }
    private static double averageMillis(long nanos, long count) { return count > 0 ? nanos/(double)count/1e6 : 0; }
    private void drain() {
        MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
        int newest=-1, index; long newestPts=0, newestOut=0;
        while ((index=codec.dequeueOutputBuffer(info,0)) >= 0) {
            if (newest >= 0) { codec.releaseOutputBuffer(newest,false); droppedFrames.incrementAndGet(); enqueueTimes.remove(newestPts); }
            newest=index; newestPts=info.presentationTimeUs; newestOut=System.nanoTime();
        }
        if (newest >= 0) {
            long[] times=enqueueTimes.get(newestPts);
            codec.releaseOutputBuffer(newest,true);
            long released=System.nanoTime();
            if (times != null) {
                inputNanos.addAndGet(times[1]-times[0]);
                holdNanos.addAndGet(newestOut-times[1]);
                drainNanos.addAndGet(released-newestOut);
                decodeNanos.addAndGet(released-times[0]); decodedFrames.incrementAndGet(); enqueueTimes.remove(newestPts);
            }
        }
    }
}
