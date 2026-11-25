"""Build the Windows community release from a clean Veyra build and the Qt SDK."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent.parent
NAME = "Veyra-2.2.1-PS5-NIS-win64"
BASE_HASH = "e09150d1594aa9416fa27457a42b4d50b5d814e05471216bef95b9303544386b"
BASE_DIR = "Veyra-2.2.1-NVIDIA-win64-portable/versions/2.2.1/"
LICENSES = ("GPL-2.0-only.txt", "GPL-3.0-only.txt", "LGPL-3.0-only.txt", "Qt-GPL-exception-1.0.txt")


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def archive(folder, target, prefix=""):
    with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in sorted(p for p in folder.rglob("*") if p.is_file()):
            name = prefix + path.relative_to(folder).as_posix()
            compression = zipfile.ZIP_STORED if path.suffix.lower() == ".zip" else zipfile.ZIP_DEFLATED
            z.write(path, name, compress_type=compression)


def deploy_qt(qt, stage, build, base):
    env = os.environ.copy()
    env["PATH"] = str(qt / "bin") + os.pathsep + env["PATH"]
    version = subprocess.check_output([qt / "bin/qmake.exe", "-query", "QT_VERSION"], text=True, env=env).strip()
    if version != "6.9.3":
        raise ValueError("This package uses Qt 6.9.3 MSVC x64")
    player = stage / "veyra_qml_ui.exe"
    shutil.copy2(build / player.name, player)
    subprocess.run([qt / "bin/windeployqt.exe", "--release", "--no-compiler-runtime", "--no-translations",
                    "--no-system-d3d-compiler", "--no-system-dxc-compiler", "--qmldir", SOURCE / "qml",
                    "--dir", stage, player], check=True, env=env)
    shutil.copytree(build / "qml/Veyra", stage / "qml/Veyra", dirs_exist_ok=True)
    translations = stage / "translations"
    translations.mkdir(exist_ok=True)
    for p in (qt / "translations").glob("*.qm"):
        if re.match(r"^qt(?:|base|declarative|multimedia|quickcontrols2?)_", p.name):
            shutil.copy2(p, translations / p.name)
    license_dir = stage / "licenses/qt"
    license_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(base) as z:
        for name in LICENSES:
            (license_dir / name).write_bytes(z.read(BASE_DIR + "licenses/qt/" + name))
    shutil.copytree(qt / "sbom", license_dir / "sbom")
    (stage / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=.\nQmlImports=qml\nTranslations=translations\n",
                                   encoding="ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("build", "qt", "base-zip", "out"):
        parser.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args()
    build, qt, base, out = (p.resolve() for p in (args.build, args.qt, args.base_zip, args.out))
    if digest(base) != BASE_HASH:
        raise ValueError("The upstream NVIDIA 2.2.1 portable archive has a different SHA-256")
    shaders = sorted((build / "shaders").rglob("*.dxil"))
    if len(shaders) != 108:
        raise ValueError("Expected all 108 compiled release shaders")
    out.mkdir(parents=True, exist_ok=True)
    release = out / (NAME + ".zip")
    with tempfile.TemporaryDirectory(prefix="community-package-", dir=out) as tmp:
        root = Path(tmp)
        stage, outer = root / "payload", root / "release"
        stage.mkdir()
        outer.mkdir()
        deploy_qt(qt, stage, build, base)
        for shader in shaders:
            target = stage / "shaders" / shader.relative_to(build / "shaders")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(shader, target)
        shutil.copytree(HERE / "defaults", stage / "community-defaults")
        for target in (stage, outer):
            shutil.copy2(SOURCE / "LICENSE", target / "LICENSE")
            shutil.copy2(SOURCE / "THIRD_PARTY_NOTICES.md", target / "THIRD_PARTY_NOTICES.md")
            shutil.copy2(HERE / "SOURCES.md", target / "SOURCES.md")
        shutil.copy2(SOURCE / "third_party/nis/licence.txt", stage / "licenses/NVIDIA_NIS_MIT.txt")
        for name in LICENSES:
            target = outer / "licenses/qt" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(stage / "licenses/qt" / name, target)
        shutil.copy2(HERE / "README.md", outer / "START HERE.md")
        payload = outer / "community-payload.zip"
        archive(stage, payload)
        constant = root / "CommunityPayload.cs"
        constant.write_text('internal static class CommunityPayload { public const string Sha256 = "' +
                            digest(payload) + '"; }\n', encoding="ascii")
        csc = Path(os.environ["WINDIR"]) / "Microsoft.NET/Framework64/v4.0.30319/csc.exe"
        subprocess.run([csc, "/nologo", "/target:winexe", "/optimize+", "/platform:x64",
                        "/reference:System.Windows.Forms.dll", "/reference:System.IO.Compression.dll",
                        "/reference:System.IO.Compression.FileSystem.dll", "/win32icon:" + str(SOURCE / "assets/veyra.ico"),
                        "/out:" + str(outer / "Veyra.exe"), HERE / "Launcher.cs", HERE / "PortableSetup.cs", constant],
                       check=True)
        output = root / release.name
        archive(outer, output, NAME + "/")
        output.replace(release)
    sums = digest(release) + "  " + release.name + "\n"
    (out / "SHA256SUMS.txt").write_bytes(sums.encode("ascii"))
    print(sums, end="")


if __name__ == "__main__":
    main()
