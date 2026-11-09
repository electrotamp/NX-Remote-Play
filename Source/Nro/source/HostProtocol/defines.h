#pragma once

// This is the main version shown to the user
#define NXREMOTEPLAY_VERSION_STRING  "1.0.0-beta.1"
//This is a version for the NxRemotePlay net and usb protocol, it's included in the beacon and may be shown in the UI. This is ascii so it's easy to insert in beacons for example as the serial of the USB device
#define NXREMOTEPLAY_PROTOCOL_VERSION "03"

#define NXREMOTEPLAY_CRASH_MODULEID 0x69

#define NXREMOTEPLAY_CONTENT_ID 0x4E5852656D6F7465

#define ERR_USB_OUT_OF_MEMORY MAKERESULT(NXREMOTEPLAY_CRASH_MODULEID, 20)

#define ERR_IPC_INVHEADER MAKERESULT(NXREMOTEPLAY_CRASH_MODULEID, 9)
#define ERR_IPC_INVSIZE MAKERESULT(NXREMOTEPLAY_CRASH_MODULEID, 10)
#define ERR_IPC_INVMAGIC MAKERESULT(NXREMOTEPLAY_CRASH_MODULEID, 11)
#define ERR_IPC_UNKCMD MAKERESULT(NXREMOTEPLAY_CRASH_MODULEID, 13)

#define ERR_MAIN_UNKMODE MAKERESULT(NXREMOTEPLAY_CRASH_MODULEID, 14)
#define ERR_MAIN_UNKMODESET MAKERESULT(NXREMOTEPLAY_CRASH_MODULEID, 15)
#define ERR_MAIN_UNEXPECTED_MODE MAKERESULT(NXREMOTEPLAY_CRASH_MODULEID, 22)
#define ERR_MAIN_NOTRUNNING MAKERESULT(NXREMOTEPLAY_CRASH_MODULEID, 18)
#define ERR_INIT_FAILED MAKERESULT(NXREMOTEPLAY_CRASH_MODULEID, 21)

#define ERR_HIPC_UNKREQ MAKERESULT(11, 403)

#define TYPE_MODE_USB 1
#define TYPE_MODE_NULL 3

// CMD_GET_HOSTING_STATE returns a HostingSnapshot (HostingSnapshot.h).
// CMD_REQUEST_NEW_CODE takes/returns nothing - just forces a fresh REGISTER.
#define CMD_GET_HOSTING_STATE 104
#define CMD_REQUEST_NEW_CODE 105

// CMD_SET_HOSTING_ENABLED takes a single u32 (0/1) input payload - turns the
// "connect anytime" background feature fully on/off (no REGISTER/REFRESH
// traffic at all while off) and persists the choice across reboots.
#define CMD_SET_HOSTING_ENABLED 106

// Rendezvous server: GET returns/SET takes a 64-byte NUL-terminated
// "host:port" (SET validates and applies it, re-registers; the NRO persists it in server.txt).
#define CMD_GET_SERVER 107
#define CMD_SET_SERVER 108
