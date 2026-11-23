using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
using tessnet2;

[ComImport, Guid("3050F669-98B5-11CF-BB82-00AA00BDCE0B"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IHTMLElementRender
{
    void DrawToDC(IntPtr hdc);
    void SetDocumentPrinter([MarshalAs(UnmanagedType.BStr)] string printer, IntPtr hdc);
}

// Captures many fresh captchas from the live login page (DrawToDC) and OCRs each
// with the vendor's exact tessnet2 pipeline. Tallies how often the result is 7
// digits (trailing "1") vs the correct 6.
class Probe : Form
{
    private readonly WebBrowser wb = new WebBrowser();
    private readonly string url = "https://medeczane.sgk.gov.tr/eczane/faces/login.jsp";
    private readonly string dir = @"C:\";
    private int round;
    private const int ROUNDS = 12;

    private void Log(string s)
    {
        try { File.AppendAllText(Path.Combine(dir, "ocrcount.log"),
              DateTime.Now.ToString("HH:mm:ss.fff") + "  " + s + "\r\n"); } catch { }
    }

    public Probe()
    {
        Text = "ocrcount"; Width = 1000; Height = 700;
        wb.Dock = DockStyle.Fill; wb.ScriptErrorsSuppressed = true;
        Controls.Add(wb);
        wb.DocumentCompleted += (s, e) =>
        {
            var t = new Timer { Interval = 900 };
            int r = round;
            t.Tick += (s2, ev) => { t.Stop(); DoRound(r); };
            t.Start();
        };
        Shown += (s, e) => { Log("== start " + ROUNDS + " rounds"); round = 1; wb.Navigate(url); };
    }

    private void DoRound(int r)
    {
        string ocr = CaptureAndOcr("cap" + r + ".png");
        Log(string.Format("round {0}: ocr=\"{1}\" len={2} {3}",
            r, ocr, ocr.Length, ocr.Length == 6 ? "OK6" : (ocr.Length == 7 ? "TRAILING1" : "OTHER")));
        if (r >= ROUNDS) { Log("== done"); Close(); return; }
        round = r + 1;
        wb.Navigate(url); // fresh captcha
    }

    private HtmlElement FindCaptcha()
    {
        try
        {
            var imgs = wb.Document.GetElementsByTagName("img");
            for (int i = 0; i < imgs.Count; i++)
            {
                string src = "";
                try { src = Convert.ToString(imgs[i].GetAttribute("src")); } catch { }
                if (src != null && src.IndexOf("SayiUreten", StringComparison.OrdinalIgnoreCase) >= 0)
                    return imgs[i];
            }
        }
        catch (Exception ex) { Log("FindCaptcha threw: " + ex.Message); }
        return null;
    }

    private Bitmap ResizeV(Bitmap o, int w, int h)
    {
        double n = (double)w / o.Width, n2 = (double)h / o.Height, n3 = Math.Min(n, n2);
        int w2 = Convert.ToInt32(o.Width * n3), h2 = Convert.ToInt32(o.Height * n3);
        var b = new Bitmap(w2, h2);
        var g = Graphics.FromImage(b);
        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
        g.SmoothingMode = SmoothingMode.HighQuality;
        g.PixelOffsetMode = PixelOffsetMode.HighQuality;
        g.CompositingQuality = CompositingQuality.HighQuality;
        g.Clear(Color.Transparent);
        g.DrawImage(o, 0, 0, w2, h2);
        g.Dispose();
        return b;
    }
    private Bitmap GrayV(Bitmap img)
    {
        var b = (Bitmap)img.Clone();
        for (int i = 0; i < b.Width; i++)
            for (int j = 0; j < b.Height; j++)
            { Color p = b.GetPixel(i, j); b.SetPixel(i, j, (p.R >= 150 || p.G >= 150 || p.B >= 150) ? Color.FromArgb(255, 255, 255) : Color.FromArgb(0, 0, 0)); }
        return (Bitmap)b.Clone();
    }
    private Bitmap NoiseV(Bitmap b)
    {
        for (int i = 0; i < b.Width; i++)
            for (int j = 0; j < b.Height; j++)
            { Color p = b.GetPixel(i, j);
              if (p.R < 180 && p.G < 180 && p.B < 180) b.SetPixel(i, j, Color.Black);
              else if (p.R > 180 && p.G > 180 && p.B > 180) b.SetPixel(i, j, Color.White); }
        return b;
    }
    private string CleanV(string s)
    {
        var sb = new StringBuilder();
        foreach (char c in s) if ("0123456789+".Contains(c)) sb.Append(c);
        return sb.ToString();
    }

    private string CaptureAndOcr(string file)
    {
        var el = FindCaptcha();
        if (el == null) return "NOCAPTCHA";
        int w = el.ClientRectangle.Width, h = el.ClientRectangle.Height;
        try
        {
            dynamic d = el.DomElement;
            Log(string.Format("  size: img.width/height={0}x{1} naturalWidth={2} clientrect={3}x{4}",
                d.width, d.height, d.naturalWidth, w, h));
        }
        catch (Exception ex) { Log("  size probe failed: " + ex.Message); }
        if (w <= 0 || h <= 0) { w = 140; h = 30; }
        try
        {
            var render = (IHTMLElementRender)el.DomElement;
            using (var raw = new Bitmap(w, h, PixelFormat.Format24bppRgb))
            {
                using (var g = Graphics.FromImage(raw))
                {
                    IntPtr hdc = g.GetHdc();
                    try { render.DrawToDC(hdc); } finally { g.ReleaseHdc(hdc); }
                }
                raw.Save(Path.Combine(dir, file), ImageFormat.Png);
                using (var resized = ResizeV(raw, raw.Width * 8, raw.Height * 8))
                using (var gray = GrayV(resized))
                using (var cleaned = NoiseV(gray))
                {
                    cleaned.SetResolution(480f, 480f);
                    var t = new Tesseract();
                    string result;
                    try
                    {
                        t.SetVariable("tessedit_char_whitelist", "0123456789");
                        t.Init("ocr", "eng", true);
                        var list = t.DoOCR(cleaned, Rectangle.Empty);
                        var src = (from k in list where !k.Text.Contains("~") select k.Text).ToList();
                        string text = src.Aggregate("", (c, nx) => c + nx);
                        result = string.IsNullOrEmpty(text) ? "" : CleanV(text);
                    }
                    catch (Exception ex) { result = "ERR:" + ex.Message; }
                    t.Dispose();
                    return result;
                }
            }
        }
        catch (Exception ex) { return "ERR:" + ex.GetType().Name + ":" + ex.Message; }
    }

    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new Probe());
    }
}
