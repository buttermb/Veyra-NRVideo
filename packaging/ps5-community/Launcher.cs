using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("Veyra PS5 Capture Community Edition")]
[assembly: AssemblyDescription("Set up and open the portable PS5 capture player")]
[assembly: AssemblyVersion("1.0.0.0")]

internal static class VeyraLauncher
{
    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr window);

    [DllImport("user32.dll")]
    private static extern bool ShowWindowAsync(IntPtr window, int command);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr window);

    private static bool ActivatePlayer(string target)
    {
        int session = Process.GetCurrentProcess().SessionId;
        foreach (Process process in Process.GetProcessesByName("veyra_qml_ui"))
        {
            using (process)
            {
                try
                {
                    if (process.SessionId != session ||
                        !String.Equals(process.MainModule.FileName, target, StringComparison.OrdinalIgnoreCase))
                        continue;
                    IntPtr window = process.MainWindowHandle;
                    if (window == IntPtr.Zero)
                    {
                        process.WaitForInputIdle(2000);
                        process.Refresh();
                        window = process.MainWindowHandle;
                    }
                    if (window != IntPtr.Zero)
                    {
                        if (IsIconic(window)) ShowWindowAsync(window, 9); // SW_RESTORE
                        SetForegroundWindow(window);
                    }
                    return !process.HasExited;
                }
                catch (InvalidOperationException) { } // A closing window can disappear during the lookup.
                catch (Win32Exception) { } // Other sessions can restrict access to their process image.
            }
        }
        return false;
    }

    [STAThread]
    private static int Main(string[] args)
    {
        AppContext.SetSwitch("Switch.System.IO.UseLegacyPathHandling", false);
        AppContext.SetSwitch("Switch.System.IO.BlockLongPaths", false);
        bool prepareOnly = args.Length == 1 && args[0] == "--prepare-only";
        try
        {
            if (args.Length != 0 && !prepareOnly)
                throw new ArgumentException("Double-click Veyra.exe to play, or run Veyra.exe --prepare-only to set up the files.");
            string root = AppDomain.CurrentDomain.BaseDirectory;
            bool acquired;
            using (var gate = new Mutex(true, @"Local\Veyra.Community.Ps5." + PortableSetup.FolderId(root), out acquired))
            {
                if (!acquired)
                {
                    if (!prepareOnly) return 0;
                    if (!gate.WaitOne(TimeSpan.FromMinutes(15))) throw new TimeoutException("Setup is already running.");
                }
                PortableSetup.Prepare(root, prepareOnly);
                if (prepareOnly) return 0;
                string app = Path.Combine(root, "app");
                string player = Path.Combine(app, "veyra_qml_ui.exe");
                if (ActivatePlayer(player)) return 0;
                string profile = Path.Combine(root, "profile");
                bool hasCapture = File.Exists(Path.Combine(profile, "capture-preferences.v1"));
                var start = new ProcessStartInfo(player)
                {
                    UseShellExecute = false,
                    WorkingDirectory = app,
                    Arguments = "--data-dir \"" + profile + (hasCapture ? "\" --fullscreen" : "\" --page pro")
                };
                start.EnvironmentVariables["VEYRA_CAPTURE_REFRESH_PACED_FG"] = "1";
                start.EnvironmentVariables["VEYRA_CAPTURE_SHADOW_DENOISE"] = "1";
                using (Process child = Process.Start(start)) { return child == null ? 1 : 0; }
            }
        }
        catch (Exception error)
        {
            if (prepareOnly) Console.Error.WriteLine(error);
            else MessageBox.Show(error.Message, "Veyra PS5 Capture", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}
