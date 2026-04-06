# Apps PeerConnection Client

This directory contains a standalone Linux/ChromeOS peer connection client
modeled after `examples/peerconnection/client`, but kept self-contained under
`apps/peerconnection/client`.

## Build

Because the root `BUILD.gn` is intentionally left untouched, build this target
by overriding GN's root target:

```bash
cd /home/light/webrtc-checkout/src/apps/peerconnection/client
./build.sh
```

Or run the commands manually:

```bash
cd /home/light/webrtc-checkout/src
gn gen out/apps_peerconnection_client \
  --root="$PWD" \
  --root-target=//apps/peerconnection/client:peerconnection_client \
  --args='is_debug=true rtc_build_examples=false rtc_build_tools=false rtc_include_tests=true rtc_include_pulse_audio=true'
ninja -C out/apps_peerconnection_client peerconnection_client
```

## Run

```bash
/home/light/webrtc-checkout/src/out/apps_peerconnection_client/apps_peerconnection_client
```

Useful flags:

```bash
--server=localhost
--port=8888
--autoconnect
--autocall
--force_fieldtrials='WebRTC-FooFeature/Enabled/'
```

## Signaling Server

This client speaks the same simple HTTP signaling protocol as the original
example. You can run the existing example signaling server from another build
directory, for example:

```bash
cd /home/light/webrtc-checkout/src
gn gen out/peerconnection_server \
  --root="$PWD" \
  --root-target=//examples:peerconnection_server
ninja -C out/peerconnection_server peerconnection_server
./out/peerconnection_server/peerconnection_server
```

## Notes

- The app keeps the original GTK UI and signaling behavior.
- The audio device module is created on the worker thread to avoid the Linux
  PulseAudio thread-affinity issue seen in the original example path.
