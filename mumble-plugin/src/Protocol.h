// Wire formats (all little endian).
//
// Valheim server mod <-> Mumble plugin, over TCP. Every message is framed as  u32 length | payload.
//
//   client -> server  HELLO     u32 'VHC1' | u16 version | str key
//   server -> client  SNAPSHOT  u32 'VHS1' | u16 version | u16 flags
//                               str context
//                               f32 radioStart, radioFull, noise, drive, crackle, radioVolume, maxBoost
//                               u8 enabled, squelch, keepVolume, reserved
//                               u16 count, then per player: str name | f32[3] position (eye) | f32[3] facing
//
//   str = u16 byte length + UTF-8
//
// Plugin <-> plugin (Mumble plugin data, id "valheim-radio:name"), only needed when someone's Mumble
// name differs from their character name:   u32 'VHN1' | u16 version | str characterName
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vr {

constexpr uint32_t kHelloMagic    = 0x31434856u; // "VHC1"
constexpr uint32_t kSnapshotMagic = 0x31534856u; // "VHS1"
constexpr uint32_t kNameMagic     = 0x314E4856u; // "VHN1"
constexpr uint16_t kVersion       = 1;
constexpr uint16_t kDefaultPort   = 47812;
constexpr uint32_t kMaxFrame      = 64 * 1024;
constexpr const char *kNameDataID = "valheim-radio:name";

struct RadioConfig {
	bool enabled      = true;
	float start       = 30.f;
	float full        = 60.f;
	float noise       = 0.035f;
	float drive       = 4.f;
	float crackle     = 1.f;
	bool squelch      = true;
	bool keepVolume   = true;
	float radioVolume = 0.7f;
	float maxBoost    = 4.f;
};

struct PlayerPose {
	float pos[3];
	float dir[3];
};

struct PlayerEntry {
	std::string name;
	PlayerPose pose;
};

struct Snapshot {
	std::string context;
	RadioConfig radio;
	std::vector< PlayerEntry > players;
};

// ASCII-lowercase + trim, used to match Mumble names against character names.
inline std::string normalizeName(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t')) {
		++b;
	}
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
		--e;
	}
	std::string out = s.substr(b, e - b);
	for (char &c : out) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast< char >(c - 'A' + 'a');
		}
	}
	return out;
}

class Reader {
public:
	Reader(const uint8_t *d, size_t n) : m_d(d), m_n(n) {}
	template< typename T > bool get(T &out) {
		if (m_pos + sizeof(T) > m_n) {
			return false;
		}
		std::memcpy(&out, m_d + m_pos, sizeof(T));
		m_pos += sizeof(T);
		return true;
	}
	bool str(std::string &out) {
		uint16_t len;
		if (!get(len) || m_pos + len > m_n) {
			return false;
		}
		out.assign(reinterpret_cast< const char * >(m_d + m_pos), len);
		m_pos += len;
		return true;
	}
	bool vec3(float *v) {
		if (!get(v[0]) || !get(v[1]) || !get(v[2])) {
			return false;
		}
		for (int i = 0; i < 3; ++i) {
			if (!(v[i] == v[i]) || v[i] > 1e7f || v[i] < -1e7f) {
				return false;
			}
		}
		return true;
	}

private:
	const uint8_t *m_d;
	size_t m_n;
	size_t m_pos = 0;
};

class Writer {
public:
	std::vector< uint8_t > buf;
	template< typename T > void put(const T &v) {
		const auto *p = reinterpret_cast< const uint8_t * >(&v);
		buf.insert(buf.end(), p, p + sizeof(T));
	}
	void str(const std::string &s) {
		const uint16_t len = static_cast< uint16_t >(std::min< size_t >(s.size(), 0xFFFF));
		put(len);
		buf.insert(buf.end(), s.begin(), s.begin() + len);
	}
	void vec3(const float *v) {
		put(v[0]);
		put(v[1]);
		put(v[2]);
	}
	// Prepends the u32 frame length.
	std::vector< uint8_t > framed() const {
		Writer f;
		f.put(static_cast< uint32_t >(buf.size()));
		f.buf.insert(f.buf.end(), buf.begin(), buf.end());
		return f.buf;
	}
};

inline std::vector< uint8_t > makeHello(const std::string &key) {
	Writer w;
	w.put(kHelloMagic);
	w.put(kVersion);
	w.str(key);
	return w.framed();
}

inline bool parseSnapshot(const uint8_t *data, size_t len, Snapshot &out) {
	Reader r(data, len);
	uint32_t magic;
	uint16_t version, flags, count;
	if (!r.get(magic) || magic != kSnapshotMagic || !r.get(version) || version != kVersion || !r.get(flags)
		|| !r.str(out.context)) {
		return false;
	}
	RadioConfig &c = out.radio;
	uint8_t enabled, squelch, keepVolume, reserved;
	if (!r.get(c.start) || !r.get(c.full) || !r.get(c.noise) || !r.get(c.drive) || !r.get(c.crackle)
		|| !r.get(c.radioVolume) || !r.get(c.maxBoost) || !r.get(enabled) || !r.get(squelch) || !r.get(keepVolume)
		|| !r.get(reserved) || !r.get(count)) {
		return false;
	}
	c.enabled    = enabled != 0;
	c.squelch    = squelch != 0;
	c.keepVolume = keepVolume != 0;
	out.players.clear();
	out.players.reserve(count);
	for (uint16_t i = 0; i < count; ++i) {
		PlayerEntry e;
		if (!r.str(e.name) || !r.vec3(e.pose.pos) || !r.vec3(e.pose.dir)) {
			return false;
		}
		out.players.push_back(std::move(e));
	}
	return true;
}

inline std::vector< uint8_t > makeNameAnnouncement(const std::string &name) {
	Writer w;
	w.put(kNameMagic);
	w.put(kVersion);
	w.str(name);
	return w.buf;
}

inline bool parseNameAnnouncement(const uint8_t *data, size_t len, std::string &name) {
	Reader r(data, len);
	uint32_t magic;
	uint16_t version;
	return r.get(magic) && magic == kNameMagic && r.get(version) && version == kVersion && r.str(name)
		   && name.size() <= 64;
}

} // namespace vr
