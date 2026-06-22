package org.light.webrtc;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.method.ScrollingMovementMethod;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;

public class MainActivity extends Activity {
    static { System.loadLibrary("client_android_jni"); }

    private TextView logView;
    private Button connectBtn, disconnectBtn, callBtn, hangupBtn, sendBtn;
    private EditText serverInput, portInput, nameInput, peerIdInput, msgInput;
    private Handler mainHandler;

    // ── Native methods (implemented in jni_bridge.cc) ──
    private native void nativeConnect(String server, int port, String name);
    private native void nativeDisconnect();
    private native void nativeCall(int peerId);
    private native void nativeHangup();
    private native void nativeSendData(String msg);

    // ── C++→Java callbacks (called from JNI on arbitrary thread) ──
    private void onSignedIn() {
        runOnUi(() -> {
            log("Signed in to server");
            connectBtn.setEnabled(false);
            disconnectBtn.setEnabled(true);
            callBtn.setEnabled(true);
        });
    }

    private void onDisconnected() {
        runOnUi(() -> {
            log("Disconnected from server");
            connectBtn.setEnabled(true);
            disconnectBtn.setEnabled(false);
            callBtn.setEnabled(false);
            hangupBtn.setEnabled(false);
            sendBtn.setEnabled(false);
        });
    }

    private void onPeerConnected(int peerId, String name) {
        runOnUi(() -> {
            log("Peer online: id=" + peerId + " name=" + name);
            peerIdInput.setText(String.valueOf(peerId));
        });
    }

    private void onPeerDisconnected(int peerId) {
        runOnUi(() -> log("Peer offline: id=" + peerId));
    }

    private void onCallConnected() {
        runOnUi(() -> {
            log(">>> CALL CONNECTED <<<");
            hangupBtn.setEnabled(true);
            sendBtn.setEnabled(true);
        });
    }

    private void onCallDisconnected() {
        runOnUi(() -> {
            log("Call disconnected");
            hangupBtn.setEnabled(false);
            sendBtn.setEnabled(false);
        });
    }

    private void onDataReceived(String msg) {
        runOnUi(() -> log("Peer: " + msg));
    }

    private void onError(String error) {
        runOnUi(() -> log("ERROR: " + error));
    }

    // ── Helpers ──
    private void log(String text) {
        logView.append(text + "\n");
    }

    private void runOnUi(Runnable r) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            r.run();
        } else {
            mainHandler.post(r);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        mainHandler = new Handler(Looper.getMainLooper());

        logView = findViewById(R.id.log_view);
        logView.setMovementMethod(new ScrollingMovementMethod());

        serverInput = findViewById(R.id.server_input);
        portInput = findViewById(R.id.port_input);
        nameInput = findViewById(R.id.name_input);
        peerIdInput = findViewById(R.id.peer_id_input);
        msgInput = findViewById(R.id.msg_input);

        connectBtn = findViewById(R.id.connect_btn);
        disconnectBtn = findViewById(R.id.disconnect_btn);
        callBtn = findViewById(R.id.call_btn);
        hangupBtn = findViewById(R.id.hangup_btn);
        sendBtn = findViewById(R.id.send_btn);

        connectBtn.setOnClickListener(v -> {
            String server = serverInput.getText().toString().trim();
            int port = Integer.parseInt(portInput.getText().toString().trim());
            String name = nameInput.getText().toString().trim();
            log("Connecting to " + server + ":" + port + " as " + name);
            new Thread(() -> nativeConnect(server, port, name)).start();
        });

        disconnectBtn.setOnClickListener(v -> {
            log("Disconnecting...");
            new Thread(() -> nativeDisconnect()).start();
        });

        callBtn.setOnClickListener(v -> {
            int peerId = Integer.parseInt(peerIdInput.getText().toString().trim());
            log("Calling peer " + peerId);
            new Thread(() -> nativeCall(peerId)).start();
        });

        hangupBtn.setOnClickListener(v -> {
            log("Hanging up");
            new Thread(() -> nativeHangup()).start();
        });

        sendBtn.setOnClickListener(v -> {
            String msg = msgInput.getText().toString().trim();
            if (!msg.isEmpty()) {
                log("You: " + msg);
                new Thread(() -> nativeSendData(msg)).start();
                msgInput.setText("");
            }
        });
    }
}
