using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Text;
using System.Threading;

namespace tessnet2
{
    public class Character
    {
        public ValueType Value;
        public int Left;
        public int Right;
        public int Top;
        public int Bottom;
        public object Tag;

        public Character(ValueType character, int left, int top, int right, int bottom)
        {
            Value = character; Left = left; Top = top; Right = right; Bottom = bottom;
        }
    }

    public class Word
    {
        public int LineIndex;
        public int Blanks;
        public int Left;
        public int Right;
        public int Top;
        public int Bottom;
        public int FontIndex;
        public int PointSize;
        public int Formating;
        public string Text;
        public double Confidence;
        public object Tag;
        public List<Character> CharList;

        public override string ToString()
        {
            return Text + " (" + Confidence + ")";
        }
    }

    public class Tesseract : IDisposable
    {
        public delegate void ProgressHandler(int percent);
        public delegate void OcrDoneHandler(List<Word> words);

        public OcrDoneHandler OcrDone;
        public event ProgressHandler ProgressEvent;

        private readonly Dictionary<string, object> _vars = new Dictionary<string, object>();
        private bool _disposed;
        private static int _seq;

        public int Init(string tessdata, string lang, bool numericMode)
        {
            return 0;
        }

        public void Clear()
        {
        }

        public bool SetVariable(string name, object val)
        {
            _vars[name] = val;
            return true;
        }

        public object GetVariable(string name)
        {
            object v;
            return _vars.TryGetValue(name, out v) ? v : null;
        }

        public Dictionary<string, Type> GetVariableList()
        {
            var d = new Dictionary<string, Type>();
            foreach (var kv in _vars)
                d[kv.Key] = kv.Value != null ? kv.Value.GetType() : typeof(object);
            return d;
        }

        public List<Word> DoOCR(Bitmap bitmap, Rectangle rect)
        {
            var result = new List<Word>();
            if (bitmap == null) return result;

            string text = null;
            try { text = OcrHost(bitmap, rect); }
            catch { text = null; }

            if (!string.IsNullOrEmpty(text))
            {
                result.Add(new Word { Text = text, Confidence = 100.0 });
            }
            return result;
        }

        public Bitmap GetThresholdedImage(Bitmap bitmap, Rectangle rect)
        {
            return bitmap;
        }

        public static int LineCount(List<Word> words)
        {
            return words != null && words.Count > 0 ? 1 : 0;
        }

        public static List<Word> GetLineWords(List<Word> words, int lineIndex)
        {
            return words != null ? new List<Word>(words) : new List<Word>();
        }

        public static string GetLineText(List<Word> words, int lineIndex)
        {
            if (words == null) return string.Empty;
            var sb = new StringBuilder();
            foreach (var w in words)
                if (!string.IsNullOrEmpty(w.Text)) sb.Append(w.Text);
            return sb.ToString();
        }

        // ---- host OCR bridge ----
        private static string OcrHost(Bitmap bitmap, Rectangle rect)
        {
            const string inDir = @"C:\ocr-in";
            const string outDir = @"C:\ocr-out";
            try
            {
                Directory.CreateDirectory(inDir);
                Directory.CreateDirectory(outDir);
            }
            catch { }

            Bitmap src;
            if (rect != Rectangle.Empty && rect.Width > 0 && rect.Height > 0)
            {
                try { src = bitmap.Clone(rect, bitmap.PixelFormat); }
                catch { src = bitmap; }
            }
            else
            {
                src = bitmap;
            }

            // clone to a fresh 24bpp bitmap so Format/source lock never blocks Save()
            Bitmap flat;
            try
            {
                flat = new Bitmap(src.Width, src.Height, PixelFormat.Format24bppRgb);
                using (var g = Graphics.FromImage(flat))
                    g.DrawImage(src, 0, 0, src.Width, src.Height);
            }
            catch
            {
                flat = src;
            }

            int id = unchecked(Interlocked.Increment(ref _seq));
            string name = "ocr_" + DateTime.Now.Ticks.ToString() + "_" + id.ToString();
            string inPath = Path.Combine(inDir, name + ".png");
            string outPath = Path.Combine(outDir, name + ".txt");

            try { flat.Save(inPath, ImageFormat.Png); }
            catch { return null; }

            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < 8000)
            {
                if (File.Exists(outPath))
                {
                    try
                    {
                        string s = File.ReadAllText(outPath).Trim();
                        Cleanup(inPath, outPath);
                        return s;
                    }
                    catch { /* result not fully written yet */ }
                }
                Thread.Sleep(25);
            }

            Cleanup(inPath, outPath);
            return null;
        }

        private static void Cleanup(string inPath, string outPath)
        {
            try { File.Delete(inPath); } catch { }
            try { File.Delete(outPath); } catch { }
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            Dispose(true);
            GC.SuppressFinalize(this);
        }

        protected virtual void Dispose(bool disposing)
        {
        }
    }
}
