// Client-side settings.
//
// Players normally configure nothing: the server address comes from a tag in the Mumble channel
// description, e.g.   valheim-radio=play.example.com:47812/secretkey
// The optional ini file (see settingsPath()) can override the address, the character name and any
// of the radio parameters the server hands out.
#pragma once

#include "Protocol.h"

#include <cstdint>
#include <optional>
#include <string>

namespace vr {

struct Endpoint {
	std::string host;
	uint16_t port = kDefaultPort;
	std::string key;

	bool valid() const { return !host.empty() && port != 0; }
	bool operator==(const Endpoint &o) const { return host == o.host && port == o.port && key == o.key; }
	bool operator!=(const Endpoint &o) const { return !(*this == o); }
};

struct RadioOverrides {
	std::optional< bool > enabled, squelch, keepVolume;
	std::optional< float > start, full, noise, drive, crackle, radioVolume, maxBoost;

	void applyTo(RadioConfig &c) const;
};

struct LocalSettings {
	std::optional< Endpoint > server; // [server] address/key
	std::string characterName;        // [player] name
	RadioOverrides radio;             // [radio] ...
};

// "host", "host:port", "host:port/key", "host/key"; returns nullopt if unusable.
std::optional< Endpoint > parseAddress(const std::string &text);

// Finds "valheim-radio=<address>" inside a (possibly HTML) channel description.
std::optional< Endpoint > findChannelTag(const std::string &description);

std::string settingsPath();
LocalSettings parseIni(const std::string &content);

// Reloads the ini when its modification time changes; creates a commented template if missing.
class SettingsFile {
public:
	// Returns true if the settings changed.
	bool poll(LocalSettings &out);

private:
	int64_t m_mtime = -1;
	bool m_templateTried = false;
};

} // namespace vr
