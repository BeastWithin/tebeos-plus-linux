using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Text;
using tessnet2;

// Faithful reproduction of TEBEOS's captcha OCR pipeline (Form1.CapthaParser):
//   Resize(8x, HighQualityBicubic) -> SetGrayscale(150) -> RemoveNoise(180)
//   -> tessnet2 (Init "ocr"/"eng" numeric, whitelist 0123456789) -> DoOCR
// Dumps the full Word list so we can see where a trailing "1" comes from.
class Program
{
    static void Log(string s) { Console.WriteLine(s); }

    static Bitmap Resize(Bitmap originalImage, int w, int h)
    {
        int width = originalImage.Width;
        int height = originalImage.Height;
        double num = (double)w / (double)width;
        double num2 = (double)h / (double)height;
        double num3 = ((num < num2) ? num : num2);
        int height2 = Convert.ToInt32((double)height * num3);
        int width2 = Convert.ToInt32((double)width * num3);
        Bitmap bitmap = new Bitmap(width2, height2);
        Graphics graphics = Graphics.FromImage(bitmap);
        graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
        graphics.SmoothingMode = SmoothingMode.HighQuality;
        graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
        graphics.CompositingQuality = CompositingQuality.HighQuality;
        graphics.Clear(Color.Transparent);
        graphics.DrawImage(originalImage, 0, 0, width2, height2);
        graphics.Dispose();
        return bitmap;
    }

    static Bitmap SetGrayscale(Bitmap img)
    {
        Bitmap bitmap = (Bitmap)img.Clone();
        for (int i = 0; i < bitmap.Width; i++)
            for (int j = 0; j < bitmap.Height; j++)
            {
                Color pixel = bitmap.GetPixel(i, j);
                if (pixel.R >= 150 || pixel.G >= 150 || pixel.B >= 150)
                    bitmap.SetPixel(i, j, Color.FromArgb(255, 255, 255));
                else
                    bitmap.SetPixel(i, j, Color.FromArgb(0, 0, 0));
            }
        return (Bitmap)bitmap.Clone();
    }

    static Bitmap RemoveNoise(Bitmap bmap)
    {
        for (int i = 0; i < bmap.Width; i++)
            for (int j = 0; j < bmap.Height; j++)
            {
                Color pixel = bmap.GetPixel(i, j);
                if (pixel.R < 180 && pixel.G < 180 && pixel.B < 180)
                    bmap.SetPixel(i, j, Color.Black);
                else if (pixel.R > 180 && pixel.G > 180 && pixel.B > 180)
                    bmap.SetPixel(i, j, Color.White);
            }
        return bmap;
    }

    static string CleanSecurityCode(string securityCode)
    {
        StringBuilder stringBuilder = new StringBuilder();
        for (int i = 0; i < securityCode.Length; i++)
        {
            string value = securityCode.Substring(i, 1);
            if ("0123456789+".Contains(value)) stringBuilder.Append(value);
        }
        return stringBuilder.ToString();
    }

    [STAThread]
    static int Main(string[] args)
    {
        string imgPath = args.Length > 0 ? args[0] : @"C:\captcha.png";
        string outDir = args.Length > 1 ? args[1] : @"C:\";

        Log("== ocrprobe: " + imgPath);
        if (!File.Exists(imgPath)) { Log("missing input " + imgPath); return 2; }

        bool invert = args.Length > 2 && args[2] == "invert";
        Bitmap bitmap = new Bitmap(imgPath);
        Log("native=" + bitmap.Width + "x" + bitmap.Height + " fmt=" + bitmap.PixelFormat + (invert ? " [INVERT]" : ""));
        if (invert)
        {
            for (int x = 0; x < bitmap.Width; x++)
                for (int y = 0; y < bitmap.Height; y++)
                {
                    Color c = bitmap.GetPixel(x, y);
                    bitmap.SetPixel(x, y, Color.FromArgb(255 - c.R, 255 - c.G, 255 - c.B));
                }
        }

        Bitmap resized = Resize(bitmap, bitmap.Width * 8, bitmap.Height * 8);
        Log("resized=" + resized.Width + "x" + resized.Height);
        Bitmap grayscale = SetGrayscale(resized);
        Bitmap cleaned = RemoveNoise(grayscale);
        cleaned.SetResolution(480f, 480f);

        try { resized.Save(Path.Combine(outDir, "ocrprobe-resized.png"), ImageFormat.Png); } catch { }
        try { cleaned.Save(Path.Combine(outDir, "ocrprobe-cleaned.png"), ImageFormat.Png); } catch { }
        Log("saved intermediates to " + outDir);

        Tesseract tesseract = new Tesseract();
        try
        {
            tesseract.SetVariable("tessedit_char_whitelist", "0123456789");
            tesseract.Init("ocr", "eng", true);
            List<Word> list = tesseract.DoOCR(cleaned, Rectangle.Empty);

            Log("words=" + (list == null ? -1 : list.Count));
            if (list != null)
            {
                int wi = 0;
                foreach (Word k in list)
                {
                    Log(string.Format("  w[{0}] text=\"{1}\" conf={2:F1} box=({3},{4})-({5},{6}) blanks={7}",
                        wi++, k.Text, k.Confidence, k.Left, k.Top, k.Right, k.Bottom, k.Blanks));
                }
                List<string> source = (from k in list
                                       where !k.Text.ToString().Contains("~")
                                       select k.Text).ToList();
                string text = source.Aggregate("", (string current, string next) => current + next);
                Log("aggregate=\"" + text + "\"");
                Log("clean=\"" + (string.IsNullOrEmpty(text) ? "" : CleanSecurityCode(text)) + "\"");
            }
        }
        catch (Exception ex)
        {
            Log("OCR threw: " + ex.GetType().Name + ": " + ex.Message);
        }
        tesseract.Dispose();
        Log("== done");
        return 0;
    }
}
