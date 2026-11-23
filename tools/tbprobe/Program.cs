using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

[ComImport, Guid("3050F669-98B5-11CF-BB82-00AA00BDCE0B"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IHTMLElementRender
{
    void DrawToDC(IntPtr hdc);
    void SetDocumentPrinter([MarshalAs(UnmanagedType.BStr)] string printer, IntPtr hdc);
}

class Probe : Form
{
    private readonly WebBrowser wb = new WebBrowser();
    private readonly string url;
    private readonly string dir = @"C:\";
    private int completed, navs;

    private void Log(string s)
    {
        try { File.AppendAllText(Path.Combine(dir, "tbprobe.log"),
              DateTime.Now.ToString("HH:mm:ss.fff") + "  " + s + "\r\n"); } catch { }
    }

    public Probe(string u)
    {
        url = u;
        Text = "tbprobe"; Width = 1000; Height = 700;
        wb.Dock = DockStyle.Fill; wb.ScriptErrorsSuppressed = true;
        Controls.Add(wb);
        wb.Navigated += (s, e) => { navs++; Log("Navigated #" + navs + " url=" + e.Url); };
        wb.ProgressChanged += (s, e) => { if (e.CurrentProgress >= e.MaximumProgress) Log("ProgressChanged COMPLETE (" + e.CurrentProgress + "/" + e.MaximumProgress + ")"); };
        wb.DocumentCompleted += Wb_DocumentCompleted;
        Shown += (s, e) => { Log("== probe start, navigating to " + url); wb.Navigate(url); };
        var t = new Timer { Interval = 45000 };
        t.Tick += (s, e) => { t.Stop(); Log("== timeout, closing"); Close(); };
        t.Start();
    }

    private void Wb_DocumentCompleted(object sender, WebBrowserDocumentCompletedEventArgs e)
    {
        completed++;
        string info = "DocumentCompleted #" + completed + " url=" + e.Url;
        try { info += " readyState=" + wb.ReadyState; } catch { }
        Log(info);
        try { Log("  Document.Url=" + (wb.Document != null && wb.Document.Url != null ? wb.Document.Url.ToString() : "<null>")); } catch (Exception ex) { Log("  Document.Url failed: " + ex.Message); }
        try { Log("  DocumentText.Length=" + (wb.DocumentText != null ? wb.DocumentText.Length : -1)); } catch (Exception ex) { Log("  DocumentText failed: " + ex.Message); }
        try
        {
            var fr = wb.Document.GetElementById("form1") ?? wb.Document.GetElementById("f") ?? wb.Document.GetElementById("form");
            Log("  form action=" + (fr != null ? Convert.ToString(fr.GetAttribute("action")) : "<no form1/f/form>"));
        }
        catch (Exception ex) { Log("  form lookup failed: " + ex.Message); }
        ReplayImmediate();

        var t = new Timer { Interval = 2500 };
        t.Tick += (s, e) => { t.Stop(); ReplayGetStatus(); InspectImages(); };
        t.Start();
    }


    // Replays what the vendor app's wbMedula_DocumentCompleted -> GetStatus() does,
    // step by step, so the failing call shows up even though the app swallows it.

    // The same calls the vendor app makes INSIDE its DocumentCompleted handler
    // (its first statement enumerates Document.Forms).
    private void ReplayImmediate()
    {
        Log("== IMMEDIATE (inside DocumentCompleted) ==");
        try
        {
            var forms = wb.Document.Forms;
            Log("  Forms.Count = " + forms.Count);
            int i = 0;
            foreach (HtmlElement f in forms)
            {
                string act = null;
                try { act = Convert.ToString(f.GetAttribute("action")); } catch (Exception ex) { act = "<threw " + ex.GetType().Name + ">"; }
                Log("    form[" + i++ + "] action=" + act);
            }
        }
        catch (Exception ex) { Log("  ! Document.Forms threw: " + ex.GetType().Name + ": " + ex.Message); }
        try
        {
            Log("  Document.Body = " + (wb.Document.Body == null ? "NULL" : "present"));
            if (wb.Document.Body != null) Log("    InnerHtml.Length = " + wb.Document.Body.InnerHtml.Length);
        }
        catch (Exception ex) { Log("  ! Document.Body threw: " + ex.GetType().Name + ": " + ex.Message); }
        try { Log("  DocumentText.Length = " + wb.DocumentText.Length); }
        catch (Exception ex) { Log("  ! DocumentText threw: " + ex.GetType().Name + ": " + ex.Message); }
    }

    private void ReplayGetStatus()
    {
        const string baseUrl = "https://medeczane.sgk.gov.tr/eczane";
        Log("== replay of the app's GetStatus() sequence ==");

        try
        {
            var forms = wb.Document.Forms;
            Log("  Document.Forms.Count=" + forms.Count);
            foreach (HtmlElement f in forms)
            {
                string act = null, id = null;
                try { act = Convert.ToString(f.GetAttribute("action")); } catch { }
                try { id = Convert.ToString(f.GetAttribute("id")); } catch { }
                Log("    form id=" + id + " action=" + act);
            }
        }
        catch (Exception ex) { Log("  ! Document.Forms threw: " + ex.GetType().Name + ": " + ex.Message); }

        foreach (var id in new[] { "form1", "f", "form" })
        {
            try
            {
                var el = wb.Document.GetElementById(id);
                string act = el == null ? "<null>" : Convert.ToString(el.GetAttribute("action"));
                Log("  GetElementById(\"" + id + "\") -> " + (el == null ? "null" : "found") + " action=" + act);
            }
            catch (Exception ex) { Log("  ! GetElementById(\"" + id + "\") threw: " + ex.GetType().Name + ": " + ex.Message); }
        }

        try
        {
            var body = wb.Document.Body;
            Log("  Document.Body is " + (body == null ? "NULL" : "present"));
            if (body != null) Log("    Body.InnerHtml contains 'Uygulamaya erişim yetkiniz bulunmamaktadır.' = " +
                                  body.InnerHtml.Contains("Uygulamaya erişim yetkiniz bulunmamaktadır."));
        }
        catch (Exception ex) { Log("  ! Document.Body.InnerHtml threw: " + ex.GetType().Name + ": " + ex.Message); }

        try
        {
            string dt = wb.DocumentText;
            Log("  DocumentText length=" + dt.Length);
            Log("    contains 'Sosyal'            = " + dt.Contains("Sosyal"));
            Log("    contains 'Yeniden Giri&#351; Yap&#305;n&#305;z.' = " + dt.Contains("Yeniden Giri&#351; Yap&#305;n&#305;z."));
            Log("    contains 'Sistem hatas\ufffd'  = " + dt.Contains("Sistem hatas\ufffd"));
            Log("    contains 'Kullan\ufffdc\ufffd ad\ufffd veya \ufffdifre hatal\ufffd' = " +
                dt.Contains("Kullan\ufffdc\ufffd ad\ufffd veya \ufffdifre hatal\ufffd"));
            Log("    contains sayi-servlet url    = " + dt.Contains("SayiUretenImageYeniServlet"));
        }
        catch (Exception ex) { Log("  ! DocumentText threw: " + ex.GetType().Name + ": " + ex.Message); }

        try
        {
            string du = wb.Document.Url == null ? "<null>" : wb.Document.Url.ToString();
            Log("  Document.Url = " + du);
            Log("  matches (base + '/PostLogoutRedirectServlet') = " + (du == baseUrl + "/PostLogoutRedirectServlet"));
            Log("  matches (base + '/faces/login.jsp')           = " + (du == baseUrl + "/faces/login.jsp"));
        }
        catch (Exception ex) { Log("  ! Document.Url threw: " + ex.GetType().Name + ": " + ex.Message); }

        foreach (var id in new[] { "j_id_jsp_2072829783_2:text1", "j_id_jsp_2072829783_2:secret1",
                                   "j_id_jsp_2072829783_2:j_id_jsp_2072829783_5", "j_id_jsp_2072829783_2:button1" })
        {
            try
            {
                var el = wb.Document.GetElementById(id);
                Log("  login field \"" + id + "\" -> " + (el == null ? "NOT FOUND" : "found"));
            }
            catch (Exception ex) { Log("  ! GetElementById(\"" + id + "\") threw: " + ex.GetType().Name + ": " + ex.Message); }
        }
        Log("== replay done ==");
    }

    private void InspectImages()
    {
        Log("-- image inspection --");
        try
        {
            var imgs = wb.Document.GetElementsByTagName("img");
            Log("  images=" + imgs.Count);
            int pick = -1;
            for (int i = 0; i < imgs.Count; i++)
            {
                string src = "";
                try { src = Convert.ToString(imgs[i].GetAttribute("src")); } catch { }
                Log("   [" + i + "] src=" + src + " size=" + imgs[i].ClientRectangle.Width + "x" + imgs[i].ClientRectangle.Height);
                if (pick < 0 && src != null && src.IndexOf("SayiUreten", StringComparison.OrdinalIgnoreCase) >= 0) pick = i;
            }
            if (pick < 0 && imgs.Count > 0) pick = 0;
            if (pick < 0) { Log("  no <img> to render"); return; }
            var el = imgs[pick];
            int w = el.ClientRectangle.Width, h = el.ClientRectangle.Height;
            if (w <= 0 || h <= 0) { w = 60; h = 20; }
            Log("  rendering image #" + pick + " (" + w + "x" + h + ") via IHTMLElementRender");
            object dom = el.DomElement;
            Log("  DomElement type=" + (dom == null ? "<null>" : dom.GetType().FullName));
            IHTMLElementRender render = null;
            try { render = (IHTMLElementRender)dom; Log("  cast to IHTMLElementRender: OK"); }
            catch (Exception ex) { Log("  cast to IHTMLElementRender FAILED: " + ex.GetType().Name + ": " + ex.Message); }
            if (render == null) return;
            using (var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb))
            {
                using (var g = Graphics.FromImage(bmp))
                {
                    IntPtr hdc = g.GetHdc();
                    try { render.DrawToDC(hdc); Log("  DrawToDC returned"); }
                    catch (Exception ex) { Log("  DrawToDC threw: " + ex.GetType().Name + ": " + ex.Message); }
                    finally { g.ReleaseHdc(hdc); }
                }
                string png = Path.Combine(dir, "tbprobe-img" + pick + ".png");
                bmp.Save(png, ImageFormat.Png);
                int nonwhite = 0; long sum = 0;
                for (int y = 0; y < h; y += 1) for (int x = 0; x < w; x += 1)
                { Color c = bmp.GetPixel(x, y); sum += (c.R + c.G + c.B) / 3; if (c.R < 240 || c.G < 240 || c.B < 240) nonwhite++; }
                Log("  saved " + png + "  nonwhite=" + nonwhite + "/" + (w * h) + " mean=" + (sum / (w * h)));
            }
        }
        catch (Exception ex) { Log("  InspectImages failed: " + ex.GetType().Name + ": " + ex.Message); }
    }

    [STAThread]
    static void Main(string[] args)
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        string u = args.Length > 0 ? args[0] : "about:blank";
        Application.Run(new Probe(u));
    }
}
