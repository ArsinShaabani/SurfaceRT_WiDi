// SurfaceRT-WiDi - PC sender as a real executable.
//
// Compiles with the C# compiler that ships inside Windows (.NET Framework 4.x),
// so no Visual Studio, SDK or third-party tool is required:
//
//   %WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe /out:widi-pc.exe ^
//       tools\pc\lib\WiDiNative.cs tools\pc\src\WiDiSender.cs /r:System.Drawing.dll
//
// or simply:  tools\pc\Build-Sender.cmd
//
// Written for C# 5 (the version in the in-box compiler): no string interpolation,
// no null-conditional operators, no expression-bodied members.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace SurfaceRTWiDi
{
    internal sealed class Options
    {
        public string Host;
        public int Port = 47000;
        public int DiscoveryPort = 47001;
        public int Fps = 20;
        public int Quality = 60;
        public int MaxWidth = 1366;
        public bool NoInput;
        public bool ListOnly;
        public bool SelfTest;
        public bool Help;
        public int Seconds;
    }

    internal sealed class SinkInfo
    {
        public string Address;
        public int Port;
        public string Name;
        public int Width;
        public int Height;
    }

    internal sealed class SinkStats
    {
        public uint Decoded;
        public uint Dropped;
        public double DecodeMs;
        public double Fps;
    }

    internal static class Program
    {
        private const byte kBeaconMagic0 = 0x57;  // W
        private const byte kBeaconMagic1 = 0x49;  // I
        private const byte kBeaconMagic2 = 0x44;  // D
        private const byte kBeaconMagic3 = 0x49;  // I
        private const byte kBeaconMagic4 = 0x44;  // D
        private const byte kBeaconMagic5 = 0x53;  // S

        private static volatile bool s_stop;

        private static void Log(string message)
        {
            Console.WriteLine("[widi] " + message);
        }

        private static void LogWarning(string message)
        {
            ConsoleColor previous = Console.ForegroundColor;
            Console.ForegroundColor = ConsoleColor.Yellow;
            Console.WriteLine("[widi] " + message);
            Console.ForegroundColor = previous;
        }

        private static void LogError(string message)
        {
            ConsoleColor previous = Console.ForegroundColor;
            Console.ForegroundColor = ConsoleColor.Red;
            Console.Error.WriteLine("[widi] " + message);
            Console.ForegroundColor = previous;
        }

        private static void Usage()
        {
            Console.WriteLine("widi-pc - stream this Windows desktop to a Surface RT");
            Console.WriteLine();
            Console.WriteLine("  widi-pc                        discover the tablet and start streaming");
            Console.WriteLine("  widi-pc --host 192.168.1.50    connect to a known address");
            Console.WriteLine("  widi-pc --list                 only list receivers on the LAN");
            Console.WriteLine("  widi-pc --selftest             verify the wire encoder, then exit");
            Console.WriteLine();
            Console.WriteLine("  --port N            receiver TCP port (default 47000)");
            Console.WriteLine("  --discovery-port N  UDP discovery port (default 47001)");
            Console.WriteLine("  --fps N             maximum frame rate (default 20)");
            Console.WriteLine("  --quality N         maximum JPEG quality, 20-90 (default 60)");
            Console.WriteLine("  --max-width N       capture width cap (default 1366)");
            Console.WriteLine("  --no-input          do not inject tablet touch as mouse input");
            Console.WriteLine("  --seconds N         stop automatically after N seconds (0 = forever)");
            Console.WriteLine();
            Console.WriteLine("Press Ctrl+C to stop. Nothing is installed on this PC.");
        }

        private static string ParseInt(string text, int low, int high, string name, out int value)
        {
            value = 0;
            int parsed;
            if (!int.TryParse(text, out parsed))
            {
                return name + " expects a number";
            }
            if (parsed < low || parsed > high)
            {
                return name + " must be between " + low + " and " + high;
            }
            value = parsed;
            return null;
        }

        private static string ParseArguments(string[] args, Options options)
        {
            for (int i = 0; i < args.Length; i++)
            {
                string arg = args[i];
                string error = null;
                int value = 0;
                switch (arg)
                {
                    case "--help":
                    case "-h":
                    case "/?":
                        options.Help = true;
                        break;
                    case "--selftest":
                        options.SelfTest = true;
                        break;
                    case "--list":
                        options.ListOnly = true;
                        break;
                    case "--no-input":
                        options.NoInput = true;
                        break;
                    case "--host":
                        if (i + 1 >= args.Length) return "--host needs an address";
                        options.Host = args[++i];
                        break;
                    case "--port":
                        if (i + 1 >= args.Length) return "--port needs a number";
                        error = ParseInt(args[++i], 1, 65535, "--port", out value);
                        if (error != null) return error;
                        options.Port = value;
                        break;
                    case "--discovery-port":
                        if (i + 1 >= args.Length) return "--discovery-port needs a number";
                        error = ParseInt(args[++i], 1, 65535, "--discovery-port", out value);
                        if (error != null) return error;
                        options.DiscoveryPort = value;
                        break;
                    case "--fps":
                        if (i + 1 >= args.Length) return "--fps needs a number";
                        error = ParseInt(args[++i], 1, 60, "--fps", out value);
                        if (error != null) return error;
                        options.Fps = value;
                        break;
                    case "--quality":
                        if (i + 1 >= args.Length) return "--quality needs a number";
                        error = ParseInt(args[++i], 20, 90, "--quality", out value);
                        if (error != null) return error;
                        options.Quality = value;
                        break;
                    case "--max-width":
                        if (i + 1 >= args.Length) return "--max-width needs a number";
                        error = ParseInt(args[++i], 320, 3840, "--max-width", out value);
                        if (error != null) return error;
                        options.MaxWidth = value;
                        break;
                    case "--seconds":
                        if (i + 1 >= args.Length) return "--seconds needs a number";
                        error = ParseInt(args[++i], 0, 86400, "--seconds", out value);
                        if (error != null) return error;
                        options.Seconds = value;
                        break;
                    default:
                        return "unknown option: " + arg;
                }
            }
            return null;
        }

        // -------------------------------------------------------------------
        // Entry point
        // -------------------------------------------------------------------

        private static int Main(string[] args)
        {
            Options options = new Options();
            string error = ParseArguments(args, options);
            if (error != null)
            {
                LogError(error);
                Usage();
                return 2;
            }
            if (options.Help)
            {
                Usage();
                return 0;
            }
            if (options.SelfTest)
            {
                return SelfTest();
            }

            Console.CancelKeyPress += delegate(object sender, ConsoleCancelEventArgs e)
            {
                e.Cancel = true;
                s_stop = true;
            };

            try
            {
                return Run(options);
            }
            catch (Exception ex)
            {
                LogError(ex.Message);
                return 1;
            }
        }

        // -------------------------------------------------------------------
        // Discovery
        // -------------------------------------------------------------------

        private static bool IsBeacon(byte[] data)
        {
            return data.Length >= 15 && data[0] == kBeaconMagic0 && data[1] == kBeaconMagic1 &&
                   data[2] == kBeaconMagic2 && data[3] == kBeaconMagic3 &&
                   data[4] == kBeaconMagic4 && data[5] == kBeaconMagic5;
        }

        private static List<SinkInfo> Discover(int discoveryPort, int timeoutMs)
        {
            List<SinkInfo> found = new List<SinkInfo>();
            UdpClient udp = new UdpClient();
            try
            {
                udp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress,
                                           true);
                udp.Client.Bind(new IPEndPoint(IPAddress.Any, discoveryPort));
                udp.Client.ReceiveTimeout = 400;

                IPEndPoint remote = new IPEndPoint(IPAddress.Any, 0);
                DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
                while (DateTime.UtcNow < deadline)
                {
                    byte[] data;
                    try
                    {
                        data = udp.Receive(ref remote);
                    }
                    catch (SocketException)
                    {
                        continue;
                    }
                    if (data == null || !IsBeacon(data))
                    {
                        continue;
                    }

                    SinkInfo info = new SinkInfo();
                    info.Address = remote.Address.ToString();
                    info.Port = BitConverter.ToUInt16(data, 7);
                    info.Width = BitConverter.ToUInt16(data, 9);
                    info.Height = BitConverter.ToUInt16(data, 11);
                    int nameLength = data[14];
                    info.Name = (nameLength > 0 && data.Length >= 15 + nameLength)
                                    ? Encoding.UTF8.GetString(data, 15, nameLength)
                                    : "Surface RT";

                    bool duplicate = false;
                    for (int i = 0; i < found.Count; i++)
                    {
                        if (found[i].Address == info.Address && found[i].Port == info.Port)
                        {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate)
                    {
                        found.Add(info);
                    }
                }
            }
            finally
            {
                udp.Close();
            }
            return found;
        }

        // -------------------------------------------------------------------
        // Framing helpers (mirror of the PowerShell sender, kept tiny)
        // -------------------------------------------------------------------

        private static byte[] ReadExactly(Stream stream, int count)
        {
            byte[] buffer = new byte[count];
            int read = 0;
            while (read < count)
            {
                int n = stream.Read(buffer, read, count - read);
                if (n <= 0)
                {
                    throw new IOException("the tablet closed the connection");
                }
                read += n;
            }
            return buffer;
        }

        private static byte[] ReadMessage(Stream stream, int timeoutMs, out byte type)
        {
            stream.ReadTimeout = timeoutMs;
            byte[] header = ReadExactly(stream, 20);
            type = header[5];
            int length = (int)BitConverter.ToUInt32(header, 12);
            if (length < 0 || length > 16 * 1024 * 1024)
            {
                throw new IOException("implausible payload length " + length);
            }
            return length > 0 ? ReadExactly(stream, length) : new byte[0];
        }

        private static string ReadName(byte[] ackPayload)
        {
            if (ackPayload.Length < 13)
            {
                return "Surface RT";
            }
            int length = ackPayload[12];
            if (length > 0 && ackPayload.Length >= 13 + length)
            {
                return Encoding.UTF8.GetString(ackPayload, 13, length);
            }
            return "Surface RT";
        }

        private static int SafeAvailable(TcpClient client)
        {
            try
            {
                return client.Available;
            }
            catch (Exception)
            {
                return -1;
            }
        }

        // -------------------------------------------------------------------
        // Session
        // -------------------------------------------------------------------

        private static int Run(Options o)
        {
            ScreenCapture.SetProcessDPIAware();

            string host = o.Host;
            int port = o.Port;

            if (o.ListOnly)
            {
                Log("searching for Surface RT receivers (3 s)...");
                List<SinkInfo> sinks = Discover(o.DiscoveryPort, 3000);
                if (sinks.Count == 0)
                {
                    LogWarning("no receiver answered. Start SurfaceRT-WiDi on the tablet, or use --host <ip>.");
                    return 1;
                }
                for (int i = 0; i < sinks.Count; i++)
                {
                    Console.WriteLine("  " + sinks[i].Address.PadRight(16) + " tcp/" +
                                      sinks[i].Port.ToString().PadRight(6) + " " + sinks[i].Name +
                                      " (" + sinks[i].Width + "x" + sinks[i].Height + ")");
                }
                return 0;
            }

            if (host == null)
            {
                Log("looking for a receiver on the LAN...");
                List<SinkInfo> sinks = Discover(o.DiscoveryPort, 3000);
                if (sinks.Count == 0)
                {
                    LogWarning("no receiver found. Start SurfaceRT-WiDi on the tablet, or pass --host <ip>.");
                    return 1;
                }
                host = sinks[0].Address;
                port = sinks[0].Port;
                Log("using " + host + " (" + sinks[0].Name + ")");
            }

            TcpClient client = null;
            NetworkStream stream = null;
            ScreenCapture capture = null;
            int result = 0;
            try
            {
                Log("connecting to " + host + ":" + port + "...");
                client = new TcpClient();
                client.NoDelay = true;
                IAsyncResult pending = client.BeginConnect(host, port, null, null);
                if (!pending.AsyncWaitHandle.WaitOne(5000, false))
                {
                    throw new IOException("timeout connecting to " + host + ":" + port);
                }
                client.EndConnect(pending);
                stream = client.GetStream();
                stream.WriteTimeout = 10000;  // a dead link must fail, not hang

                byte[] hello = Wire.Hello(Environment.MachineName, 1920, 1080, o.Fps, (byte)0x07);
                stream.Write(hello, 0, hello.Length);

                byte ackType = 0;
                byte[] ack = ReadMessage(stream, 5000, out ackType);
                if (ackType != Wire.T_HELLO_ACK)
                {
                    throw new IOException("unexpected handshake reply (type " + ackType + ")");
                }
                if (ack.Length < 12 || ack[10] != 1)
                {
                    throw new IOException("the tablet rejected the session (reason " +
                                          (ack.Length >= 12 ? ack[11].ToString() : "?") + ")");
                }
                int sinkWidth = BitConverter.ToUInt16(ack, 2);
                int sinkHeight = BitConverter.ToUInt16(ack, 4);
                Log("connected to '" + ReadName(ack) + "' (" + sinkWidth + "x" + sinkHeight + ")");

                capture = new ScreenCapture();
                StreamFrames(o, capture, stream, client, sinkWidth);
            }
            catch (Exception ex)
            {
                LogError(ex.Message);
                result = 1;
            }
            finally
            {
                if (stream != null)
                {
                    try
                    {
                        byte[] bye = Wire.Bye();
                        stream.Write(bye, 0, bye.Length);
                        stream.Flush();
                    }
                    catch (Exception)
                    {
                    }
                }
                if (capture != null)
                {
                    capture.Dispose();
                }
                if (client != null)
                {
                    client.Close();
                }
                Log("stopped");
            }
            return result;
        }

        // -------------------------------------------------------------------
        // Streaming loop with the same adaptation policy as the C++ sink
        // -------------------------------------------------------------------

        private static void StreamFrames(Options o, ScreenCapture capture, NetworkStream stream,
                                        TcpClient client, int sinkWidth)
        {
            int targetWidth = Math.Min(o.MaxWidth, capture.SourceWidth);
            if (sinkWidth > 0 && sinkWidth < targetWidth)
            {
                targetWidth = sinkWidth;
            }

            int quality = o.Quality;
            int fps = o.Fps;
            double frameMs = 1000.0 / fps;
            uint sequence = 10;
            int sent = 0;
            long lastReport = 0;
            SinkStats stats = null;
            Stopwatch clock = Stopwatch.StartNew();
            double nextDue = 0.0;

            Log("streaming " + capture.SourceWidth + "x" + capture.SourceHeight + " -> " +
                targetWidth + "px @ " + fps + " fps, quality " + quality);
            Log("press Ctrl+C to stop");

            while (!s_stop)
            {
                if (o.Seconds > 0 && clock.Elapsed.TotalSeconds >= o.Seconds)
                {
                    break;
                }

                byte[] jpeg = capture.CaptureFrame(targetWidth, quality);
                uint stamp = (uint)(clock.ElapsedMilliseconds & 0x7FFFFFFF);
                byte[] message = Wire.Frame(sequence, capture.LastWidth, capture.LastHeight, 1,
                                            stamp, jpeg, jpeg.Length);
                sequence++;
                stream.Write(message, 0, message.Length);
                sent++;

                // Drain feedback without stalling the pipeline.
                int guard = 0;
                while (guard < 8 && SafeAvailable(client) >= 20)
                {
                    guard++;
                    byte type = 0;
                    byte[] payload = null;
                    try
                    {
                        payload = ReadMessage(stream, 400, out type);
                    }
                    catch (IOException)
                    {
                        break;
                    }
                    if (payload == null)
                    {
                        break;
                    }
                    if (type == Wire.T_STATS && payload.Length >= 12)
                    {
                        SinkStats fresh = new SinkStats();
                        fresh.Decoded = BitConverter.ToUInt32(payload, 0);
                        fresh.Dropped = BitConverter.ToUInt32(payload, 4);
                        fresh.DecodeMs = BitConverter.ToUInt16(payload, 8) / 16.0;
                        fresh.Fps = BitConverter.ToUInt16(payload, 10) / 16.0;
                        stats = fresh;
                    }
                    else if (type == Wire.T_INPUT && payload.Length >= 10 && !o.NoInput)
                    {
                        ApplyInput(payload);
                    }
                    else if (type == Wire.T_BYE)
                    {
                        LogWarning("the tablet closed the session");
                        s_stop = true;
                        break;
                    }
                }

                if (clock.ElapsedMilliseconds - lastReport >= 1000)
                {
                    lastReport = clock.ElapsedMilliseconds;
                    if (stats != null)
                    {
                        Console.WriteLine("[widi] " + sent + " fps sent | tablet " +
                                          stats.Fps.ToString("0.0") + " fps, " +
                                          stats.DecodeMs.ToString("0.0") + " ms decode, " +
                                          stats.Dropped + " dropped | q" + quality + " " +
                                          targetWidth + "px");
                        double budget = (1000.0 / fps) * 0.7;
                        if (stats.DecodeMs > budget)
                        {
                            if (quality > 32)
                            {
                                quality -= 8;
                            }
                            else if (fps > 12)
                            {
                                fps -= 2;
                                frameMs = 1000.0 / fps;
                            }
                            else if (targetWidth > 960)
                            {
                                targetWidth -= 120;
                            }
                        }
                        else if (stats.DecodeMs < budget / 2.0)
                        {
                            if (quality < o.Quality)
                            {
                                quality += 3;
                            }
                            else if (fps < o.Fps)
                            {
                                fps += 1;
                                frameMs = 1000.0 / fps;
                            }
                            else if (targetWidth < o.MaxWidth)
                            {
                                targetWidth += 80;
                            }
                        }
                        stats = null;
                    }
                    else
                    {
                        Log(sent + " fps sent (no feedback from the tablet yet)");
                    }
                }

                nextDue += frameMs;
                double wait = nextDue - clock.ElapsedMilliseconds;
                if (wait > 0)
                {
                    Thread.Sleep((int)Math.Ceiling(wait));
                }
                else
                {
                    nextDue = clock.ElapsedMilliseconds;
                }
            }
        }

        private static void ApplyInput(byte[] payload)
        {
            int kind = payload[0];
            int flags = payload[1];
            ushort x = BitConverter.ToUInt16(payload, 2);
            ushort y = BitConverter.ToUInt16(payload, 4);

            if (kind == 2)
            {
                Pointer.Wheel(BitConverter.ToInt32(payload, 6));
                return;
            }
            if (kind != 0 && kind != 1)
            {
                return;
            }

            bool down = (flags & 0x08) != 0;
            bool right = (flags & 0x02) != 0;
            bool touch = (flags & 0x10) != 0;
            if (down)
            {
                Pointer.Button(x, y, true, right);
            }
            else if (touch)
            {
                Pointer.Button(x, y, false, right);
            }
            else
            {
                Pointer.Move(x, y);
            }
        }

        // -------------------------------------------------------------------
        // Self test: proves the build works before a tablet is involved
        // -------------------------------------------------------------------

        private static int Check(string name, bool ok, string detail)
        {
            if (ok)
            {
                Console.WriteLine("  PASS  " + name);
                return 0;
            }
            Console.WriteLine("  FAIL  " + name + (string.IsNullOrEmpty(detail) ? "" : " (" + detail + ")"));
            return 1;
        }

        private static int SelfTest()
        {
            int failures = 0;
            Console.WriteLine("widi-pc self test - CLR " + Environment.Version + ", " +
                              Environment.OSVersion.VersionString);

            // 1. CRC-32 must match the C++ receiver (poly 0xEDB88320, reflected).
            byte[] digits = Encoding.ASCII.GetBytes("123456789");
            uint crc = Wire.Crc32(digits, 0, digits.Length);
            failures += Check("crc32 vector", crc == 0xCBF43926u, "0x" + crc.ToString("X8"));
            failures += Check("crc32 empty", Wire.Crc32(new byte[0], 0, 0) == 0u, "");

            // 2. Frame message layout (20-byte header + 10-byte frame header + pixels).
            byte[] pixels = new byte[] { 1, 2, 3, 4 };
            byte[] frame = Wire.Frame(7u, 2, 1, 1, 1234u, pixels, pixels.Length);
            failures += Check("frame total size", frame.Length == 34, frame.Length.ToString());
            failures += Check("frame magic",
                              frame[0] == 0x57 && frame[1] == 0x49 && frame[2] == 0x44 &&
                                  frame[3] == 0x49,
                              "");
            failures += Check("frame version", frame[4] == 1, frame[4].ToString());
            failures += Check("frame type", frame[5] == Wire.T_FRAME, frame[5].ToString());
            failures += Check("frame sequence", BitConverter.ToUInt32(frame, 8) == 7u, "");
            failures += Check("frame payload length", BitConverter.ToUInt32(frame, 12) == 14u,
                              BitConverter.ToUInt32(frame, 12).ToString());
            failures += Check("frame crc covers payload only",
                              BitConverter.ToUInt32(frame, 16) == Wire.Crc32(frame, 20, 14), "");
            failures += Check("frame width", BitConverter.ToUInt16(frame, 20) == 2, "");
            failures += Check("frame height", BitConverter.ToUInt16(frame, 22) == 1, "");
            failures += Check("frame format", frame[24] == 1, frame[24].ToString());
            failures += Check("frame timestamp", BitConverter.ToUInt32(frame, 26) == 1234u, "");
            failures += Check("frame pixel offset", frame[30] == 1 && frame[33] == 4, "");

            // 3. Hello payload layout, as parsed by SinkSession::handle_message.
            byte[] hello = Wire.Hello("ABC", 1920, 1080, 30, 0x07);
            failures += Check("hello type", hello[5] == Wire.T_HELLO, hello[5].ToString());
            failures += Check("hello payload length", BitConverter.ToUInt32(hello, 12) == 12u,
                              BitConverter.ToUInt32(hello, 12).ToString());
            failures += Check("hello version", BitConverter.ToUInt16(hello, 20) == 1, "");
            failures += Check("hello max width", BitConverter.ToUInt16(hello, 22) == 1920, "");
            failures += Check("hello max height", BitConverter.ToUInt16(hello, 24) == 1080, "");
            failures += Check("hello fps", hello[26] == 30, hello[26].ToString());
            failures += Check("hello caps", hello[27] == 0x07, hello[27].ToString());
            failures += Check("hello name length", hello[28] == 3, hello[28].ToString());
            failures += Check("hello name", hello[29] == (byte)'A' && hello[31] == (byte)'C', "");

            // 4. HelloAck parsing exactly as Run() does it.
            byte[] ack = new byte[16];
            ack[0] = 1;
            ack[2] = 0x56;
            ack[3] = 0x05;  // 1366
            ack[4] = 0x00;
            ack[5] = 0x03;  // 768
            ack[10] = 1;    // accepted
            ack[11] = 0;
            ack[12] = 3;
            ack[13] = (byte)'R';
            ack[14] = (byte)'T';
            ack[15] = (byte)'!';
            failures += Check("ack accepted", ack[10] == 1, "");
            failures += Check("ack width", BitConverter.ToUInt16(ack, 2) == 1366, "");
            failures += Check("ack height", BitConverter.ToUInt16(ack, 4) == 768, "");
            failures += Check("ack name", ReadName(ack) == "RT!", ReadName(ack));

            // 5. Beacon parsing exactly as Discover() does it.
            byte[] beacon = new byte[17];
            beacon[0] = kBeaconMagic0;
            beacon[1] = kBeaconMagic1;
            beacon[2] = kBeaconMagic2;
            beacon[3] = kBeaconMagic3;
            beacon[4] = kBeaconMagic4;
            beacon[5] = kBeaconMagic5;
            beacon[6] = 1;
            Wire.PutU16(beacon, 7, 47000);
            Wire.PutU16(beacon, 9, 1366);
            Wire.PutU16(beacon, 11, 768);
            beacon[13] = 0x09;
            beacon[14] = 2;
            beacon[15] = (byte)'R';
            beacon[16] = (byte)'T';
            failures += Check("beacon recognized", IsBeacon(beacon), "");
            failures += Check("beacon port", BitConverter.ToUInt16(beacon, 7) == 47000, "");
            failures += Check("beacon geometry",
                              BitConverter.ToUInt16(beacon, 9) == 1366 &&
                                  BitConverter.ToUInt16(beacon, 11) == 768,
                              "");
            failures += Check("beacon name",
                              Encoding.UTF8.GetString(beacon, 15, beacon[14]) == "RT", "");

            // 6. Bye message.
            byte[] bye = Wire.Bye();
            failures += Check("bye type and payload",
                              bye[5] == Wire.T_BYE && BitConverter.ToUInt32(bye, 12) == 1u && bye[20] == 0,
                              "");

            if (failures == 0)
            {
                Console.WriteLine("self test passed - the sender speaks protocol v1 correctly");
                return 0;
            }
            Console.WriteLine("self test FAILED with " + failures + " problem(s)");
            return 1;
        }
    }
}
