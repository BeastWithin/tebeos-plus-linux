using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

[ComImport, Guid("3050F669-98B5-11CF-BB82-00AA00BDCE0B"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IHTMLElementRender
{
    void DrawToDC(IntPtr hdc);
    void SetDocumentPrinter([MarshalAs(UnmanagedType.BStr)] string printer, IntPtr hdc);
}

// Decisive test: does Gecko refresh the SayiUretenImageYeniServlet captcha on reload?
// Navigates twice, captures the captcha <img> both times, saves PNGs, and reports
// a pixel-level comparison. Identical images => stale captcha (caching bug).
class Probe : Form
{
    private readonly WebBrowser wb = new WebBrowser();
    private readonly string url = "https://medeczane.sgk.gov.tr/eczane/faces/login.jsp";
    private readonly string dir = @"C:\";
    private int round;

    private void Log(string s)
    {
        try { File.AppendAllText(Path.Combine(dir, "reloadprobe.log"),
              DateTime.Now.ToString("HH:mm:ss.fff") + "  " + s + "\r\n"); } catch { }
    }

    public Probe()
    {
        Text = "reloadprobe"; Width = 1000; Height = 700;
        wb.Dock = DockStyle.Fill; wb.ScriptErrorsSuppressed = true;
        Controls.Add(wb);
        wb.DocumentCompleted += OnDocComplete;
        Shown += (s, e) => { Log("== start, navigate round 1"); round = 1; wb.Navigate(url); };
    }

    private void OnDocComplete(object sender, WebBrowserDocumentCompletedEventArgs e)
    {
        Log("DocumentCompleted round=" + round + " url=" + e.Url);
        int r = round;
        // capture immediately (~like the app's 100ms) and then again after 3s,
        // to see whether the freshly-loaded captcha is still decoding at first.
        var tFast = new Timer { Interval = 120 };
        tFast.Tick += (s, ev) => { tFast.Stop(); CaptureCaptcha("captcha" + r + "_fast.png"); };
        tFast.Start();
        var tSlow = new Timer { Interval = 3000 };
        tSlow.Tick += (s, ev) => { tSlow.Stop(); CaptureCaptcha("captcha" + r + "_slow.png"); Step(r); };
        tSlow.Start();
    }

    private void Step(int r)
    {
        if (r == 1)
        {
            round = 2;
            Log("== reloading (round 2)");
            wb.Navigate(url);
        }
        else
        {
            Log("== comparing");
            Compare("captcha1_fast.png", "captcha1_slow.png", "round1 fast vs slow (decode race?)");
            Compare("captcha1_slow.png", "captcha2_slow.png", "round1 vs round2 (reload refresh?)");
            Log("== done");
            Close();
        }
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

    private void CaptureCaptcha(string file)
    {
        var el = FindCaptcha();
        if (el == null) { Log("no captcha <img> found"); return; }
        int w = el.ClientRectangle.Width, h = el.ClientRectangle.Height;
        if (w <= 0 || h <= 0) { w = 140; h = 30; }
        try
        {
            object dom = el.DomElement;
            var render = (IHTMLElementRender)dom;
            using (var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb))
            using (var g = Graphics.FromImage(bmp))
            {
                IntPtr hdc = g.GetHdc();
                try { render.DrawToDC(hdc); } finally { g.ReleaseHdc(hdc); }
                string p = Path.Combine(dir, file);
                bmp.Save(p, ImageFormat.Png);
                Log("saved " + p + " " + w + "x" + h);
            }
        }
        catch (Exception ex) { Log("capture " + file + " failed: " + ex.GetType().Name + ": " + ex.Message); }
    }

    private void Compare(string p1, string p2, string label)
    {
        p1 = Path.Combine(dir, p1); p2 = Path.Combine(dir, p2);
        Log("-- " + label);
        if (!File.Exists(p1) || !File.Exists(p2)) { Log("missing file(s)"); return; }
        using (var a = new Bitmap(p1))
        using (var b = new Bitmap(p2))
        {
            if (a.Width != b.Width || a.Height != b.Height) { Log("size differ " + a.Width + "x" + a.Height + " vs " + b.Width + "x" + b.Height); return; }
            long diff = 0, tot = a.Width * a.Height;
            long sum = 0;
            for (int y = 0; y < a.Height; y++)
                for (int x = 0; x < a.Width; x++)
                {
                    Color ca = a.GetPixel(x, y), cb = b.GetPixel(x, y);
                    if (ca.R != cb.R || ca.G != cb.G || ca.B != cb.B) diff++;
                    sum += Math.Abs(ca.R - cb.R) + Math.Abs(ca.G - cb.G) + Math.Abs(ca.B - cb.B);
                }
            Log(string.Format("  {0}/{1} pixels differ, absRGBsum={2}", diff, tot, sum));
            Log(diff == 0 ? "  RESULT: IDENTICAL" : "  RESULT: DIFFERENT");
        }
    }

    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new Probe());
    }
}
