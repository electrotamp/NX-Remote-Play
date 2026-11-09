#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "defines.h"

// This header defines the nxremoteplay protocol used for the initial handshake over USB and TCPBridge
// The handshake happens seprately for both streams

// The client sends a ProtohandshakeRequest to the endpoint, nxremoteplay replies with a single u32 result code.
// If the result code is Handshake_OK, the client can start streaming. Otherwise the connection is closed.

#define PROTO_REQUEST_MAGIC 0xAAAAAAAA

#define PROTO_HANDSHAKE_HELLO "NxRemotePlay|" NXREMOTEPLAY_PROTOCOL_VERSION

// This is sent by the console as soon as a client connects to communicate the protocol version.
// The string is null terminated.
_Static_assert(sizeof(PROTO_HANDSHAKE_HELLO) == 16, "Wrong size for the protocol hello message");

typedef enum
{
    Handshake_UnknownFailure = 0,
    Handshake_WrongVersion = 1,
    Handshake_InvalidArg = 2,
    Handshake_InvalidSize = 3,
    Handshake_InvalidMeta = 4,
    Handshake_WrongMagic = 5,
    Handshake_Ok = 6,
    Hanshake_InvalidChannel = 7,
} ProtoHandshakeResultCode;

typedef struct
{
    uint32_t Code; // ProtoHandshakeResultCode
    struct
    {
        uint32_t QueryResult; // this is a libnx result. Special value UINT32_MAX means not requested
        uint64_t ApplicationSize, ApplicationUsed;
        uint64_t AppletSize, AppletUsed;
        uint64_t SystemSize, SystemUsed;
        uint64_t SystemUnsafeSize, SystemUnsafeUsed;
    } __attribute__((packed)) MemoryPools;
} __attribute__((packed)) ProtoHandshakeResponse;

_Static_assert(sizeof(ProtoHandshakeResponse) == 72);

typedef struct
{
    ProtoHandshakeResponse Result;
    bool RequestedAudio;
    bool RequestedVideo;
    //  (multi-viewer): the client's own request, so a session that serves several clients can apply each
    // one's options itself instead of through the global capture settings. Valid when Result.Code == Handshake_Ok.
    uint8_t VideoFlags;    // ProtoMetaVideoFlags
    uint8_t AudioBatching; // as requested (0..MaxABatching)
    bool DeliveryFeedback; // Opt-in shared-network path only; no wire struct growth.
    bool OpusAudio;        // Audio goes out as Opus packets, not PCM; needs RequestedAudio.
    // NXRP: non-zero when this session was offered UDP video (value sent in
    // MemoryPools.ApplicationSize, see UdpmVideo.h). Set by the stream handshake, not ProtoHandshake.
    uint64_t UdpOffer;
} ProtoParsedHandshake;

enum ProtoMetaFlags // 8 bits
{
    ProtoMeta_Video = (1 << 0),
    ProtoMeta_Audio = (1 << 1),
};

enum ExtraFeatureFlags // 8 bits
{
    // Request the console to turn off the display during streaming
    ExtraFeatureFlags_TurnOffScreen = (1 << 0),
    // Request a memory report before starting to stream to debug the state of the console
    ExtraFeatureFlags_MemoryDiag = (1 << 1),
    // USB-only extension. Acknowledged by USB_CAPABILITY_MARKER in QueryResult
    // and a 256-byte capability record following the normal 72-byte response.
    ExtraFeatureFlags_UsbControl = (1 << 2),
    // Opt-in owner-proof responses multiplexed alongside USB media. Requires UsbControl.
    ExtraFeatureFlags_UsbOwner = (1 << 3),
    ExtraFeatureFlags_DeliveryFeedback = (1 << 4),
    // Bit 5 is UDPM_FEATURE_FLAG and bit 6 asks for the rendezvous path (see PunchProbe.c).
    // The client can decode Opus game audio (PacketMeta_Audio_Opus).
    ExtraFeatureFlags_OpusAudio = (1 << 7),
};

// Sent by the client to set up streaming
struct ProtoHandshakeRequest
{
    uint32_t Magic;
    // Two ascii chars from NXREMOTEPLAY_PROTOCOL_VERSION
    uint16_t ProtoVer;

    uint8_t MetaFlags; // ProtoMetaFlags

    uint8_t VideoFlags; // ProtoMetaVideoFlags
    uint8_t AudioBatching;

    uint8_t FeatureFlags;
    uint8_t Reserved[6];
};

_Static_assert(sizeof(struct ProtoHandshakeRequest) == 16);

#define PROTO_HANDSHAKE_SIZE sizeof(struct ProtoHandshakeRequest)

typedef enum
{
    ProtoHandshakeAccept_Any,
    ProtoHandshakeAccept_Video,
    ProtoHandshakeAccept_Audio,
} ProtoHandshakeAccept;

// Verifies the handshake and if valid sets the global state of the client
ProtoParsedHandshake ProtoHandshake(ProtoHandshakeAccept config, uint8_t *data, int length);

// Must be called every time ProtoHandshake returned OK, it is safe to call multiple times
void ProtoClientGlobalStateDisconnected();
