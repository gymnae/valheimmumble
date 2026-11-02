// C++ side of the cross-language wire test (uses the plugin's real Protocol.h).
#include "Protocol.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char **argv) {
	const std::string mode = argv[1];
	if (mode == "hello") {
		auto h = vr::makeHello(argv[3]);
		std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast< const char * >(h.data()), h.size());
		return 0;
	}
	std::ifstream f(argv[2], std::ios::binary);
	std::vector< uint8_t > d((std::istreambuf_iterator< char >(f)), {});
	uint32_t len;
	std::memcpy(&len, d.data(), 4);
	if (len + 4 != d.size()) {
		std::printf("FAIL frame length %u vs file %zu\n", len, d.size());
		return 1;
	}
	vr::Snapshot s;
	if (!vr::parseSnapshot(d.data() + 4, len, s)) {
		std::printf("FAIL parse\n");
		return 1;
	}
	const auto &r = s.radio;
	std::printf("context=%s radio: en=%d sq=%d kv=%d start=%g full=%g noise=%g drive=%g crackle=%g vol=%g boost=%g\n",
				s.context.c_str(), r.enabled, r.squelch, r.keepVolume, r.start, r.full, r.noise, r.drive, r.crackle,
				r.radioVolume, r.maxBoost);
	for (auto &p : s.players) {
		std::printf("  %s (norm '%s') pos=(%g,%g,%g) dir=(%g,%g,%g)\n", p.name.c_str(), vr::normalizeName(p.name).c_str(),
					p.pose.pos[0], p.pose.pos[1], p.pose.pos[2], p.pose.dir[0], p.pose.dir[1], p.pose.dir[2]);
	}
	const bool ok = s.context == "valheim-abc123" && s.players.size() == 2 && s.players[1].name == "Ægir Þórsson"
					&& s.players[1].pose.pos[2] == 900.25f && r.start == 25 && r.full == 70 && !r.squelch
					&& r.maxBoost == 6;
	std::printf(ok ? "OK\n" : "FAIL values\n");
	return ok ? 0 : 1;
}
