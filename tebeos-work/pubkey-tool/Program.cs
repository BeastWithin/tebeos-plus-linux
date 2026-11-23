using System;
using System.IO;
using System.Reflection;

class Program
{
    static void Main(string[] args)
    {
        foreach (var p in args)
        {
            try
            {
                var an = AssemblyName.GetAssemblyName(p);
                Console.WriteLine($"{Path.GetFileName(p)}: {an.FullName}");
                var pk = an.GetPublicKey();
                var tok = an.GetPublicKeyToken();
                Console.WriteLine($"  publicKey len={(pk != null ? pk.Length : 0)}  token={(tok != null ? BitConverter.ToString(tok).Replace("-", "").ToLower() : "(null)")}");
                if (pk != null && pk.Length > 0)
                {
                    var outPath = p + ".pubkey.bin";
                    File.WriteAllBytes(outPath, pk);
                    Console.WriteLine($"  wrote {outPath}");
                }
            }
            catch (Exception e)
            {
                Console.WriteLine($"{p}: ERROR {e.Message}");
            }
        }
    }
}
