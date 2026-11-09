#pragma once
#include <string>
#include <cstdint>
#include <cstddef>

// TCP hole-punch Connector for the NRO  - the C++ counterpart of the
// phone app's PunchSession.kt, speaking the exact same proven protocol:
//   UDP  PUNCHWAKE <id> <pw>  -> Rendezvous server (console picks it up on its next 30 s REFRESH)
//   TCP  PUNCHJOIN <id> <pw>  -> "OK <console ip:port>" (retried while the console isn't ready)
//   both sides connect() to each other from the same local port (simultaneous open)
//   NxRemotePlay video handshake, then a stream of 18-byte-header packets: H.264 Annex-B NAL units,
//   or a JPEG still (PacketMeta_Content_Screenshot) while the console shows HOME.
//   Input goes the other way on the same socket: 5-byte touch / 17-byte controller packets
//   (see PunchProbe.c on the sysmodule).
//
// A worker thread does all the blocking network work; Poll() (called once per frame from the UI
// thread, via Network::Rendezvous::Connector::Poll) hands decoded-side callbacks their data, so
// the decoder / GL texture uploads stay on the UI thread exactly as ConnectorMedia expects.
//
namespace Network::Punch
{
enum class State
{
    Idle,
    Requesting,
    Paired,
    Handshaking,
    Connected,
    Error
};

// Starts a connection attempt to the console registered as <id>/<password> on the given
// Rendezvous server. Ignored while an attempt/connection is already in progress.
void Connect(const std::string &serverHost, uint16_t serverPort, const std::string &id, const std::string &password);
void Poll();
// Ends any attempt/connection and returns to Idle.
void Disconnect();

State GetState();
const std::string &GetErrorMessage();
// The current step ("Waiting for the console to answer (attempt 3)...") for on-screen status.
std::string GetStatusText();
// Writes a line to the connection log (nro_log.txt + UDP collector), same as the connect steps.
void Note(const std::string &line);

// Each H.264 NAL unit / access unit (Annex-B) from the stream, and each complete JPEG still.
// Invoked from Poll() on the UI thread.
void SetOnVideoFrame(void (*callback)(uint8_t *data, size_t size, void *userdata), void *userdata);
void SetOnStillFrame(void (*callback)(uint8_t *data, size_t size, void *userdata), void *userdata);

// Full input state each call (cheap): touch is normalized 0..1, buttons are HidNpadButton
// bits, sticks are -0x7FFF..0x7FFF. Sends only on change. False if not Connected.
bool SendInputState(bool touchActive, float touchX, float touchY, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx,
                    int32_t ry);

// HOME on the console (stream menu): press, ~150 ms, release. No-op unless Connected.
void SendHomeTap();

// Stops the worker thread; call once at app shutdown. Safe if never connected.
void Shutdown();
} // namespace Network::Punch
