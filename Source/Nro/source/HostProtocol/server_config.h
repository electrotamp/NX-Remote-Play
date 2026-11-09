#pragma once
// Shared server configuration. ONE file, /config/nxremoteplay/server.txt on the SD card, is the
// single source of truth for which rendezvous server is used. The NRO is the only writer; the sysmodule and
// the NRO both parse it with THIS header and apply the result to themselves.
//
// This header is deliberately dependency-free C (no libnx, no printf family) and exists as an identical copy in
// Source/Nro/source/HostProtocol/ and Source/Host/source/net/. Tools/Test-Compatibility.py fails if the copies
// differ. File format (plain ASCII, one key=value per line, unknown keys ignored, CR/LF tolerated):
//
//   mode=default            default  -> use the line "default="; custom -> use the line "custom="
//   default=play.nxremoteplay.com:7921
//   custom=my.server.example:9000
//
// Server text = IPv4 address or DNS name, optional ":port" (7921 when omitted), kept exactly as typed. The
// effective server is always returned as "host:port" with the explicit port.
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define SERVERCFG_TEXT_MAX 64
#define SERVERCFG_DEFAULT_PORT 7921
#define SERVERCFG_BUILTIN_DEFAULT "play.nxremoteplay.com:7921"
#define SERVERCFG_PATH "/config/nxremoteplay/server.txt"

typedef struct {
	bool useDefault;                       // toggle: true = "mode=default"
	char defaultServer[SERVERCFG_TEXT_MAX]; // normalised host:port
	char customServer[SERVERCFG_TEXT_MAX];  // normalised host:port, or "" when none
} ServerConfig;

static inline bool ServerCfg_IsDigit(char c) { return c >= '0' && c <= '9'; }

// Validates and normalises "host[:port]" into out as "host:port". False if invalid or it does not fit.
static inline bool ServerCfg_Normalize(const char* typed, char* out, size_t outSize)
{
	char buf[SERVERCFG_TEXT_MAX];
	size_t n = 0;
	if (!typed) return false;
	while (*typed == ' ' || *typed == '\t') typed++;
	while (typed[n] && typed[n] != '\r' && typed[n] != '\n' && n < sizeof(buf) - 1) { buf[n] = typed[n]; n++; }
	if (typed[n] && typed[n] != '\r' && typed[n] != '\n') return false; // too long
	while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) n--;
	buf[n] = 0;
	if (n == 0) return false;

	unsigned long port = SERVERCFG_DEFAULT_PORT;
	size_t hostLen = n;
	size_t colon = n;
	for (size_t i = n; i > 0; i--) if (buf[i - 1] == ':') { colon = i - 1; break; }
	if (colon < n)
	{
		hostLen = colon;
		if (colon + 1 >= n || n - (colon + 1) > 5) return false;
		port = 0;
		for (size_t i = colon + 1; i < n; i++)
		{
			if (!ServerCfg_IsDigit(buf[i])) return false;
			port = port * 10 + (unsigned long)(buf[i] - '0');
		}
		if (port < 1 || port > 65535) return false;
	}
	if (hostLen == 0 || hostLen > 60) return false;
	// Every dot-separated label must be non-empty and must not start or end with '-'.
	{
		size_t labelStart = 0;
		for (size_t i = 0; i <= hostLen; i++)
		{
			if (i == hostLen || buf[i] == '.')
			{
				if (i == labelStart || buf[labelStart] == '-' || buf[i - 1] == '-') return false;
				labelStart = i + 1;
			}
		}
	}
	for (size_t i = 0; i < hostLen; i++)
	{
		const char c = buf[i];
		if (!(ServerCfg_IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) return false;
	}
	// Dotted numeric text must be a valid IPv4 address (4 octets <= 255), not a name that merely looks like one.
	{
		bool numeric = true; int dots = 0;
		for (size_t i = 0; i < hostLen; i++) { if (buf[i] == '.') dots++; else if (!ServerCfg_IsDigit(buf[i])) numeric = false; }
		if (numeric)
		{
			if (dots != 3) return false;
			unsigned long v = 0; int digits = 0;
			for (size_t i = 0; i <= hostLen; i++)
			{
				if (i == hostLen || buf[i] == '.')
				{
					if (digits == 0 || v > 255) return false;
					v = 0; digits = 0;
				}
				else { v = v * 10 + (unsigned long)(buf[i] - '0'); digits++; if (digits > 3) return false; }
			}
		}
	}
	// out = host ":" port
	char portText[8]; int pl = 0; unsigned long q = port; char rev[8]; int rl = 0;
	do { rev[rl++] = (char)('0' + q % 10); q /= 10; } while (q);
	while (rl) portText[pl++] = rev[--rl];
	portText[pl] = 0;
	if (hostLen + 1 + (size_t)pl + 1 > outSize) return false;
	memcpy(out, buf, hostLen); out[hostLen] = ':'; memcpy(out + hostLen + 1, portText, (size_t)pl + 1);
	return true;
}

static inline void ServerCfg_Defaults(ServerConfig* c)
{
	c->useDefault = true;
	memset(c->defaultServer, 0, sizeof(c->defaultServer));
	memset(c->customServer, 0, sizeof(c->customServer));
	strcpy(c->defaultServer, SERVERCFG_BUILTIN_DEFAULT);
}

// Parses the file text. Always leaves *c usable (built-in default when keys are missing/invalid).
// Returns true if at least a valid "mode=" line was found (i.e. the file really configured something).
static inline bool ServerCfg_Parse(const char* text, ServerConfig* c)
{
	ServerCfg_Defaults(c);
	bool sawMode = false;
	if (!text) return false;
	const char* p = text;
	while (*p)
	{
		const char* eol = p;
		while (*eol && *eol != '\n') eol++;
		char line[96]; size_t n = (size_t)(eol - p);
		if (n >= sizeof(line)) n = sizeof(line) - 1;
		memcpy(line, p, n); line[n] = 0;
		while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
		char* eq = strchr(line, '=');
		if (eq)
		{
			*eq = 0; const char* key = line; const char* val = eq + 1;
			while (*val == ' ' || *val == '\t') val++;
			char norm[SERVERCFG_TEXT_MAX];
			if (strcmp(key, "mode") == 0)
			{
				if (strcmp(val, "default") == 0) { c->useDefault = true; sawMode = true; }
				else if (strcmp(val, "custom") == 0) { c->useDefault = false; sawMode = true; }
			}
			else if (strcmp(key, "default") == 0) { if (ServerCfg_Normalize(val, norm, sizeof(norm))) strcpy(c->defaultServer, norm); }
			else if (strcmp(key, "custom") == 0)  { if (ServerCfg_Normalize(val, norm, sizeof(norm))) strcpy(c->customServer, norm); }
		}
		p = *eol ? eol + 1 : eol;
	}
	return sawMode;
}

// The server to actually use: the default when the toggle is ON (or no valid custom exists), else the custom one.
static inline const char* ServerCfg_Effective(const ServerConfig* c)
{
	if (c->useDefault || c->customServer[0] == 0) return c->defaultServer;
	return c->customServer;
}

// Serialises to the file text (always ends with a newline). Returns the length, or 0 if outSize is too small.
static inline size_t ServerCfg_Format(const ServerConfig* c, char* out, size_t outSize)
{
	const char* mode = c->useDefault ? "mode=default\n" : "mode=custom\n";
	const size_t need = strlen(mode) + 8 + strlen(c->defaultServer) + 1 + 7 + strlen(c->customServer) + 1 + 1;
	if (need > outSize) return 0;
	size_t o = 0;
	strcpy(out + o, mode); o += strlen(mode);
	strcpy(out + o, "default="); o += 8; strcpy(out + o, c->defaultServer); o += strlen(c->defaultServer); out[o++] = '\n';
	strcpy(out + o, "custom="); o += 7; strcpy(out + o, c->customServer); o += strlen(c->customServer); out[o++] = '\n';
	out[o] = 0;
	return o;
}

// Splits a normalised "host:port" into host (outHost, outHostSize) and port. False if not in that form.
static inline bool ServerCfg_Split(const char* hostPort, char* outHost, size_t outHostSize, unsigned* outPort)
{
	const char* colon = strrchr(hostPort, ':');
	if (!colon || colon == hostPort) return false;
	const size_t hl = (size_t)(colon - hostPort);
	if (hl + 1 > outHostSize) return false;
	unsigned long port = 0;
	for (const char* d = colon + 1; *d; d++) { if (!ServerCfg_IsDigit(*d)) return false; port = port * 10 + (unsigned long)(*d - '0'); if (port > 65535) return false; }
	if (port == 0) return false;
	memcpy(outHost, hostPort, hl); outHost[hl] = 0; *outPort = (unsigned)port;
	return true;
}
