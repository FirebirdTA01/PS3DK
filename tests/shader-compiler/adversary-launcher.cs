// t_f078965f: a native entry point for Windows Python's subprocess.run.
// Compiled into the test's temporary directory by Windows PowerShell Add-Type.
// A .cmd cannot preserve argv: cmd parses &, | and % before its body starts.
using System;
using System.Diagnostics;
using System.IO;
using System.Text;

class AdversaryLauncher
{
    // Quote for the Windows process argument parser, never for cmd.exe.
    static string Quote(string value)
    {
        var result = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in value)
        {
            if (c == '\\') { ++slashes; continue; }
            result.Append('\\', c == '"' ? slashes * 2 + 1 : slashes);
            result.Append(c);
            slashes = 0;
        }
        result.Append('\\', slashes * 2);
        return result.Append('"').ToString();
    }

    static int Main(string[] args)
    {
        try
        {
            string exe = System.Reflection.Assembly.GetExecutingAssembly().Location;
            string[] config = File.ReadAllLines(exe + ".paths");
            if (config.Length != 2 || !Path.IsPathRooted(config[0]) ||
                !Path.IsPathRooted(config[1]) ||
                !File.Exists(config[0]) || !File.Exists(config[1]))
                throw new InvalidOperationException("expected explicit Bash and body paths");
            var command = new StringBuilder(Quote(config[1]));
            foreach (string arg in args) command.Append(' ').Append(Quote(arg));
            var start = new ProcessStartInfo(config[0], command.ToString());
            start.UseShellExecute = false;
            // Bash callers already converted argv on entry to this .exe;
            // Python callers supplied native argv directly. Do not convert
            // either a second time when the body invokes the native compiler.
            start.EnvironmentVariables["MSYS2_ARG_CONV_EXCL"] = "*";
            using (Process child = Process.Start(start))
            {
                child.WaitForExit();
                // Only SIGABRT is injected by this body. MSYS Bash exposes
                // that death to a native parent as 6 << 8;
                // its shell parent presents that same death as 128 + 6.
                // Python guards must see the shell's existing 134 contract.
                return child.ExitCode == (6 << 8) ? 134 : child.ExitCode;
            }
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("adversary-launcher: " + error.Message);
            return 125;
        }
    }
}
