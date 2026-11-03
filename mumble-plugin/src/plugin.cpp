// Valheim positional audio + distance radio effect for Mumble (plugin API 1.2, Mumble >= 1.5).
//
// Players run vanilla Valheim. A BepInEx mod on the Valheim *server* streams every player's position
// over TCP; this plugin connects to it, feeds the local player's position to Mumble, and uses the
// other players' positions for the radio effect. Players are matched by name (Mumble name ==
// character name, or an override in the ini that is announced to the other plugins).
//
// Threads:
//   * net thread    – TCP connection to the Valheim server mod, parses snapshots.
//   * worker thread – Mumble API calls (user names, channel description, settings, name announcements).
//                     API calls from here block, but time out after 800 ms inside Mumble -> no deadlock.
//   * Mumble audio threads – fetchPositionalData (input+output), onAudioSourceFetched (output).
//   * Mumble main thread   – onReceiveData and connection callbacks.

#include "MumblePlugin.h"

#include "Protocol.h"
#include "RadioFx.h"
#include "Settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <winsock2.h>
#	include <ws2tcpip.h>
using socket_t                       = SOCKET;
static constexpr socket_t kBadSocket = INVALID_SOCKET;
static constexpr int kSendFlags     = 0;
static void closeSocket(socket_t s) { closesocket(s); }
static bool lastErrorIsTimeout() {
	const int e = WSAGetLastError();
	return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
}
static void setNonBlocking(socket_t s, bool on) {
	u_long v = on ? 1 : 0;
	ioctlsocket(s, FIONBIO, &v);
}
#else
#	include <cerrno>
#	include <fcntl.h>
#	include <netdb.h>
#	include <netinet/in.h>
#	include <netinet/tcp.h>
#	include <sys/select.h>
#	include <sys/socket.h>
#	include <sys/time.h>
#	include <unistd.h>
using socket_t                       = int;
static constexpr socket_t kBadSocket = -1;
// A send() on a socket the server closed must not raise SIGPIPE: that would kill Mumble itself.
#	ifdef MSG_NOSIGNAL
static constexpr int kSendFlags = MSG_NOSIGNAL;
#	else
static constexpr int kSendFlags = 0; // macOS: SO_NOSIGPIPE is set on the socket instead
#	endif
static void closeSocket(socket_t s) { close(s); }
static bool lastErrorIsTimeout() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
static void setNonBlocking(socket_t s, bool on) {
	const int flags = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
#endif

using Clock = std::chrono::steady_clock;

namespace {

constexpr auto kSnapshotTimeout  = std::chrono::seconds(3);
constexpr auto kReconnectDelay   = std::chrono::seconds(3);
constexpr auto kMumbleInterval   = std::chrono::seconds(1);
constexpr auto kSettingsInterval = std::chrono::seconds(2);
constexpr auto kAnnounceInterval = std::chrono::seconds(5);
constexpr int kConnectTimeoutMs  = 3000;

MumbleAPI g_api;
mumble_plugin_id_t g_id = 0;
bool g_apiValid         = false;

// ---- latest snapshot from the Valheim server (written by net thread) ----
struct SnapshotState {
	std::string context;
	vr::RadioConfig radio;
	std::unordered_map< std::string, vr::PlayerPose > players; // key: normalized character name
	Clock::time_point time;
	bool valid = false;
};
std::mutex g_snapMtx;
SnapshotState g_snap;

// ---- identities and settings (written by worker / main thread) ----
struct Identity {
	std::string myName; // normalized character name we look ourselves up by
	std::string myDisplayName;
	std::unordered_map< mumble_userid_t, std::string > mumbleNames; // normalized Mumble names
	std::unordered_map< mumble_userid_t, std::string > announced;   // normalized announced character names
	vr::RadioOverrides overrides;
	vr::Endpoint endpoint; // effective server endpoint
};
std::mutex g_idMtx;
Identity g_ident;

// ---- Mumble positional-audio settings (written by worker) ----
struct PaSettings {
	bool valid        = false;
	float minDistance = 1.f;
	float maxDistance = 15.f;
	float minVolume   = 1.f;
};
std::mutex g_paMtx;
PaSettings g_pa;

// ---- audio-thread-only state ----
std::unordered_map< mumble_userid_t, vr::RadioVoice > g_voices;
std::atomic< bool > g_resetVoices{ false };

// ---- strings handed to Mumble from fetchPositionalData (serialized by Mumble's lock) ----
std::string g_ctxOut, g_idOut;

std::atomic< bool > g_stop{ false };
std::thread g_netThread, g_workerThread;

double nowSeconds() {
	return std::chrono::duration< double >(Clock::now().time_since_epoch()).count();
}

void logMsg(const std::string &msg) {
	if (g_apiValid) {
		g_api.log(g_id, ("Valheim Radio: " + msg).c_str());
	}
}

// Messages produced on threads that must not call the API are queued and logged by the worker.
std::mutex g_logMtx;
std::vector< std::string > g_logQueue;
void queueLog(std::string msg) {
	std::lock_guard< std::mutex > lock(g_logMtx);
	g_logQueue.push_back(std::move(msg));
}

bool snapFresh(Clock::time_point now) {
	return g_snap.valid && now - g_snap.time < kSnapshotTimeout;
}

// Mirrors AudioOutput::calcGain()'s distance term so we can undo it for radio voices.
float mumbleAttenuation(float d, const PaSettings &s) {
	if (d < 0.01f || s.minVolume > 0.99f || d < s.minDistance) {
		return 1.f;
	}
	if (d >= s.maxDistance) {
		return s.minVolume;
	}
	const float mvol = std::max(s.minVolume, 0.005f);
	const float drel = (d - s.minDistance) / (s.maxDistance - s.minDistance);
	return std::pow(10.f, std::log10(mvol) * drel);
}

// The volume we want a voice to have at distance d (used with KeepVolume): full volume up to Mumble's
// Minimum Distance, then a gentle fade (linear in dB) down to RadioVolume at the radio start distance,
// and RadioVolume from there on, however far away. Mumble's own fade is divided out (see comp below),
// so this curve applies whatever the listener's Maximum Distance / Minimum Volume are.
float desiredGain(float d, const vr::RadioConfig &cfg, const PaSettings &s) {
	const float nearDist = std::max(s.minDistance, 0.f);
	const float vol  = std::max(cfg.radioVolume, 0.005f);
	if (d <= nearDist) {
		return 1.f;
	}
	if (d >= cfg.start || cfg.start <= nearDist) {
		return vol;
	}
	return std::pow(10.f, std::log10(vol) * (d - nearDist) / (cfg.start - nearDist));
}

// ================================================================================================
// Network thread
// ================================================================================================

std::string addrToString(const addrinfo *ai) {
	char host[64] = "?";
	getnameinfo(ai->ai_addr, static_cast< socklen_t >(ai->ai_addrlen), host, sizeof(host), nullptr, 0, NI_NUMERICHOST);
	return ai->ai_family == AF_INET6 ? std::string("[") + host + "]" : std::string(host);
}

// Tries every address the host resolves to (IPv6 and IPv4), starting with the one at index
// `rotate` (modulo the count). `used` gets the address that worked; `detail` lists each
// address's outcome for the log when none did.
socket_t connectTcp(const vr::Endpoint &ep, size_t rotate, std::string &used, std::string &detail) {
	used.clear();
	detail.clear();
	addrinfo hints{};
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo *res     = nullptr;
	const std::string port = std::to_string(ep.port);
	if (getaddrinfo(ep.host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
		detail = "cannot resolve host name";
		return kBadSocket;
	}
	std::vector< addrinfo * > addrs;
	for (addrinfo *ai = res; ai; ai = ai->ai_next) {
		addrs.push_back(ai);
	}
	socket_t result = kBadSocket;
	for (size_t i = 0; i < addrs.size() && result == kBadSocket && !g_stop.load(); ++i) {
		addrinfo *ai = addrs[(rotate + i) % addrs.size()];
		const std::string name = addrToString(ai);
		socket_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == kBadSocket) {
			detail += (detail.empty() ? "" : ", ") + name + " unsupported";
			continue;
		}
		setNonBlocking(s, true);
		connect(s, ai->ai_addr, static_cast< int >(ai->ai_addrlen));
		fd_set wfds;
		FD_ZERO(&wfds);
		FD_SET(s, &wfds);
		timeval tv{ kConnectTimeoutMs / 1000, (kConnectTimeoutMs % 1000) * 1000 };
		int err         = -1;
		socklen_t errLen = sizeof(err);
		const int ready = select(static_cast< int >(s) + 1, nullptr, &wfds, nullptr, &tv);
		if (ready == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast< char * >(&err), &errLen) == 0 && err == 0) {
			setNonBlocking(s, false);
			result = s;
			used   = name;
		} else {
			closeSocket(s);
			detail += (detail.empty() ? "" : ", ") + name + (ready == 1 ? " refused" : " timed out");
		}
	}
	freeaddrinfo(res);
	if (result != kBadSocket) {
		int one = 1;
		setsockopt(result, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast< const char * >(&one), sizeof(one));
#ifdef _WIN32
		DWORD tv = 250;
		setsockopt(result, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast< const char * >(&tv), sizeof(tv));
#else
		timeval tv{ 0, 250000 };
		setsockopt(result, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#	ifdef SO_NOSIGPIPE
		setsockopt(result, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#	endif
#endif
	}
	return result;
}

bool sendAll(socket_t s, const std::vector< uint8_t > &data) {
	size_t off = 0;
	while (off < data.size()) {
		const int n = static_cast< int >(
			send(s, reinterpret_cast< const char * >(data.data() + off), static_cast< int >(data.size() - off), kSendFlags));
		if (n <= 0) {
			return false;
		}
		off += static_cast< size_t >(n);
	}
	return true;
}

void storeSnapshot(vr::Snapshot &&snap) {
	std::unordered_map< std::string, vr::PlayerPose > players;
	for (auto &p : snap.players) {
		players[vr::normalizeName(p.name)] = p.pose;
	}
	std::lock_guard< std::mutex > lock(g_snapMtx);
	g_snap.context = std::move(snap.context);
	g_snap.radio   = snap.radio;
	g_snap.players = std::move(players);
	g_snap.time    = Clock::now();
	g_snap.valid   = true;
}

void netLoop() {
	socket_t s = kBadSocket;
	vr::Endpoint current;
	std::vector< uint8_t > rx;
	auto nextAttempt = Clock::now();
	std::string lastError;
	std::string usedAddr;
	size_t rotate = 0; // which resolved address to try first
	bool gotData = false;
	uint8_t buf[8192];

	auto disconnect = [&](const std::string &why) {
		if (s != kBadSocket) {
			closeSocket(s);
			s = kBadSocket;
			queueLog("disconnected from " + current.host + ":" + std::to_string(current.port) + " via " + usedAddr + " ("
					 + why + ")");
		}
		std::lock_guard< std::mutex > lock(g_snapMtx);
		g_snap.valid = false;
	};

	while (!g_stop.load()) {
		vr::Endpoint want;
		{
			std::lock_guard< std::mutex > lock(g_idMtx);
			want = g_ident.endpoint;
		}
		if (s != kBadSocket && want != current) {
			disconnect("server address changed");
		}
		if (s == kBadSocket) {
			if (!want.valid() || Clock::now() < nextAttempt) {
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}
			current     = want;
			nextAttempt = Clock::now() + kReconnectDelay;
			std::string detail;
			s = connectTcp(current, rotate, usedAddr, detail);
			if (s != kBadSocket && !sendAll(s, vr::makeHello(current.key))) {
				closeSocket(s);
				s      = kBadSocket;
				detail = usedAddr + " closed the connection";
				++rotate;
			}
			if (s == kBadSocket) {
				const std::string err = "cannot reach Valheim server at " + current.host + ":" + std::to_string(current.port)
										+ " (" + detail + ")";
				if (err != lastError) {
					queueLog(err + " (retrying)");
					lastError = err;
				}
				continue;
			}
			lastError.clear();
			rx.clear();
			gotData = false;
			queueLog("connected to Valheim server " + current.host + ":" + std::to_string(current.port) + " via " + usedAddr);
		}

		const int n = static_cast< int >(recv(s, reinterpret_cast< char * >(buf), sizeof(buf), 0));
		if (n == 0) {
			if (!gotData) {
				// Closed before the first snapshot: a wrong key, or something other than the mod
				// accepted the connection (e.g. a Docker/router proxy on IPv6 with nothing behind it).
				// Start with the host's next address next time.
				++rotate;
				disconnect("closed before any data - wrong key, or nothing behind this address/port?");
			} else {
				disconnect("server closed the connection");
			}
			continue;
		}
		if (n < 0) {
			if (!lastErrorIsTimeout()) {
				disconnect("network error");
			}
			continue;
		}
		gotData = true;
		rx.insert(rx.end(), buf, buf + n);
		size_t off = 0;
		while (rx.size() - off >= 4) {
			uint32_t len;
			std::memcpy(&len, rx.data() + off, 4);
			if (len > vr::kMaxFrame) {
				off = rx.size();
				disconnect("protocol error");
				break;
			}
			if (rx.size() - off - 4 < len) {
				break;
			}
			vr::Snapshot snap;
			if (vr::parseSnapshot(rx.data() + off + 4, len, snap)) {
				storeSnapshot(std::move(snap));
			}
			off += 4 + len;
		}
		rx.erase(rx.begin(), rx.begin() + static_cast< std::ptrdiff_t >(std::min(off, rx.size())));
	}
	if (s != kBadSocket) {
		closeSocket(s);
	}
}

// ================================================================================================
// Worker thread (all Mumble API calls)
// ================================================================================================

std::string takeString(const char *s) {
	std::string out = s ? s : "";
	if (s) {
		g_api.freeMemory(g_id, s);
	}
	return out;
}

std::string apiUserName(mumble_connection_t conn, mumble_userid_t uid) {
	const char *name = nullptr;
	if (g_api.getUserName(g_id, conn, uid, &name) != MUMBLE_STATUS_OK) {
		return {};
	}
	return takeString(name);
}

// Looks for the tag in a channel's description. `status` describes what was found, for the log.
std::optional< vr::Endpoint > apiChannelTag(mumble_connection_t conn, mumble_channelid_t channel, std::string &status) {
	std::string name;
	const char *n = nullptr;
	if (g_api.getChannelName(g_id, conn, channel, &n) == MUMBLE_STATUS_OK) {
		name = takeString(n);
	}
	const std::string label = channel == 0 ? "root channel \"" + name + "\"" : "channel \"" + name + "\"";
	const char *desc = nullptr;
	if (g_api.getChannelDescription(g_id, conn, channel, &desc) != MUMBLE_STATUS_OK) {
		status = label + ": description not available";
		return std::nullopt;
	}
	const std::string text = takeString(desc);
	auto tag               = vr::findChannelTag(text);
	if (text.empty()) {
		status = label + ": description empty or not downloaded yet";
	} else if (!tag) {
		status = label + ": description has no valid valheim-radio= tag";
	}
	return tag;
}

void refreshPaSettings() {
	auto get = [](mumble_settings_key_t key, float &out) {
		double v = 0;
		if (g_api.getMumbleSetting_double(g_id, key, &v) != MUMBLE_STATUS_OK) {
			return false;
		}
		out = static_cast< float >(v);
		return true;
	};
	PaSettings s;
	s.valid = get(MUMBLE_SK_AUDIO_OUTPUT_PA_MINIMUM_DISTANCE, s.minDistance)
			  && get(MUMBLE_SK_AUDIO_OUTPUT_PA_MAXIMUM_DISTANCE, s.maxDistance)
			  && get(MUMBLE_SK_AUDIO_OUTPUT_PA_MINIMUM_VOLUME, s.minVolume) && s.maxDistance > s.minDistance;
	if (s.valid) {
		std::lock_guard< std::mutex > lock(g_paMtx);
		g_pa = s;
	}
}

// Logs Mumble's positional-audio settings whenever they (or the radio config) change, with a warning
// if they keep far voices from being heard as radio.
void checkPaSettings(const vr::RadioOverrides &overrides) {
	static std::string last;
	PaSettings pa;
	{
		std::lock_guard< std::mutex > lock(g_paMtx);
		pa = g_pa;
	}
	vr::RadioConfig cfg;
	{
		std::lock_guard< std::mutex > lock(g_snapMtx);
		if (!snapFresh(Clock::now())) {
			return;
		}
		cfg = g_snap.radio;
	}
	overrides.applyTo(cfg);
	if (!pa.valid || !cfg.enabled) {
		return;
	}
	char buf[512];
	std::snprintf(buf, sizeof(buf),
				  "Mumble positional audio: minimum distance %.1f m, maximum distance %.1f m, minimum volume %.0f%%; "
				  "radio from %.0f m (full at %.0f m) at %.0f%% volume%s",
				  pa.minDistance, pa.maxDistance, pa.minVolume * 100.f, cfg.start, cfg.full, cfg.radioVolume * 100.f,
				  cfg.keepVolume ? ", volume curve set by the plugin" : ", Mumble's volume curve (KeepVolume off)");
	std::string msg = buf;
	if (cfg.keepVolume && pa.minVolume < 0.02f) {
		msg += ". WARNING: Minimum Volume is 0 - Mumble mutes voices beyond the maximum distance, so they can't "
			   "become radio. Set Minimum Volume to 30% or more.";
	} else if (cfg.keepVolume && cfg.radioVolume / std::max(pa.minVolume, 1e-3f) > std::max(cfg.maxBoost, 1.f)) {
		std::snprintf(buf, sizeof(buf),
					  ". WARNING: Minimum Volume is too low for the radio volume (needs %.1fx boost, MaxBoost is %.1f) - "
					  "far voices will be quieter. Raise Minimum Volume.",
					  cfg.radioVolume / std::max(pa.minVolume, 1e-3f), cfg.maxBoost);
		msg += buf;
	}
	if (msg != last) {
		last = msg;
		logMsg(msg);
	}
}

struct MumbleView {
	bool connected = false;
	mumble_connection_t conn{};
	mumble_userid_t self{};
	std::string selfName;
	std::vector< mumble_userid_t > others;
	std::unordered_map< mumble_userid_t, std::string > names;
	std::optional< vr::Endpoint > tag;
	std::string tagStatus; // why no tag was found
};

MumbleView readMumble() {
	MumbleView v;
	bool synced = false;
	if (g_api.getActiveServerConnection(g_id, &v.conn) != MUMBLE_STATUS_OK
		|| g_api.isConnectionSynchronized(g_id, v.conn, &synced) != MUMBLE_STATUS_OK || !synced
		|| g_api.getLocalUserID(g_id, v.conn, &v.self) != MUMBLE_STATUS_OK) {
		return v;
	}
	v.connected = true;
	v.selfName  = apiUserName(v.conn, v.self);

	mumble_channelid_t channel;
	if (g_api.getChannelOfUser(g_id, v.conn, v.self, &channel) != MUMBLE_STATUS_OK) {
		return v;
	}
	mumble_userid_t *users = nullptr;
	size_t count           = 0;
	if (g_api.getUsersInChannel(g_id, v.conn, channel, &users, &count) == MUMBLE_STATUS_OK) {
		for (size_t i = 0; i < count; ++i) {
			if (users[i] != v.self) {
				v.others.push_back(users[i]);
			}
		}
		g_api.freeMemory(g_id, users);
	}
	for (mumble_userid_t uid : v.others) {
		v.names[uid] = vr::normalizeName(apiUserName(v.conn, uid));
	}
	std::string status;
	v.tag = apiChannelTag(v.conn, channel, status);
	v.tagStatus = status;
	if (!v.tag && channel != 0) {
		v.tag = apiChannelTag(v.conn, 0, status); // root channel
		v.tagStatus += "; " + status;
	}
	return v;
}

void workerLoop() {
	vr::SettingsFile file;
	vr::LocalSettings local;
	auto nextMumble = Clock::now(), nextSettings = Clock::now(), nextAnnounce = Clock::now();
	bool wasLinked = false;
	std::string lastNoServer;
	MumbleView view;

	while (!g_stop.load()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		const auto now = Clock::now();

		std::vector< std::string > logs;
		{
			std::lock_guard< std::mutex > lock(g_logMtx);
			logs.swap(g_logQueue);
		}
		for (const auto &m : logs) {
			logMsg(m);
		}

		bool settingsChanged = false;
		if (now >= nextSettings) {
			nextSettings = now + kSettingsInterval;
			refreshPaSettings();
			settingsChanged = file.poll(local);
			checkPaSettings(local.radio);
		}
		if (now >= nextMumble || settingsChanged) {
			nextMumble = now + kMumbleInterval;
			view       = readMumble();

			const std::string display = !local.characterName.empty() ? local.characterName : view.selfName;
			vr::Endpoint ep;
			if (local.server) {
				ep = *local.server;
			} else if (view.connected && view.tag) {
				ep = *view.tag;
			}
			// Say why we're not connecting, instead of staying silent.
			std::string noServer;
			if (view.connected && !ep.valid()) {
				noServer = "no Valheim server address found (" + view.tagStatus
						   + "). Mumble only sends channel descriptions longer than about 128 bytes (HTML "
							 "included) once you open them: keep the description short, open it once, or set "
							 "the address in valheim_radio.ini";
			}
			if (noServer != lastNoServer) {
				if (!noServer.empty()) {
					queueLog(noServer);
				}
				lastNoServer = noServer;
			}
			std::lock_guard< std::mutex > lock(g_idMtx);
			if (ep != g_ident.endpoint && ep.valid()) {
				queueLog(std::string("using Valheim server ") + ep.host + ":" + std::to_string(ep.port)
						 + (local.server ? " (from valheim_radio.ini)" : " (from channel description)"));
			}
			g_ident.endpoint      = ep;
			g_ident.myDisplayName = display;
			g_ident.myName        = vr::normalizeName(display);
			g_ident.mumbleNames   = view.names;
			g_ident.overrides     = local.radio;
			if (!view.connected) {
				g_ident.announced.clear();
			}
		}

		// Tell the others our character name if it differs from our Mumble name.
		if (view.connected && !local.characterName.empty() && !view.others.empty() && now >= nextAnnounce) {
			nextAnnounce   = now + kAnnounceInterval;
			const auto msg = vr::makeNameAnnouncement(local.characterName);
			g_api.sendData(g_id, view.conn, view.others.data(), view.others.size(), msg.data(), msg.size(),
						   vr::kNameDataID);
		}

		bool linked = false;
		std::string me;
		{
			std::lock_guard< std::mutex > lock(g_idMtx);
			me = g_ident.myName;
		}
		{
			std::lock_guard< std::mutex > lock(g_snapMtx);
			linked = snapFresh(now) && g_snap.players.count(me) != 0;
		}
		if (linked != wasLinked) {
			logMsg(linked ? "found you (\"" + me + "\") in the game - positional audio active"
						  : "you are not in the game (or your character name differs from your Mumble name - "
							"set it in valheim_radio.ini)");
			wasLinked = linked;
		}
	}
}

// Looks up the local player's pose. Caller holds g_snapMtx.
bool findSelf(Clock::time_point now, vr::PlayerPose &out) {
	if (!snapFresh(now)) {
		return false;
	}
	std::string me;
	{
		std::lock_guard< std::mutex > lock(g_idMtx);
		me = g_ident.myName;
	}
	auto it = g_snap.players.find(me);
	if (me.empty() || it == g_snap.players.end()) {
		return false;
	}
	out = it->second;
	return true;
}

// Audio thread: logs (via the worker) when a speaker's state changes, so a missing radio effect can be
// traced to name matching or distance.
void reportSpeaker(mumble_userid_t uid, const std::string &name, bool matched, float distance,
				   const vr::RadioConfig &cfg) {
	static std::unordered_map< mumble_userid_t, int > states; // 1 unmatched, 2 direct, 3 radio
	int &st = states[uid];
	int next;
	if (!matched) {
		next = 1;
	} else if (distance > cfg.start) {
		next = 3;
	} else if (st == 3 && distance > cfg.start - 3.f) {
		next = 3; // hysteresis
	} else {
		next = 2;
	}
	if (next == st) {
		return;
	}
	st = next;
	char buf[96];
	std::snprintf(buf, sizeof(buf), "%.0f m away", distance);
	if (next == 1) {
		queueLog("hearing Mumble user \"" + name + "\" but no game character has that name - no radio effect for "
				 "them (their character name must equal their Mumble name, or they set it in their valheim_radio.ini)");
	} else if (next == 2) {
		queueLog("hearing \"" + name + "\", " + buf + " - direct voice");
	} else {
		queueLog("hearing \"" + name + "\", " + buf + " - radio" + (cfg.enabled ? "" : " (but radio is disabled)"));
	}
}

} // namespace

// ================================================================================================
// Mumble plugin interface
// ================================================================================================

static MumbleStringWrapper wrap(const char *s) {
	MumbleStringWrapper w;
	w.data           = s;
	w.size           = std::strlen(s);
	w.needsReleasing = false;
	return w;
}

mumble_error_t mumble_init(mumble_plugin_id_t id) {
	g_id = id;
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	g_stop         = false;
	g_netThread    = std::thread(netLoop);
	g_workerThread = std::thread(workerLoop);
	logMsg("settings file: " + vr::settingsPath());
	return MUMBLE_STATUS_OK;
}

void mumble_shutdown() {
	g_stop = true;
	if (g_netThread.joinable()) {
		g_netThread.join();
	}
	if (g_workerThread.joinable()) {
		g_workerThread.join();
	}
#ifdef _WIN32
	WSACleanup();
#endif
	std::lock_guard< std::mutex > lock(g_snapMtx);
	g_snap.valid = false;
}

MumbleStringWrapper mumble_getName() { return wrap("Valheim"); }
MumbleStringWrapper mumble_getAuthor() { return wrap("valheimmumble"); }
MumbleStringWrapper mumble_getDescription() {
	return wrap("Positional audio for Valheim with a vanilla game client (needs the server-side BepInEx mod). "
				"Players beyond a configurable distance sound like they are talking over a radio.");
}
mumble_version_t mumble_getAPIVersion() { return MUMBLE_PLUGIN_API_VERSION; }
mumble_version_t mumble_getVersion() { return { 2, 0, 0 }; }

void mumble_registerAPIFunctions(void *apiStruct) {
	g_api      = MUMBLE_API_CAST(apiStruct);
	g_apiValid = true;
}

void mumble_releaseResource(const void *) {}

uint32_t mumble_getFeatures() { return MUMBLE_FEATURE_POSITIONAL | MUMBLE_FEATURE_AUDIO; }

uint32_t mumble_deactivateFeatures(uint32_t features) { return features; }

// ---- positional data --------------------------------------------------------------------------

uint8_t mumble_initPositionalData(const char *const *, const uint64_t *, size_t) {
	// The game runs vanilla, so we don't look for its process: we're linked whenever the server
	// reports our character.
	std::lock_guard< std::mutex > lock(g_snapMtx);
	vr::PlayerPose me;
	return findSelf(Clock::now(), me) ? MUMBLE_PDEC_OK : MUMBLE_PDEC_ERROR_TEMP;
}

bool mumble_fetchPositionalData(float *avatarPos, float *avatarDir, float *avatarAxis, float *cameraPos,
								float *cameraDir, float *cameraAxis, const char **context, const char **identity) {
	std::lock_guard< std::mutex > lock(g_snapMtx);
	vr::PlayerPose me;
	if (!findSelf(Clock::now(), me)) {
		for (float *v : { avatarPos, avatarDir, avatarAxis, cameraPos, cameraDir, cameraAxis }) {
			v[0] = v[1] = v[2] = 0.f;
		}
		g_ctxOut.clear();
		g_idOut.clear();
		*context  = g_ctxOut.c_str();
		*identity = g_idOut.c_str();
		return false;
	}
	// The server doesn't know the camera, so the listener is the character's head, facing its look direction.
	const float up[3] = { 0.f, 1.f, 0.f };
	std::memcpy(avatarPos, me.pos, 12);
	std::memcpy(avatarDir, me.dir, 12);
	std::memcpy(avatarAxis, up, 12);
	std::memcpy(cameraPos, me.pos, 12);
	std::memcpy(cameraDir, me.dir, 12);
	std::memcpy(cameraAxis, up, 12);
	g_ctxOut = g_snap.context;
	{
		std::lock_guard< std::mutex > idLock(g_idMtx);
		g_idOut = g_ident.myDisplayName;
	}
	*context  = g_ctxOut.c_str();
	*identity = g_idOut.c_str();
	return true;
}

void mumble_shutdownPositionalData() {}

MumbleStringWrapper mumble_getPositionalDataContextPrefix() { return wrap("Valheim"); }

// ---- name announcements ------------------------------------------------------------------------

bool mumble_onReceiveData(mumble_connection_t, mumble_userid_t sender, const uint8_t *data, size_t dataLength,
						  const char *dataID) {
	if (!dataID || std::strcmp(dataID, vr::kNameDataID) != 0) {
		return false;
	}
	std::string name;
	if (vr::parseNameAnnouncement(data, dataLength, name)) {
		std::lock_guard< std::mutex > lock(g_idMtx);
		g_ident.announced[sender] = vr::normalizeName(name);
	}
	return true;
}

void mumble_onUserRemoved(mumble_connection_t, mumble_userid_t userID) {
	std::lock_guard< std::mutex > lock(g_idMtx);
	g_ident.announced.erase(userID);
}

void mumble_onServerDisconnected(mumble_connection_t) {
	{
		std::lock_guard< std::mutex > lock(g_idMtx);
		g_ident.announced.clear();
		g_ident.mumbleNames.clear();
	}
	g_resetVoices = true;
}

// ---- the radio effect --------------------------------------------------------------------------

bool mumble_onAudioSourceFetched(float *pcm, uint32_t sampleCount, uint16_t channelCount, uint32_t sampleRate,
								 bool isSpeech, mumble_userid_t userID) {
	if (!isSpeech) {
		return false;
	}
	if (g_resetVoices.exchange(false)) {
		g_voices.clear();
	}
	const auto now = Clock::now();

	std::string speakerName;
	vr::RadioOverrides overrides;
	{
		std::lock_guard< std::mutex > lock(g_idMtx);
		auto a = g_ident.announced.find(userID);
		if (a != g_ident.announced.end()) {
			speakerName = a->second;
		} else {
			auto m = g_ident.mumbleNames.find(userID);
			if (m != g_ident.mumbleNames.end()) {
				speakerName = m->second;
			}
		}
		overrides = g_ident.overrides;
	}

	vr::RadioConfig cfg;
	float wet = 0.f, distance = 0.f;
	bool matched = false;
	{
		std::lock_guard< std::mutex > lock(g_snapMtx);
		vr::PlayerPose me;
		if (!findSelf(now, me)) {
			return false;
		}
		cfg = g_snap.radio;
		overrides.applyTo(cfg);
		auto it = speakerName.empty() ? g_snap.players.end() : g_snap.players.find(speakerName);
		if (it != g_snap.players.end()) {
			const float dx = it->second.pos[0] - me.pos[0];
			const float dy = it->second.pos[1] - me.pos[1];
			const float dz = it->second.pos[2] - me.pos[2];
			distance       = std::sqrt(dx * dx + dy * dy + dz * dz);
			wet            = vr::radioAmount(distance, cfg.start, std::max(cfg.full, cfg.start));
			matched        = true;
		}
	}
	reportSpeaker(userID, speakerName, matched, distance, cfg);
	if (!cfg.enabled) {
		return false;
	}

	// With KeepVolume the plugin owns the distance fade: undo Mumble's attenuation and apply ours.
	float comp        = 1.f;
	bool ownsVolume   = false;
	if (cfg.keepVolume && matched) {
		PaSettings pa;
		{
			std::lock_guard< std::mutex > lock(g_paMtx);
			pa = g_pa;
		}
		if (pa.valid) {
			const float att = mumbleAttenuation(distance, pa);
			comp = std::clamp(desiredGain(distance, cfg, pa) / std::max(att, 1e-4f), 0.f, std::max(cfg.maxBoost, 1.f));
			ownsVolume = std::fabs(comp - 1.f) > 1e-3f;
		}
	}

	auto vit = g_voices.find(userID);
	if (!ownsVolume && wet <= 0.f && (vit == g_voices.end() || vit->second.currentWet() < 1e-4f)) {
		// Close by, nothing to change and not fading out of radio mode: leave the audio untouched.
		if (vit != g_voices.end()) {
			vit->second.process(pcm, sampleCount, channelCount, sampleRate, 0.f, 1.f, nowSeconds(), {});
		}
		return false;
	}

	vr::RadioParams params;
	params.noise   = cfg.noise;
	params.drive   = cfg.drive;
	params.crackle = cfg.crackle;
	params.squelch = cfg.squelch;
	g_voices[userID].process(pcm, sampleCount, channelCount, sampleRate, wet, comp, nowSeconds(), params);
	return true;
}
