// SurfaceRT-WiDi - PC side helper compiled in memory by Send-Screen.ps1 (Add-Type).
// Written for C# 5 so the in-box .NET Framework compiler accepts it. Nothing is
// installed on the PC: the source is compiled into a temporary dynamic assembly.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

namespace SurfaceRTWiDi
{
    /// <summary>Wire format v1 (see docs/PROTOCOL.md).</summary>
    public static class Wire
    {
        public const uint Magic = 0x49444957u; // "WIDI" little endian
        public const byte Version = 1;
        public const byte T_HELLO = 2;
        public const byte T_HELLO_ACK = 3;
        public const byte T_FRAME = 4;
        public const byte T_STATS = 5;
        public const byte T_INPUT = 6;
        public const byte T_PING = 7;
        public const byte T_BYE = 9;

        private static readonly uint[] Table = BuildTable();

        private static uint[] BuildTable()
        {
            uint[] table = new uint[256];
            for (uint i = 0; i < 256; i++)
            {
                uint c = i;
                for (int k = 0; k < 8; k++)
                {
                    c = ((c & 1) != 0) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                table[i] = c;
            }
            return table;
        }

        public static uint Crc32(byte[] data, int offset, int count)
        {
            uint c = 0xFFFFFFFFu;
            for (int i = 0; i < count; i++)
            {
                c = Table[(c ^ data[offset + i]) & 0xFF] ^ (c >> 8);
            }
            return c ^ 0xFFFFFFFFu;
        }

        public static byte[] Message(byte type, ushort flags, uint seq, byte[] payload, int count)
        {
            int length = count < 0 ? 0 : count;
            byte[] message = new byte[20 + length];
            PutU32(message, 0, Magic);
            message[4] = Version;
            message[5] = type;
            PutU16(message, 6, flags);
            PutU32(message, 8, seq);
            PutU32(message, 12, (uint)length);
            uint crc = length > 0 ? Crc32(payload, 0, length) : 0u;
            PutU32(message, 16, crc);
            if (length > 0) Array.Copy(payload, 0, message, 20, length);
            return message;
        }

        public static byte[] Hello(string name, int maxWidth, int maxHeight, int maxFps, byte caps)
        {
            byte[] n = System.Text.Encoding.UTF8.GetBytes(name ?? "Windows-PC");
            int nameLength = n.Length > 60 ? 60 : n.Length;
            byte[] payload = new byte[9 + nameLength];
            PutU16(payload, 0, Version);
            PutU16(payload, 2, (ushort)Math.Max(0, Math.Min(65535, maxWidth)));
            PutU16(payload, 4, (ushort)Math.Max(0, Math.Min(65535, maxHeight)));
            payload[6] = (byte)Math.Max(1, Math.Min(255, maxFps));
            payload[7] = caps;
            payload[8] = (byte)nameLength;
            Array.Copy(n, 0, payload, 9, nameLength);
            return Message(T_HELLO, 0, 1, payload, payload.Length);
        }

        public static byte[] Frame(uint seq, int width, int height, byte format, uint timestampMs,
                                   byte[] pixels, int pixelCount)
        {
            byte[] payload = new byte[10 + pixelCount];
            PutU16(payload, 0, (ushort)width);
            PutU16(payload, 2, (ushort)height);
            payload[4] = format;
            payload[5] = 0;
            PutU32(payload, 6, timestampMs);
            Array.Copy(pixels, 0, payload, 10, pixelCount);
            return Message(T_FRAME, 0, seq, payload, payload.Length);
        }

        public static byte[] Bye()
        {
            return Message(T_BYE, 0, 0, new byte[] { 0 }, 1);
        }

        public static void PutU16(byte[] b, int o, ushort v)
        {
            b[o] = (byte)(v & 0xFF);
            b[o + 1] = (byte)((v >> 8) & 0xFF);
        }

        public static void PutU32(byte[] b, int o, uint v)
        {
            b[o] = (byte)(v & 0xFF);
            b[o + 1] = (byte)((v >> 8) & 0xFF);
            b[o + 2] = (byte)((v >> 16) & 0xFF);
            b[o + 3] = (byte)((v >> 24) & 0xFF);
        }
    }

    /// <summary>GDI desktop capture + JPEG encode with persistent GDI objects.</summary>
    public sealed class ScreenCapture : IDisposable
    {
        [DllImport("user32.dll")] private static extern IntPtr GetDC(IntPtr hWnd);
        [DllImport("user32.dll")] private static extern int ReleaseDC(IntPtr hWnd, IntPtr hDC);
        [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);
        [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
        [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleDC(IntPtr hdc);
        [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleBitmap(IntPtr hdc, int w, int h);
        [DllImport("gdi32.dll")] private static extern IntPtr SelectObject(IntPtr hdc, IntPtr obj);
        [DllImport("gdi32.dll")] private static extern bool DeleteObject(IntPtr obj);
        [DllImport("gdi32.dll")] private static extern bool DeleteDC(IntPtr hdc);
        [DllImport("gdi32.dll")]
        private static extern bool BitBlt(IntPtr dst, int x, int y, int w, int h, IntPtr src, int sx,
                                          int sy, int rop);

        private const int SRCCOPY = 0x00CC0020;

        private readonly int _srcX;
        private readonly int _srcY;
        private readonly int _srcW;
        private readonly int _srcH;
        private readonly IntPtr _screenDc;
        private readonly IntPtr _memoryDc;
        private readonly IntPtr _bitmap;
        private readonly ImageCodecInfo _jpeg;
        private byte[] _raw = new byte[0];

        public ScreenCapture()
        {
            _srcW = GetSystemMetrics(78); // SM_CXVIRTUALSCREEN
            _srcH = GetSystemMetrics(79); // SM_CYVIRTUALSCREEN
            _srcX = GetSystemMetrics(76); // SM_XVIRTUALSCREEN
            _srcY = GetSystemMetrics(77); // SM_YVIRTUALSCREEN
            if (_srcW <= 0 || _srcH <= 0)
            {
                throw new InvalidOperationException("no interactive desktop to capture");
            }
            _screenDc = GetDC(IntPtr.Zero);
            _memoryDc = CreateCompatibleDC(_screenDc);
            _bitmap = CreateCompatibleBitmap(_screenDc, _srcW, _srcH);
            SelectObject(_memoryDc, _bitmap);
            _jpeg = FindJpegCodec();
        }

        private static ImageCodecInfo FindJpegCodec()
        {
            foreach (ImageCodecInfo codec in ImageCodecInfo.GetImageEncoders())
            {
                if (codec.FormatID == ImageFormat.Jpeg.Guid) return codec;
            }
            throw new InvalidOperationException("no JPEG encoder available");
        }

        public int SourceWidth { get { return _srcW; } }
        public int SourceHeight { get { return _srcH; } }

        /// <summary>Dimensions of the JPEG returned by the last CaptureFrame call.</summary>
        public int LastWidth { get; private set; }
        public int LastHeight { get; private set; }

        /// <summary>Captures the whole virtual desktop and returns a JPEG buffer.</summary>
        public byte[] CaptureFrame(int targetWidth, int quality)
        {
            BitBlt(_memoryDc, 0, 0, _srcW, _srcH, _screenDc, _srcX, _srcY, SRCCOPY);

            int width = targetWidth <= 0 || targetWidth > _srcW ? _srcW : targetWidth;
            int height = (int)Math.Round((double)width * _srcH / _srcW);
            if (height < 2) height = 2;
            if ((height & 1) != 0) height++;
            LastWidth = width;
            LastHeight = height;

            using (Bitmap full = Image.FromHbitmap(_bitmap))
            using (Bitmap scaled = new Bitmap(width, height, PixelFormat.Format24bppRgb))
            using (MemoryStream stream = new MemoryStream(Math.Max(16 * 1024, width * height / 8)))
            {
                using (Graphics g = Graphics.FromImage(scaled))
                {
                    g.CompositingMode = CompositingMode.SourceCopy;
                    g.InterpolationMode = width == _srcW
                        ? InterpolationMode.NearestNeighbor
                        : InterpolationMode.HighQualityBilinear;
                    g.PixelOffsetMode = PixelOffsetMode.HighSpeed;
                    g.DrawImage(full, new Rectangle(0, 0, width, height));
                }
                using (EncoderParameters parameters = new EncoderParameters(1))
                {
                    parameters.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality,
                                                               (long)Clamp(quality, 20, 90));
                    scaled.Save(stream, _jpeg, parameters);
                }
                _raw = stream.ToArray();
            }
            return _raw;
        }

        private static int Clamp(int value, int low, int high)
        {
            if (value < low) return low;
            if (value > high) return high;
            return value;
        }

        public void Dispose()
        {
            if (_bitmap != IntPtr.Zero) DeleteObject(_bitmap);
            if (_memoryDc != IntPtr.Zero) DeleteDC(_memoryDc);
            if (_screenDc != IntPtr.Zero) ReleaseDC(IntPtr.Zero, _screenDc);
        }
    }

    /// <summary>Pointer/wheel injection for the tablet-as-touchpad feature.</summary>
    public static class Pointer
    {
        [DllImport("user32.dll")]
        private static extern void mouse_event(uint flags, int dx, int dy, int data, UIntPtr extra);

        private const uint MOVE = 0x0001;
        private const uint LEFTDOWN = 0x0002;
        private const uint LEFTUP = 0x0004;
        private const uint RIGHTDOWN = 0x0008;
        private const uint RIGHTUP = 0x0010;
        private const uint WHEEL = 0x0800;
        private const uint ABSOLUTE = 0x8000;
        private const uint VIRTUALDESK = 0x4000;

        public static void Move(ushort nx, ushort ny)
        {
            mouse_event(MOVE | ABSOLUTE | VIRTUALDESK, nx, ny, 0, UIntPtr.Zero);
        }

        public static void Button(ushort nx, ushort ny, bool down, bool right)
        {
            mouse_event(MOVE | ABSOLUTE | VIRTUALDESK, nx, ny, 0, UIntPtr.Zero);
            uint flag = right ? (down ? RIGHTDOWN : RIGHTUP) : (down ? LEFTDOWN : LEFTUP);
            mouse_event(flag, 0, 0, 0, UIntPtr.Zero);
        }

        public static void Wheel(int delta)
        {
            mouse_event(WHEEL, 0, 0, delta, UIntPtr.Zero);
        }
    }
}
