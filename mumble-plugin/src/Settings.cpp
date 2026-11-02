#include "Settings.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace vr {

namespace {

	std::string trim(const std::string &s) {
		const char *ws = " \t\r\n";
		const size_t b = s.find_first_not_of(ws);
		if (b == std::string::npos) {
			return {};
		}
		return s.substr(b, s.find_last_not_of(ws) - b + 1);
	}

	std::optional< float > toFloat(const std::string &v) {
		char *end       = nullptr;
		const float f   = std::strtof(v.c_str(), &end);
		if (end == v.c_str() || !(f == f)) {
			return std::nullopt;
		}
		return f;
	}

	std::optional< bool > toBool(const std::string &v) {
		const std::string s = normalizeName(v);
		if (s == "1" || s == "true" || s == "yes" || s == "on") {
			return true;
		}
		if (s == "0" || s == "false" || s == "no" || s == "off") {
			return false;
		}
		return std::nullopt;
	}

	const char *kTemplate = R"(; Valheim Radio - Mumble plugin settings. Everything here is optional.
;
; Normally the server address comes from the Mumble channel description
; (tag "valheim-radio=host:port/key"), so you don't need to set anything.

[server]
; address = play.example.com:47812
; key =

[player]
; Your Valheim character name, only needed if your Mumble name is different.
; name =

[radio]
; Override the server's radio settings for yourself only.
; enabled = true
; start = 30        ; meters where the radio effect starts
; full = 60         ; meters where it is complete
; static = 0.035
; distortion = 4
; crackle = 1
; squelch = true
; keepVolume = true
; radioVolume = 0.7
; maxBoost = 4
)";

} // namespace

void RadioOverrides::applyTo(RadioConfig &c) const {
	if (enabled) c.enabled = *enabled;
	if (squelch) c.squelch = *squelch;
	if (keepVolume) c.keepVolume = *keepVolume;
	if (start) c.start = *start;
	if (full) c.full = *full;
	if (noise) c.noise = *noise;
	if (drive) c.drive = *drive;
	if (crackle) c.crackle = *crackle;
	if (radioVolume) c.radioVolume = *radioVolume;
	if (maxBoost) c.maxBoost = *maxBoost;
}

std::optional< Endpoint > parseAddress(const std::string &text) {
	std::string s = trim(text);
	if (s.rfind("valheim-radio://", 0) == 0) {
		s = s.substr(16);
	}
	Endpoint ep;
	const size_t slash = s.find('/');
	if (slash != std::string::npos) {
		ep.key = s.substr(slash + 1);
		s      = s.substr(0, slash);
	}
	const size_t colon = s.rfind(':');
	if (colon != std::string::npos && s.find(':') == colon) { // exactly one ':' -> host:port (no IPv6)
		const long port = std::strtol(s.c_str() + colon + 1, nullptr, 10);
		if (port <= 0 || port > 65535) {
			return std::nullopt;
		}
		ep.port = static_cast< uint16_t >(port);
		s       = s.substr(0, colon);
	}
	ep.host = s;
	if (!ep.valid()) {
		return std::nullopt;
	}
	return ep;
}

std::optional< Endpoint > findChannelTag(const std::string &description) {
	static const std::string tag = "valheim-radio=";
	const size_t pos = description.find(tag);
	if (pos == std::string::npos) {
		return std::nullopt;
	}
	size_t end = pos + tag.size();
	while (end < description.size()) {
		const char c = description[end];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '<' || c == '"' || c == '\'') {
			break;
		}
		++end;
	}
	return parseAddress(description.substr(pos + tag.size(), end - pos - tag.size()));
}

std::string settingsPath() {
#ifdef _WIN32
	const char *base = std::getenv("APPDATA");
	return std::string(base ? base : ".") + "\\Mumble\\valheim_radio.ini";
#else
	if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
		return std::string(xdg) + "/Mumble/valheim_radio.ini";
	}
	const char *home = std::getenv("HOME");
	return std::string(home ? home : ".") + "/.config/Mumble/valheim_radio.ini";
#endif
}

LocalSettings parseIni(const std::string &content) {
	LocalSettings s;
	std::istringstream in(content);
	std::string line, section;
	Endpoint ep;
	bool haveAddress = false;
	while (std::getline(in, line)) {
		const size_t comment = line.find_first_of(";#");
		if (comment != std::string::npos) {
			line = line.substr(0, comment);
		}
		line = trim(line);
		if (line.empty()) {
			continue;
		}
		if (line.front() == '[' && line.back() == ']') {
			section = normalizeName(line.substr(1, line.size() - 2));
			continue;
		}
		const size_t eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		const std::string key = normalizeName(line.substr(0, eq));
		const std::string val = trim(line.substr(eq + 1));
		if (val.empty()) {
			continue;
		}
		if (section == "server") {
			if (key == "address") {
				if (auto a = parseAddress(val)) {
					const std::string keepKey = ep.key;
					ep                        = *a;
					if (ep.key.empty()) {
						ep.key = keepKey;
					}
					haveAddress = true;
				}
			} else if (key == "key") {
				ep.key = val;
			}
		} else if (section == "player" && key == "name") {
			s.characterName = val;
		} else if (section == "radio") {
			RadioOverrides &r = s.radio;
			if (key == "enabled") r.enabled = toBool(val);
			else if (key == "squelch") r.squelch = toBool(val);
			else if (key == "keepvolume") r.keepVolume = toBool(val);
			else if (key == "start") r.start = toFloat(val);
			else if (key == "full") r.full = toFloat(val);
			else if (key == "static") r.noise = toFloat(val);
			else if (key == "distortion") r.drive = toFloat(val);
			else if (key == "crackle") r.crackle = toFloat(val);
			else if (key == "radiovolume") r.radioVolume = toFloat(val);
			else if (key == "maxboost") r.maxBoost = toFloat(val);
		}
	}
	if (haveAddress) {
		s.server = ep;
	}
	return s;
}

bool SettingsFile::poll(LocalSettings &out) {
	const std::string path = settingsPath();
	struct stat st;
	if (stat(path.c_str(), &st) != 0) {
		if (!m_templateTried) {
			m_templateTried = true;
			std::ofstream(path) << kTemplate; // fails silently if the Mumble folder doesn't exist
		}
		if (m_mtime != -1) {
			m_mtime = -1;
			out     = LocalSettings{};
			return true;
		}
		return false;
	}
	const int64_t mtime = static_cast< int64_t >(st.st_mtime);
	if (mtime == m_mtime) {
		return false;
	}
	m_mtime = mtime;
	std::ifstream f(path, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	out = parseIni(ss.str());
	return true;
}

} // namespace vr
