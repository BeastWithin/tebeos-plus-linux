using System;
using System.IO;
using System.Windows.Forms;

// Verifies that Wine's HTMLElement::setAttribute("value", ...) now also sets the
// *value property* of an <input type=password> (mirrors IE; Gecko never reflects
// the value attribute into a password input's value).  The vendor app fills the
// SGK Medula password exactly this way: SetAttribute("Value", password).

class PwProbe : Form
{
    private readonly WebBrowser wb = new WebBrowser();
    private readonly string dir = @"C:\";

    private void Log(string s)
    {
        try { File.AppendAllText(Path.Combine(dir, "pwprobe.log"),
              DateTime.Now.ToString("HH:mm:ss.fff") + "  " + s + "\r\n"); } catch { }
    }

    public PwProbe()
    {
        Text = "pwprobe"; Width = 600; Height = 400;
        wb.Dock = DockStyle.Fill; wb.ScriptErrorsSuppressed = true;
        Controls.Add(wb);
        wb.DocumentCompleted += Wb_DocumentCompleted;
        Shown += (s, e) =>
        {
            Log("== start, navigating to file:///C:/pwtest.html");
            wb.Navigate("file:///C:/pwtest.html");
        };
        var t = new Timer { Interval = 15000 };
        t.Tick += (s, e) => { t.Stop(); Log("== timeout, closing"); Close(); };
        t.Start();
    }

    private void Wb_DocumentCompleted(object sender, WebBrowserDocumentCompletedEventArgs e)
    {
        Log("DocumentCompleted url=" + e.Url);
        try
        {
            var txt = wb.Document.GetElementById("text1");
            var pwd = wb.Document.GetElementById("secret1");

            // Fill the text input and the password input the same way the app does.
            txt.SetAttribute("Value", "hello");
            pwd.SetAttribute("Value", "çığ");

            // Read back the *property* value through the DOM dispatch (IDispatch).
            dynamic txtDom = txt.DomElement;
            dynamic pwdDom = pwd.DomElement;

            string txtVal = null, pwdVal = null;
            try { txtVal = Convert.ToString(txtDom.value); } catch (Exception ex) { Log("  text .value read threw: " + ex.GetType().Name + ": " + ex.Message); }
            try { pwdVal = Convert.ToString(pwdDom.value); } catch (Exception ex) { Log("  pwd .value read threw: " + ex.GetType().Name + ": " + ex.Message); }

            Log("  text .value             = " + (txtVal == null ? "<null>" : "\"" + txtVal + "\""));
            Log("  password .value         = " + (pwdVal == null ? "<null>" : "\"" + pwdVal + "\""));
            Log("  password .value equals [çığ] = " + (pwdVal == "çığ"));

            if (pwdVal == "çığ")
                Log("RESULT: PASS - password SetAttribute works");
            else
                Log("RESULT: FAIL - password SetAttribute did not set the value property");
        }
        catch (Exception ex)
        {
            Log("! failed: " + ex.GetType().Name + ": " + ex.Message);
        }
        Close();
    }

    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new PwProbe());
    }
}
