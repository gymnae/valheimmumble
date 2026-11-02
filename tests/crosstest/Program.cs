using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using ValheimMumbleRadio;

// usage: snapshot <out.bin> | hello <in.bin> <expectedKey>
if (args[0] == "snapshot")
{
    var ms = new MemoryStream();
    var w = new BinaryWriter(ms, Encoding.UTF8);
    var radio = new WireRadio { Enabled = true, Squelch = false, KeepVolume = true, Start = 25, Full = 70,
        Noise = 0.05f, Drive = 3, Crackle = 0.5f, RadioVolume = 0.8f, MaxBoost = 6 };
    var players = new List<WirePlayer> {
        new WirePlayer { Name = "Ragnar", Px = 10, Py = 31.6f, Pz = 5, Dx = 0, Dy = 0, Dz = 1 },
        new WirePlayer { Name = "Ægir Þórsson", Px = -120.5f, Py = 40, Pz = 900.25f, Dx = 1, Dy = 0, Dz = 0 },
    };
    // Write twice into the same stream to make sure reuse (SetLength(0)) works.
    Wire.WriteSnapshot(ms, w, "stale", radio, new List<WirePlayer>());
    Wire.WriteSnapshot(ms, w, "valheim-abc123", radio, players);
    File.WriteAllBytes(args[1], ms.ToArray());
    Console.WriteLine($"wrote {ms.Length} bytes");
}
else if (args[0] == "hello")
{
    byte[] data = File.ReadAllBytes(args[1]);
    var buf = new byte[Wire.MaxHelloBytes + 4];
    Array.Copy(data, buf, data.Length);
    // also check partial delivery
    Console.WriteLine($"partial={Wire.ParseHello(buf, data.Length - 1, args[2])} full={Wire.ParseHello(buf, data.Length, args[2])}");
}
