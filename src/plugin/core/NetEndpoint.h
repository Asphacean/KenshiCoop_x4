// NetEndpoint.h - UDP "host:port" parsing + display (pure, zero game/Win32 deps).
//
// The F2 panel's "Paste server address" button lets a UDP player point their
// client at a host without hand-editing coop_config.json next to the DLL.
// Clipboard text is noisy the same way a pasted Steam ID is (surrounding
// whitespace, a trailing newline, or a "Server: 10.0.0.4:27800" wrapper the host
// copied out of chat), so the endpoint is extracted and validated before use.
// This logic is shared by:
//   * EngineUi.cpp - the "Paste server address" button
//   * prototest    - the no-game unit layer that guards the parse
//
// REJECTION IS THE LOAD-BEARING HALF, not an afterthought. A silent default
// port, or a host written before the port was validated, would leave the player
// pressing Connect against a wrong-but-plausible endpoint shown on their own
// panel. So: no address without an explicit port is ever accepted, and EVERY
// rejection leaves the caller's host and port untouched - a failed paste cannot
// half-overwrite an address that was already armed. Same contract
// parseSteamId64 established.
//
// IPv6 IS OUT OF SCOPE AND IS REJECTED, deliberately rather than implicitly: an
// IPv6 literal is all colons, so a naive split would silently turn "2001:db8::1"
// into host "1" on some port. The guard is explicit (a host token may not be
// directly preceded by a colon) so such input fails visibly instead of
// connecting somewhere unintended. KenshiCoop's transport is IPv4/hostname
// today; if that changes, this header is where the bracketed [::1]:port form
// belongs.

#ifndef COOP_NET_ENDPOINT_H
#define COOP_NET_ENDPOINT_H

#include <string>

namespace coop {

// Characters legal inside a host or hostname for our purposes: the IPv4 dotted
// quad, DNS labels, and the hyphen/underscore that appear in real host names.
// Anything else (space, '/', '[', ':') ends the token, which is what lets a
// copied wrapper prefix like "Server: " or "udp://" fall away.
inline bool netEndpointHostChar(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
}

// Parse "host:port" out of arbitrary text. On success writes host and port and
// returns true; on ANY failure returns false and leaves BOTH outputs untouched.
// Accepts an IPv4 literal or a hostname (ENet resolves names, so refusing them
// would be a parser opinion the transport does not share). Requires an explicit
// port in 1..65535. Pure - safe to unit-test without the game.
inline bool parseHostPort(const std::string& text, std::string& host, int& port) {
    // Trim leading/trailing whitespace and control bytes (\r\n from a paste).
    size_t b = 0, e = text.size();
    while (b < e && (unsigned char)text[b] <= ' ') ++b;
    while (e > b && (unsigned char)text[e - 1] <= ' ') --e;
    if (b >= e) return false;

    // The port is whatever follows the LAST colon, so a wrapper that itself
    // contains a colon ("Server: 10.0.0.4:27800") still resolves correctly.
    size_t colon = std::string::npos;
    for (size_t i = e; i > b; --i) {
        if (text[i - 1] == ':') { colon = i - 1; break; }
    }
    if (colon == std::string::npos) return false; // no port => reject, never default

    // Port: digits only, at least one, at most five, and in range. A leading '-'
    // or any letter is not a digit, so "-1" and "abc" fall out here.
    size_t pb = colon + 1;
    if (pb >= e) return false;               // trailing colon
    if (e - pb > 5) return false;            // cannot be <= 65535
    int p = 0;
    for (size_t i = pb; i < e; ++i) {
        char ch = text[i];
        if (ch < '0' || ch > '9') return false;
        p = p * 10 + (int)(ch - '0');
    }
    if (p < 1 || p > 65535) return false;    // port 0 is not a destination

    // Host: the run of host characters immediately before the colon. Walking
    // backwards is what discards a copied prefix without having to enumerate the
    // prefixes people paste.
    size_t he = colon;
    size_t hb = he;
    while (hb > b && netEndpointHostChar(text[hb - 1])) --hb;
    if (hb == he) return false;              // empty host (":27800", "[::1]:27800")
    if (he - hb > 255) return false;         // DNS name bound
    // An IPv6 literal mis-splits into a plausible host/port pair on its own
    // colons ("2001:db8::1:27800" -> host "1"), so a host token sitting directly
    // after a colon is rejected rather than half-understood.
    if (hb > b && text[hb - 1] == ':') return false;

    // Require at least one alphanumeric so a token of pure punctuation ("...")
    // is not accepted as a destination.
    bool alnum = false;
    for (size_t i = hb; i < he; ++i) {
        char ch = text[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9')) { alnum = true; break; }
    }
    if (!alnum) return false;

    // Only now, with everything validated, are the caller's values written.
    host = text.substr(hb, he - hb);
    port = p;
    return true;
}

// Render a parsed endpoint back as "host:port" for the panel's armed row - the
// value the player reads to confirm where Connect is about to send them. Digits
// are built here instead of with _snprintf to keep this header pure.
inline std::string formatEndpoint(const std::string& host, int port) {
    char digits[8];
    int n = 0;
    int v = port;
    if (v <= 0) digits[n++] = '0';
    while (v > 0 && n < 7) { digits[n++] = (char)('0' + (v % 10)); v /= 10; }
    std::string out(host);
    out += ':';
    for (int i = n - 1; i >= 0; --i) out += digits[i];
    return out;
}

} // namespace coop

#endif // COOP_NET_ENDPOINT_H
