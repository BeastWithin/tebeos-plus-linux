using System;
using System.IO;
using System.Windows.Forms;

// Focused diagnostic: navigate to the live SGK Medula login page, report the
// document mode, fill the username / password / captcha exactly like the vendor
// app (SetAttribute("Value", ...)), tick the KVKK checkbox, click the submit
// button, then observe whether the form actually submits.

class LoginProbe : Form
{
    private readonly WebBrowser wb = new WebBrowser();
    private readonly string dir = @"C:\";
    private int completed;
    private int navs;

    private void Log(string s)
    {
        try { File.AppendAllText(Path.Combine(dir, "loginprobe.log"),
              DateTime.Now.ToString("HH:mm:ss.fff") + "  " + s + "\r\n"); } catch { }
    }

    public LoginProbe()
    {
        Text = "loginprobe"; Width = 1000; Height = 700;
        wb.Dock = DockStyle.Fill; wb.ScriptErrorsSuppressed = true;
        Controls.Add(wb);
        wb.Navigated += (s, e) => { navs++; Log("Navigated #" + navs + " url=" + e.Url); };
        wb.DocumentCompleted += Wb_DocumentCompleted;
        Shown += (s, e) =>
        {
            Log("== start, navigating to https://medeczane.sgk.gov.tr/eczane/faces/login.jsp");
            wb.Navigate("https://medeczane.sgk.gov.tr/eczane/faces/login.jsp");
        };
        var t = new Timer { Interval = 40000 };
        t.Tick += (s, e) => { t.Stop(); Log("== timeout, closing"); Close(); };
        t.Start();
    }

    private void Wb_DocumentCompleted(object sender, WebBrowserDocumentCompletedEventArgs e)
    {
        completed++;
        Log("DocumentCompleted #" + completed + " url=" + e.Url);
        try { Log("  readyState=" + wb.ReadyState); } catch { }

        // First completion = login form.
        if (completed == 1)
        {
            var t2 = new Timer { Interval = 2500 };
            t2.Tick += (s, ev) => { t2.Stop(); DoLogin(); };
            t2.Start();
        }
        else if (completed >= 2)
        {
            // Post-submit page: report what happened.
            Log("  (post-submit) Document.Url=" + (wb.Document != null && wb.Document.Url != null ? wb.Document.Url.ToString() : "<null>"));
            try { Log("  body contains 'Giriş Yapıldı' = " + wb.Document.Body.InnerHtml.Contains("Giri\u015f Yap\u0131ld\u0131")); } catch { }
            try
            {
                string dt = wb.DocumentText;
                Log("  DocumentText.Length=" + dt.Length);
                Log("    contains 'Kullan\u0131c\u0131 ad\u0131 veya \u015fifre hatal\u0131' = " + dt.Contains("Kullan\u0131c\u0131 ad\u0131 veya \u015fifre hatal\u0131"));
                Log("    contains 'G\u00fcvenlik' = " + dt.Contains("G\u00fcvenlik"));
                Log("    contains 'index' = " + dt.Contains("index"));
            }
            catch { }
        }
    }

    private void DoLogin()
    {
        Log("== DoLogin ==");
        try
        {
            try
            {
                dynamic doc = wb.Document.DomDocument;
                try { Log("  document.documentMode = " + Convert.ToString(doc.documentMode)); } catch (Exception ex) { Log("  documentMode threw: " + ex.Message); }
                try { Log("  document.compatMode  = " + Convert.ToString(doc.compatMode)); } catch (Exception ex) { Log("  compatMode threw: " + ex.Message); }
            }
            catch (Exception ex) { Log("  DomDocument threw: " + ex.Message); }

            var text1 = wb.Document.GetElementById("j_id_jsp_2072829783_2:text1");
            var secret1 = wb.Document.GetElementById("j_id_jsp_2072829783_2:secret1");
            var cap = wb.Document.GetElementById("j_id_jsp_2072829783_2:j_id_jsp_2072829783_5");
            var kvkk = wb.Document.GetElementById("j_id_jsp_2072829783_2:kvkkTaahhut");
            var btn = wb.Document.GetElementById("j_id_jsp_2072829783_2:button1");

            Log("  fields found: text1=" + (text1 != null) + " secret1=" + (secret1 != null) + " cap=" + (cap != null) + " kvkk=" + (kvkk != null) + " btn=" + (btn != null));

            if (text1 != null) text1.SetAttribute("Value", "<ECZANE_KODU>");
            if (secret1 != null) secret1.SetAttribute("Value", "<MEDULA_PAROLA>");
            if (cap != null) cap.SetAttribute("Value", "123456");
            if (kvkk != null) kvkk.InvokeMember("Click");

            try
            {
                dynamic t = text1.DomElement;
                dynamic p = secret1.DomElement;
                dynamic c = cap.DomElement;
                dynamic k = kvkk.DomElement;
                Log("  readback text1.value   = " + Convert.ToString(t.value));
                Log("  readback secret1.value = " + Convert.ToString(p.value));
                Log("  readback cap.value     = " + Convert.ToString(c.value));
                Log("  readback kvkk.checked  = " + Convert.ToString(k.@checked));
            }
            catch (Exception ex) { Log("  readback threw: " + ex.GetType().Name + ": " + ex.Message); }

            // Submit.
            if (btn != null)
            {
                Log("  clicking button1 (submit) ...");
                try { btn.InvokeMember("Click"); Log("  InvokeMember(\"Click\") returned"); }
                catch (Exception ex) { Log("  button1 click threw: " + ex.GetType().Name + ": " + ex.Message); }
            }
            else { Log("  button1 not found, nothing submitted"); }

            // Don't close; wait and watch for navigation.
            var t3 = new Timer { Interval = 8000 };
            t3.Tick += (s, e) => { t3.Stop(); Log("  (after submit) url=" + (wb.Document != null && wb.Document.Url != null ? wb.Document.Url.ToString() : "<null>")); Log("== done, closing"); Close(); };
            t3.Start();
        }
        catch (Exception ex)
        {
            Log("! DoLogin failed: " + ex.GetType().Name + ": " + ex.Message);
            Close();
        }
    }

    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new LoginProbe());
    }
}
