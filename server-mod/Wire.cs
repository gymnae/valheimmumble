// Wire format shared with mumble-plugin/src/Protocol.h. Kept free of Unity/BepInEx types so it can be
// tested outside the game (see mumble-plugin/tools/crosstest).
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace ValheimMumbleRadio
{
    public struct WirePlayer
    {
        public string Name;
        public float Px, Py, Pz; // eye position
        public float Dx, Dy, Dz; // facing
    }

    public struct WireRadio
    {
        public bool Enabled, Squelch, KeepVolume;
        public float Start, Full, Noise, Drive, Crackle, RadioVolume, MaxBoost;
    }

    public enum HelloResult
    {
        NeedMoreData,
        Ok,
        Malformed,
        WrongKey,
    }

    public static class Wire
    {
        public const uint HelloMagic = 0x31434856;    // "VHC1"
        public const uint SnapshotMagic = 0x31534856; // "VHS1"
        public const ushort Version = 1;
        public const int MaxHelloBytes = 1024;

        /// <summary>Writes a complete length-prefixed snapshot frame into <paramref name="frame"/>.</summary>
        public static void WriteSnapshot(MemoryStream frame, BinaryWriter w, string context, in WireRadio radio,
            List<WirePlayer> players)
        {
            frame.SetLength(0);
            w.Write(0); // frame length placeholder
            w.Write(SnapshotMagic);
            w.Write(Version);
            w.Write((ushort)0);
            WriteString(w, context);
            w.Write(radio.Start);
            w.Write(Math.Max(radio.Full, radio.Start));
            w.Write(radio.Noise);
            w.Write(radio.Drive);
            w.Write(radio.Crackle);
            w.Write(radio.RadioVolume);
            w.Write(radio.MaxBoost);
            w.Write((byte)(radio.Enabled ? 1 : 0));
            w.Write((byte)(radio.Squelch ? 1 : 0));
            w.Write((byte)(radio.KeepVolume ? 1 : 0));
            w.Write((byte)0);
            int count = Math.Min(players.Count, ushort.MaxValue);
            w.Write((ushort)count);
            for (int i = 0; i < count; i++)
            {
                WirePlayer p = players[i];
                WriteString(w, p.Name);
                w.Write(p.Px); w.Write(p.Py); w.Write(p.Pz);
                w.Write(p.Dx); w.Write(p.Dy); w.Write(p.Dz);
            }
            w.Flush();

            byte[] buf = frame.GetBuffer();
            int payload = (int)frame.Length - 4;
            buf[0] = (byte)payload;
            buf[1] = (byte)(payload >> 8);
            buf[2] = (byte)(payload >> 16);
            buf[3] = (byte)(payload >> 24);
        }

        /// <summary>Checks a (possibly partial) hello frame received from a Mumble plugin.</summary>
        public static HelloResult ParseHello(byte[] data, int available, string expectedKey)
        {
            if (available < 4)
                return HelloResult.NeedMoreData;
            int len = BitConverter.ToInt32(data, 0);
            if (len < 8 || len > MaxHelloBytes)
                return HelloResult.Malformed;
            if (available < 4 + len)
                return HelloResult.NeedMoreData;

            uint magic = BitConverter.ToUInt32(data, 4);
            ushort version = BitConverter.ToUInt16(data, 8);
            ushort keyLen = BitConverter.ToUInt16(data, 10);
            if (magic != HelloMagic || version != Version || keyLen > len - 8)
                return HelloResult.Malformed;
            string key = Encoding.UTF8.GetString(data, 12, keyLen);
            return key == (expectedKey ?? "") ? HelloResult.Ok : HelloResult.WrongKey;
        }

        private static void WriteString(BinaryWriter w, string s)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(s ?? "");
            int len = Math.Min(bytes.Length, 1024);
            w.Write((ushort)len);
            w.Write(bytes, 0, len);
        }
    }
}
