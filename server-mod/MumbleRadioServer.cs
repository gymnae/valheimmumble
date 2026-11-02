// Server-side half of "Valheim Radio": streams every player's position to the players' Mumble plugins
// over TCP, so the game clients can stay vanilla. Install on the dedicated server (or on the machine
// hosting a world from the game) under BepInEx/plugins.
//
// Wire format: see mumble-plugin/src/Protocol.h
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using BepInEx;
using BepInEx.Configuration;
using UnityEngine;

namespace ValheimMumbleRadio
{
    [BepInPlugin(Guid, "Mumble Radio Server", "2.0.0")]
    public class MumbleRadioServer : BaseUnityPlugin
    {
        public const string Guid = "de.grundstil.valheim.mumbleradio.server";

        private const float EyeHeight = 1.6f;
        private const int MaxHelloBytes = Wire.MaxHelloBytes;
        private const float AuthTimeout = 5f;
        private const float SlowClientTimeout = 5f;

        private ConfigEntry<bool> _enabled;
        private ConfigEntry<int> _port;
        private ConfigEntry<string> _key;
        private ConfigEntry<int> _sendRate;
        private ConfigEntry<int> _maxClients;
        private ConfigEntry<string> _context;

        private ConfigEntry<bool> _radioEnabled;
        private ConfigEntry<float> _radioStart;
        private ConfigEntry<float> _radioFull;
        private ConfigEntry<float> _noise;
        private ConfigEntry<float> _drive;
        private ConfigEntry<float> _crackle;
        private ConfigEntry<bool> _squelch;
        private ConfigEntry<bool> _keepVolume;
        private ConfigEntry<float> _radioVolume;
        private ConfigEntry<float> _maxBoost;

        private TcpListener _listener;
        private readonly List<Client> _clients = new List<Client>();
        private readonly MemoryStream _frame = new MemoryStream(4096);
        private BinaryWriter _writer;
        private float _nextSend;

        private sealed class Client
        {
            public Socket Socket;
            public string Remote;
            public float ConnectedAt;
            public bool Authed;
            public readonly byte[] In = new byte[MaxHelloBytes + 4];
            public int InLen;
            public byte[] Pending;
            public int PendingOffset;
            public float PendingSince;
            public bool Dead;
        }

        private void Awake()
        {
            _enabled = Config.Bind("General", "Enabled", true, "Serve player positions to Mumble.");
            _port = Config.Bind("General", "Port", 47812, "TCP port the Mumble plugins connect to. Open it in your firewall.");
            _key = Config.Bind("General", "Key", "",
                "Shared secret. Only Mumble plugins that know it receive positions. Put it into the Mumble " +
                "channel tag: valheim-radio=host:port/key. Empty = no key.");
            _sendRate = Config.Bind("General", "SendRate", 15,
                new ConfigDescription("Position updates per second.", new AcceptableValueRange<int>(2, 30)));
            _maxClients = Config.Bind("General", "MaxClients", 32, "Maximum simultaneous Mumble connections.");
            _context = Config.Bind("General", "Context", "",
                "Positional-audio context sent to Mumble. Players with different contexts are not positional " +
                "to each other. Generated once if empty.");
            if (string.IsNullOrEmpty(_context.Value))
                _context.Value = "valheim-" + System.Guid.NewGuid().ToString("N").Substring(0, 12);

            _radioEnabled = Config.Bind("Radio", "Enabled", true, "Make far-away players sound like a radio.");
            _radioStart = Config.Bind("Radio", "StartDistance", 30f, "Meters where the radio effect starts fading in.");
            _radioFull = Config.Bind("Radio", "FullDistance", 60f, "Meters where the voice is fully 'radio'.");
            _noise = Config.Bind("Radio", "Static", 0.035f,
                new ConfigDescription("Hiss level at full radio.", new AcceptableValueRange<float>(0f, 0.3f)));
            _drive = Config.Bind("Radio", "Distortion", 4f,
                new ConfigDescription("Saturation at full radio.", new AcceptableValueRange<float>(0f, 15f)));
            _crackle = Config.Bind("Radio", "Crackle", 1f,
                new ConfigDescription("Crackle density (0 = off).", new AcceptableValueRange<float>(0f, 5f)));
            _squelch = Config.Bind("Radio", "Squelch", true, "Short burst of static when a far-away player starts talking.");
            _keepVolume = Config.Bind("Radio", "KeepVolume", true,
                "Radio voices keep a constant volume instead of fading out with distance.");
            _radioVolume = Config.Bind("Radio", "RadioVolume", 0.7f,
                new ConfigDescription("Volume of fully-radio voices when KeepVolume is on.",
                    new AcceptableValueRange<float>(0.05f, 1.5f)));
            _maxBoost = Config.Bind("Radio", "MaxBoost", 4f,
                new ConfigDescription("Safety cap for the KeepVolume gain.", new AcceptableValueRange<float>(1f, 20f)));

            _writer = new BinaryWriter(_frame, Encoding.UTF8);
            _port.SettingChanged += (s, e) => Restart();
            _enabled.SettingChanged += (s, e) => Restart();
            Restart();
        }

        private void OnDestroy() => Stop();

        private void Restart()
        {
            Stop();
            if (!_enabled.Value)
                return;
            try
            {
                _listener = new TcpListener(IPAddress.IPv6Any, _port.Value);
                _listener.Server.DualMode = true;
                _listener.Start();
            }
            catch (Exception)
            {
                // No IPv6 on this host: IPv4 only.
                try
                {
                    _listener = new TcpListener(IPAddress.Any, _port.Value);
                    _listener.Start();
                }
                catch (Exception e)
                {
                    Logger.LogError($"Cannot listen on TCP port {_port.Value}: {e.Message}");
                    _listener = null;
                    return;
                }
            }
            Logger.LogInfo($"Serving positions for Mumble on TCP port {_port.Value} (context '{_context.Value}')");
        }

        private void Stop()
        {
            foreach (Client c in _clients)
                CloseQuietly(c.Socket);
            _clients.Clear();
            try { _listener?.Stop(); } catch (Exception) { }
            _listener = null;
        }

        private void Update()
        {
            if (_listener == null)
                return;

            AcceptNew();
            foreach (Client c in _clients)
                ServiceClient(c);

            if (Time.unscaledTime >= _nextSend)
            {
                _nextSend = Time.unscaledTime + 1f / Mathf.Max(1, _sendRate.Value);
                if (ZNet.instance != null && ZNet.instance.IsServer() && HasAuthedClient())
                {
                    byte[] frame = BuildSnapshot(out int length);
                    foreach (Client c in _clients)
                        if (c.Authed && !c.Dead)
                            SendFrame(c, frame, length);
                }
            }

            _clients.RemoveAll(c =>
            {
                if (!c.Dead)
                    return false;
                CloseQuietly(c.Socket);
                return true;
            });
        }

        private bool HasAuthedClient()
        {
            foreach (Client c in _clients)
                if (c.Authed && !c.Dead)
                    return true;
            return false;
        }

        // ------------------------------------------------------------------ connections

        private void AcceptNew()
        {
            while (_listener.Pending())
            {
                Socket s;
                try { s = _listener.AcceptSocket(); }
                catch (Exception) { return; }
                if (_clients.Count >= _maxClients.Value)
                {
                    CloseQuietly(s);
                    continue;
                }
                s.Blocking = false;
                s.NoDelay = true;
                _clients.Add(new Client { Socket = s, Remote = s.RemoteEndPoint?.ToString() ?? "?", ConnectedAt = Time.unscaledTime });
            }
        }

        private void ServiceClient(Client c)
        {
            try
            {
                // Readable with nothing available = peer closed the connection.
                if (c.Socket.Poll(0, SelectMode.SelectRead))
                {
                    if (c.Socket.Available == 0)
                    {
                        c.Dead = true;
                        return;
                    }
                    if (c.Authed)
                    {
                        // Nothing else is expected after the hello; discard.
                        c.Socket.Receive(c.In, 0, c.In.Length, SocketFlags.None, out SocketError _);
                    }
                    else
                    {
                        int n = c.Socket.Receive(c.In, c.InLen, c.In.Length - c.InLen, SocketFlags.None, out SocketError err);
                        if (err != SocketError.Success && err != SocketError.WouldBlock)
                        {
                            c.Dead = true;
                            return;
                        }
                        c.InLen += Math.Max(0, n);
                        TryAuthenticate(c);
                    }
                }
                if (!c.Authed && Time.unscaledTime - c.ConnectedAt > AuthTimeout)
                    c.Dead = true;
                if (c.Pending != null)
                    Flush(c);
            }
            catch (Exception)
            {
                c.Dead = true;
            }
        }

        private void TryAuthenticate(Client c)
        {
            switch (Wire.ParseHello(c.In, c.InLen, _key.Value))
            {
                case HelloResult.NeedMoreData:
                    return;
                case HelloResult.Ok:
                    c.Authed = true;
                    Logger.LogInfo($"Mumble plugin connected from {c.Remote}");
                    return;
                case HelloResult.WrongKey:
                    Logger.LogWarning($"Mumble plugin from {c.Remote} sent a wrong key");
                    c.Dead = true;
                    return;
                default:
                    c.Dead = true;
                    return;
            }
        }

        private void SendFrame(Client c, byte[] frame, int length)
        {
            if (c.Pending != null)
            {
                // Still busy with the previous frame: skip this one (positions are replaced anyway).
                if (Time.unscaledTime - c.PendingSince > SlowClientTimeout)
                    c.Dead = true;
                return;
            }
            try
            {
                int sent = c.Socket.Send(frame, 0, length, SocketFlags.None, out SocketError err);
                if (err != SocketError.Success && err != SocketError.WouldBlock)
                {
                    c.Dead = true;
                    return;
                }
                sent = Math.Max(0, sent);
                if (sent < length)
                {
                    c.Pending = new byte[length - sent];
                    Buffer.BlockCopy(frame, sent, c.Pending, 0, c.Pending.Length);
                    c.PendingOffset = 0;
                    c.PendingSince = Time.unscaledTime;
                }
            }
            catch (Exception)
            {
                c.Dead = true;
            }
        }

        private void Flush(Client c)
        {
            int sent = c.Socket.Send(c.Pending, c.PendingOffset, c.Pending.Length - c.PendingOffset, SocketFlags.None, out SocketError err);
            if (err != SocketError.Success && err != SocketError.WouldBlock)
            {
                c.Dead = true;
                return;
            }
            c.PendingOffset += Math.Max(0, sent);
            if (c.PendingOffset >= c.Pending.Length)
                c.Pending = null;
            else if (Time.unscaledTime - c.PendingSince > SlowClientTimeout)
                c.Dead = true;
        }

        private static void CloseQuietly(Socket s)
        {
            try { s?.Close(); } catch (Exception) { }
        }

        // ------------------------------------------------------------------ snapshot

        private readonly List<WirePlayer> _players = new List<WirePlayer>();

        private void AddPlayer(string name, Vector3 feet, Vector3 dir)
        {
            Vector3 eye = feet + Vector3.up * EyeHeight;
            _players.Add(new WirePlayer
            {
                Name = name,
                Px = eye.x, Py = eye.y, Pz = eye.z,
                Dx = dir.x, Dy = dir.y, Dz = dir.z,
            });
        }

        private byte[] BuildSnapshot(out int length)
        {
            _players.Clear();
            ZDOMan zdoMan = ZDOMan.instance;
            foreach (ZNetPeer peer in ZNet.instance.GetPeers())
            {
                if (peer == null || string.IsNullOrEmpty(peer.m_playerName))
                    continue;
                // m_refPos is reported by the client itself; the character's ZDO is more up to date when it exists
                // (it doesn't while dead/respawning).
                Vector3 pos = peer.m_refPos; // feet
                Vector3 dir = Vector3.forward;
                ZDO zdo = (zdoMan != null && !peer.m_characterID.IsNone()) ? zdoMan.GetZDO(peer.m_characterID) : null;
                if (zdo != null)
                {
                    pos = zdo.GetPosition();
                    dir = zdo.GetRotation() * Vector3.forward;
                }
                AddPlayer(peer.m_playerName, pos, dir);
            }
            // A world hosted from the game client: the host is not a peer.
            Player local = Player.m_localPlayer;
            if (local != null)
            {
                Transform t = local.transform;
                AddPlayer(local.GetPlayerName(), t.position, t.forward);
            }

            var radio = new WireRadio
            {
                Enabled = _radioEnabled.Value,
                Squelch = _squelch.Value,
                KeepVolume = _keepVolume.Value,
                Start = _radioStart.Value,
                Full = _radioFull.Value,
                Noise = _noise.Value,
                Drive = _drive.Value,
                Crackle = _crackle.Value,
                RadioVolume = _radioVolume.Value,
                MaxBoost = _maxBoost.Value,
            };
            Wire.WriteSnapshot(_frame, _writer, _context.Value, radio, _players);
            length = (int)_frame.Length;
            return _frame.GetBuffer();
        }
    }
}
