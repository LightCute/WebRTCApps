#!/bin/bash
set -e

cd "$(dirname "$0")/../../.."

echo "=== Generating GN args ==="
gn gen out/client_android_arm64 --args='
target_os = "android"
target_cpu = "arm64"
is_debug = true
is_component_build = false
rtc_include_tests = false
android_static_analysis = "off"
'

echo "=== Building client_android ==="
ninja -C out/client_android_arm64 apps/peerconnection/client_android:client_android

APK_PATH="out/client_android_arm64/apks/ClientAndroid.apk"
if [ -f "$APK_PATH" ]; then
    echo ""
    echo "=== BUILD SUCCESS ==="
    echo "APK: $APK_PATH"
else
    echo ""
    echo "=== BUILD FAILED: APK not found ==="
    echo "Check: out/client_android_arm64/apks/"
    ls out/client_android_arm64/apks/ 2>/dev/null || echo "(no apks directory)"
    exit 1
fi
