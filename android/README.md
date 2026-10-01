# VR4Mac Quest client

Native arm64 Android/OpenXR client for Quest 1 and Quest 2. The Mac companion listens on TCP 9945. The client sends tracking and Touch input, decodes side-by-side H.264 with MediaCodec, and renders each eye into an OpenXR projection swapchain. Projection poses come from the original tracking sample echoed by the server, rather than the current pose, so the headset compositor can timewarp the frame.

## Build

Requirements: JDK 21, Android SDK platform 34, NDK 27.2.12479018, CMake 3.22.1. The Gradle wrapper downloads Gradle 8.9. The Khronos OpenXR loader is a pinned Maven dependency. The sibling `../common/vr4mac.h` is required.

```sh
export JAVA_HOME=/opt/homebrew/opt/openjdk@21
export ANDROID_HOME="$HOME/Library/Android/sdk"
./gradlew assembleDebug
```

APK: `app/build/outputs/apk/debug/app-debug.apk`. A ready-to-install copy is also provided at `VR4Mac.apk`. This is a development APK, signed with Android's debug key.

## Install and connect

Enable developer mode on the headset and authorize USB debugging. Launch the Mac companion first.

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb reverse tcp:9945 tcp:9945
adb shell am start -n com.vr4mac.client/.MainActivity
```

Find VR4Mac in the headset's Unknown Sources library. The client first tries the USB reverse endpoint, then listens for the Mac's UDP discovery broadcast on port 9944. For Wi-Fi networks that block broadcasts, use an explicit Mac address:

```sh
adb shell am force-stop com.vr4mac.client
adb shell am start -n com.vr4mac.client/.MainActivity --es host 192.168.1.100
```

Allow TCP 9945 through the Mac firewall. Both devices must be on the same trusted local network. This development transport has no encryption or authentication.

## Protocol and behavior

Wire definitions are shared in `../common/vr4mac.h`. Packets have a one-byte type and a four-byte little-endian length. TRACKING is 284 bytes. VIDEO begins with the 17-byte frame/time/flags header followed by Annex-B H.264, with SPS/PPS preceding every IDR. Video is limited to 8 MiB per packet. The client requests an IDR after CONFIG and whenever input starvation forces it to skip a frame. It advertises H.264 and 72 Hz; HEVC and dynamic refresh-rate negotiation are not implemented.

Tracking uses STAGE coordinates when available and LOCAL otherwise. Tracking packets replace older pending samples rather than accumulating. The renderer stores a bounded history, matches decoder presentation timestamps to original OpenXR predicted times at microsecond precision, and refuses to display frames whose pose is missing or more than one second old. An absent stream shows a dark background. Controller inputs are cleared while the session is not focused. Haptics are queued for application on the XR thread.

## Verification

A successful APK build verifies Java compilation, native compilation/linking, manifest merging, and packaging. Hardware verification must cover Quest 1 and Quest 2 startup, tracking, stereo orientation, timewarp during head movement, controller mappings, haptics, USB and Wi-Fi reconnection, and suspend/resume. A build alone does not demonstrate these behaviors or Windows game compatibility.

View device diagnostics with `adb logcat -s VR4Mac OpenXR-Loader`.

On September 29, 2026, the debug APK was installed on Quest 1 over USB. The native runtime reported 72/72 Hz; the Qualcomm AVC decoder was active, tracking advanced at approximately 72 Hz, and a headset screenshot confirmed a stereo dashboard, floor grid, and controller pointers with upright text. The Mac wire test passed 595 frames with exact timestamps, and a synthetic D3D11 OpenXR test under Wine submitted 300 frames. These checks do not establish compatibility with actual Windows VR games, visual comfort, Quest 2, Wi-Fi, or haptics.

Keep `com.oculus.supportedDevices` set to `quest|quest2`: the older Quest 1 runtime aborts during instance creation if the list contains an unknown model such as `quest3`.

The client now drains MediaCodec output independently every 2 ms, instead of waiting for another network packet. It keeps the newest available output, uses real-time decoder priority and a high operating-rate hint, requests the Qualcomm vendor low-latency option with a configuration fallback, and requests the standard low-latency option on API 30+. Every five seconds `VR4Mac` logs received Mbps, decoded FPS, packet-receipt-to-decoder-output time, dropped frames, codec name, and whether the Qualcomm option was requested. The vendor request does not prove that the driver applies it.

The renderer prefers an sRGB swapchain and converts decoded gamma-encoded RGB to linear light before framebuffer output. This corrected the washed-out colors reported on Quest 1. Fragment shader texture coordinates and color arithmetic use high precision.

Final Quest 1 USB checks observed approximately 71–72 decoded FPS, 0–1 dropped frames per five seconds, and 23–25 ms from complete video-packet receipt to decoder output. These timings exclude Mac rendering, encoding, transport, and headset display; they are not total motion-to-photon latency. The Qualcomm option did not materially reduce this measured delay in the A/B test. The Mac encoder quality ceiling increased actual stream bandwidth from roughly 7 Mbps to 21–48 Mbps depending on scene content.

A 125% H.264 test at 3008×1664 and 72 FPS failed on the Mac encoder: its macroblock rate exceeded the active encoder's H.264 level 5.1 limit. The working stream was restored to 2432×1344 at 72 FPS, with the Mac applying a size/rate cap. Higher resolution and real Windows Steam game compatibility remain separate, unverified work.

### HEVC negotiation

The client advertises `hevc` before `h264` only when a hardware HEVC Main decoder reports support for the recommended stereo size at 72 Hz. CONFIG selects `hevc` or `h264`; HEVC uses the same VIDEO header and Annex-B payload with VPS/SPS/PPS at random-access frames. The configured size and rate are checked again. A failed HEVC configuration reconnects with H.264 only for the rest of the activity lifetime. Larger render scales remain subject to decoder capability and live throughput testing. This client build passed compilation; end-to-end HEVC is awaiting the Mac encoder implementation and headset measurement.

Client five-second diagnostics split complete VIDEO receipt to output release into `input` (receipt to queueInputBuffer return, including lock/wait/copy), `decoder-hold` (queue return to output dequeue, including driver buffering and output polling), and `drain` (output dequeue to releaseOutputBuffer return). All averages use the same matched rendered-frame sample population; dropped outputs are excluded. These measurements exclude TCP packet assembly before complete receipt, SurfaceTexture latch, compositor presentation, and motion-to-photon latency. The decoder-hold metric cannot isolate driver time from the approximately 2ms output polling interval.

HEVC HELLO also supplies `hevc_max_eye_w` and `hevc_max_eye_h`, the largest supported candidate among 150%, 125%, and 100% of recommended size, clamped to 2048 per eye and aligned down to32 as on the Mac. The server consumes the pair to bound its render scale. This metadata is an advertised capability, not a throughput guarantee. Recovery now retries IDR requests every250ms while waiting; after2s without a successfully queued recovery frame it reconnects/recreates the decoder, disabling HEVC for the activity lifetime if HEVC stalled. H.264 remains the default Mac codec.

### Audio v1

HELLO `audio: true` is sent when AudioTrack initializes. Server packet6 is an informational u64 capture timestamp followed by 48kHz stereo interleaved signed16 little-endian PCM. A dedicated audio thread writes to a low-latency-requested AudioTrack; the application queue is bounded to40ms by bytes and drops oldest chunks on overflow. Device AudioTrack buffering is additional and may exceed the requested20ms minimum; this is not an end-to-end latency guarantee. Malformed/unreasonably large chunks are ignored. Reconnect clears queued PCM and flushes the output on the audio thread. Playback and audible game/system capture still require live validation.
