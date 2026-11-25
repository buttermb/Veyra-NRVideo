using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

internal static class PortableSetup
{
    private const string BaseName = "Veyra-2.2.1-NVIDIA-win64-portable.zip";
    private const string BaseUrl = "https://github.com/Likely7/Veyra-NRVideo/releases/download/v2.2.1/" + BaseName;
    private const string BaseHash = "e09150d1594aa9416fa27457a42b4d50b5d814e05471216bef95b9303544386b";
    private const string BasePrefix = "Veyra-2.2.1-NVIDIA-win64-portable/versions/2.2.1/";
    private const string Marker = ".community-release";
    private static readonly string[] Defaults = {
        "chain-session.v1.video-denoise", "presets.v1.video-denoise", "qml-preferences.v1.json"
    };
    private static readonly HashSet<string> UpstreamFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase) {
        "assets", "docs", "i18n", "licenses", "media", "runtime", "runtime_local", "virtualcam",
        "avcodec-63.dll", "avfilter-12.dll", "avformat-63.dll", "avutil-61.dll", "D3Dcompiler_47.dll",
        "dav1d.dll", "dolby_vision.dll", "dxcompiler.dll", "msvcp140.dll", "swresample-7.dll",
        "swscale-10.dll", "vcruntime140.dll", "vcruntime140_1.dll", "veyra_update_worker.exe",
        "release-runtime-manifest.json", "vfg-runtime-manifest.json"
    };

    [DllImport("kernel32.dll")]
    private static extern bool AllocConsole();

    private static string Hex(byte[] data) { return BitConverter.ToString(data).Replace("-", "").ToLowerInvariant(); }

    private static string ExtendedPath(string path)
    {
        string full = Path.GetFullPath(path);
        if (full.StartsWith(@"\\?\", StringComparison.Ordinal)) return full;
        if (full.StartsWith(@"\\", StringComparison.Ordinal)) return @"\\?\UNC\" + full.Substring(2);
        return @"\\?\" + full;
    }

    internal static string FolderId(string root)
    {
        using (var sha = SHA256.Create())
            return Hex(sha.ComputeHash(Encoding.UTF8.GetBytes(Path.GetFullPath(root).ToUpperInvariant())));
    }

    private static void CheckHash(string path, string expected)
    {
        using (var input = File.OpenRead(path))
        using (var sha = SHA256.Create())
            if (Hex(sha.ComputeHash(input)) != expected)
                throw new InvalidDataException("The SHA-256 check failed for " + Path.GetFileName(path) +
                    ". Download a fresh copy and run Veyra again.");
    }

    private static void DownloadBase(string destination)
    {
        ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
        var request = (HttpWebRequest)WebRequest.Create(BaseUrl);
        request.UserAgent = "Veyra-PS5-Community/2.2.1";
        using (var response = (HttpWebResponse)request.GetResponse())
        using (var input = response.GetResponseStream())
        using (var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        {
            var buffer = new byte[128 * 1024];
            long total = 0, nextReport = 0;
            int count;
            while ((count = input.Read(buffer, 0, buffer.Length)) != 0)
            {
                output.Write(buffer, 0, count);
                total += count;
                if (total >= nextReport)
                {
                    Console.WriteLine("Downloaded {0:N0} of 727 MiB", total / 1048576.0);
                    nextReport = total + 32L * 1024 * 1024;
                }
            }
        }
    }

    private static void Extract(string archive, string destination, bool upstream)
    {
        string prefix = Path.GetFullPath(destination).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        using (var zip = ZipFile.OpenRead(archive))
        {
            foreach (var entry in zip.Entries)
            {
                string name = entry.FullName.Replace('\\', '/');
                if (name.EndsWith("/", StringComparison.Ordinal)) continue;
                if (upstream)
                {
                    if (!name.StartsWith(BasePrefix, StringComparison.Ordinal)) continue;
                    name = name.Substring(BasePrefix.Length);
                    var slash = name.IndexOf('/');
                    if (!UpstreamFiles.Contains(slash < 0 ? name : name.Substring(0, slash))) continue;
                    if (name.StartsWith("licenses/qt/sbom/", StringComparison.OrdinalIgnoreCase)) continue;
                }
                if (name.Length == 0 || name.Contains(":") || Path.IsPathRooted(name))
                    throw new InvalidDataException("Invalid path in the release archive.");
                foreach (string segment in name.Split('/'))
                    if (segment.Length == 0 || segment == "." || segment == "..")
                        throw new InvalidDataException("Invalid directory in the release archive.");
                string path = Path.GetFullPath(Path.Combine(destination, name.Replace('/', Path.DirectorySeparatorChar)));
                if (!path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("A file is outside the application directory.");
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                using (var input = entry.Open())
                using (var output = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None))
                    input.CopyTo(output);
            }
        }
    }

    private static string NewStage(string root)
    {
        string directory = Path.Combine(root, ".veyra-setup-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        return directory;
    }

    private static void DiscardStage(string root, string stage)
    {
        string owner = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar);
        string path = Path.GetFullPath(stage);
        Guid id;
        string name = Path.GetFileName(path);
        if (String.Equals(Path.GetDirectoryName(path), owner, StringComparison.OrdinalIgnoreCase) &&
            name.StartsWith(".veyra-setup-", StringComparison.Ordinal) &&
            Guid.TryParseExact(name.Substring(".veyra-setup-".Length), "N", out id) && Directory.Exists(path))
            Directory.Delete(path, true);
    }

    private static void SeedProfile(string root, string app)
    {
        string profile = Path.Combine(root, "profile");
        if (Directory.Exists(profile)) return;
        string stage = NewStage(root);
        try
        {
            foreach (string file in Defaults)
                File.Copy(Path.Combine(app, "community-defaults", file), Path.Combine(stage, file));
            Directory.Move(stage, profile);
        }
        finally { DiscardStage(root, stage); }
    }

    internal static void Prepare(string root, bool unattended)
    {
        root = ExtendedPath(root);
        string app = Path.Combine(root, "app");
        string receipt = Path.Combine(app, Marker);
        if (Directory.Exists(app))
        {
            if (!File.Exists(receipt) || File.ReadAllText(receipt) != CommunityPayload.Sha256 ||
                !File.Exists(Path.Combine(app, "veyra_qml_ui.exe")))
                throw new IOException("Extract this release into a new, writable folder. This app folder belongs to another installation or is incomplete.");
            SeedProfile(root, app);
            return;
        }
        if (!unattended && AllocConsole())
        {
            Console.SetOut(new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true });
            Console.Title = "Setting up Veyra PS5 Capture";
        }
        Console.WriteLine("Preparing the portable Veyra player.");
        string payload = Path.Combine(root, "community-payload.zip");
        CheckHash(payload, CommunityPayload.Sha256);
        string stage = NewStage(root);
        string localDonor = Path.Combine(root, BaseName);
        string downloaded = null;
        bool verifiedDownload = false;
        try
        {
            string donor = localDonor;
            if (!File.Exists(donor))
            {
                Console.WriteLine("Downloading the official Veyra 2.2.1 NVIDIA runtime package.");
                donor = Path.Combine(stage, BaseName);
                DownloadBase(donor);
                downloaded = donor;
            }
            Console.WriteLine("Verifying the upstream download and unpacking the player.");
            CheckHash(donor, BaseHash);
            verifiedDownload = downloaded != null;
            string contents = Path.Combine(stage, "app");
            Directory.CreateDirectory(contents);
            Extract(donor, contents, true);
            Extract(payload, contents, false);
            foreach (string required in new[] { "veyra_qml_ui.exe", "Qt6Core.dll", "platforms/qwindows.dll",
                "shaders/VideoDenoise.dxil", "shaders/NisSdrFp16.dxil", "runtime/nvidia-vfg/NVVideoEffects.dll",
                "qml/Veyra/qmldir", "community-defaults/presets.v1.video-denoise" })
                if (!File.Exists(Path.Combine(contents, required.Replace('/', Path.DirectorySeparatorChar))))
                    throw new InvalidDataException("Missing release component " + required);
            File.WriteAllText(Path.Combine(contents, Marker), CommunityPayload.Sha256);
            Directory.Move(contents, app);
            SeedProfile(root, app);
        }
        catch
        {
            // Keep a complete, verified network download available for an extraction retry.
            if (verifiedDownload && !File.Exists(localDonor) && File.Exists(downloaded))
                File.Move(downloaded, localDonor);
            throw;
        }
        finally { DiscardStage(root, stage); }
    }
}
