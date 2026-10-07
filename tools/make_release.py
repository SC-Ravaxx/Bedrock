"""Packs a release and writes the version file the updater reads.

    python tools/make_release.py [notes.txt] ["one line the app shows"]

Takes the version from include/resource.h, the exe from build-release/Release,
and the runtime DLLs from the newest package already in dist/ - topped up
from the exe's own build folder with whatever else its imports name. Writes

    dist/Bedrock_<date>_v<x.y.z>.zip   what people download
    dist/bedrock-latest.txt            what the app fetches

Then: upload the zip as a GitHub release asset tagged v<x.y.z>, edit the
url= line in bedrock-latest.txt to point at it, and put that file where
BEDROCK_UPDATE_URL says. Nothing else is needed - see README.md.

The package deliberately holds NO player data: the app keeps all of that in
%LOCALAPPDATA%\\Bedrock. It DOES hold LICENSE and THIRD-PARTY-NOTICES.txt,
which the libraries it ships require to travel with them.
"""
import datetime, hashlib, io, os, re, shutil, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# BEDROCK_DIST=<folder> packs somewhere else - a trial run beside a copy of
# the last package, which leaves dist/ and its bedrock-latest.txt untouched
DIST = os.environ.get("BEDROCK_DIST") or os.path.join(ROOT, "dist")
# The release build - configured without BEDROCK_DEV_TOOLS - or whatever
# BEDROCK_EXE names. build/ is the workshop and is refused below.
EXE = os.environ.get("BEDROCK_EXE") or os.path.join(ROOT, "build-release", "Release", "Bedrock.exe")

# --- what version is this? -------------------------------------------------
header = io.open(os.path.join(ROOT, "include", "resource.h"), encoding="utf-8").read()


def number(name):
    m = re.search(r"#define BEDROCK_VERSION_%s\s+(\d+)" % name, header)
    if not m:
        raise SystemExit("no BEDROCK_VERSION_%s in include/resource.h" % name)
    return m.group(1)


version = ".".join(number(n) for n in ("MAJOR", "MINOR", "PATCH"))
# ...and the fourth number where there is one: a package that replaces one
# of the same patch number, such as 2.0.2.1 after 2.0.2
if re.search(r"#define BEDROCK_VERSION_BUILD\s+(\d+)", header) and number("BUILD") != "0":
    version += "." + number("BUILD")
# Today, unless BEDROCK_STAMP says otherwise: for a package whose contents
# change before anybody has it, under the name the release page already
# links - a new date would leave the old file being handed out.
# The commit's time names the package and dates every file in the zip, so
# packing the same commit again gives the same zip
import subprocess as _sp0
_ct = _sp0.run(["git", "-C", ROOT, "log", "-1", "--format=%ct"], capture_output=True, text=True).stdout.strip()
COMMIT_TIME = int(_ct) if _ct.isdigit() else 0
_commit_day = (datetime.datetime.fromtimestamp(COMMIT_TIME, datetime.timezone.utc).date().isoformat()
               if COMMIT_TIME else datetime.date.today().isoformat())
stamp = os.environ.get("BEDROCK_STAMP") or _commit_day
name = "Bedrock_%s_v%s" % (stamp, version)
# Built in a staging folder, never straight into dist/: the last package is
# usually the one the app is running from, and Windows holds that folder open.
staging = os.path.join(DIST, ".staging")
out = os.path.join(staging, name)
zpath = os.path.join(DIST, name + ".zip")
if os.path.exists(zpath):
    raise SystemExit("%s is already there - look at it before anything is overwritten" % zpath)
if not os.path.exists(EXE):
    raise SystemExit("no %s - build the release configuration first (cmake -S . -B build-release; "
                     "cmake --build build-release --config Release)" % EXE)
# The update address the release was built with: BEDROCK_UPDATE_URL, else the
# release build's cache. A package without one would never hear of the next
# version, so it is refused.
UPDATE_URL = os.environ.get("BEDROCK_UPDATE_URL", "")
if not UPDATE_URL:
    cache = os.path.join(os.path.dirname(os.path.dirname(EXE)), "CMakeCache.txt")
    if os.path.exists(cache):
        m = re.search(r"^BEDROCK_UPDATE_URL:STRING=(.*)$", io.open(cache, encoding="utf-8").read(), re.M)
        UPDATE_URL = m.group(1).strip() if m else ""
if not UPDATE_URL.startswith("https://"):
    raise SystemExit("no update address - configure the release build with "
                     "-DBEDROCK_UPDATE_URL=https://.../bedrock-latest.txt")
if UPDATE_URL.encode() not in io.open(EXE, "rb").read():
    raise SystemExit("%s was not built with %s - configure it with -DBEDROCK_UPDATE_URL and build again"
                     % (EXE, UPDATE_URL))
# The commit: a release is packed from a clean checkout, and its exe names that
# exact commit - so anyone can build the same source and compare.
import subprocess as _sp
COMMIT = _sp.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
changes = _sp.run(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"],
                  capture_output=True, text=True).stdout.strip()
if not re.fullmatch(r"[0-9a-f]{40}", COMMIT):
    raise SystemExit("not a git checkout - a release is packed from one")
if changes:
    raise SystemExit("the checkout has changes that are not committed - commit them, build, then pack")
exe_bytes = io.open(EXE, "rb").read()
if (COMMIT.encode() + b"-dirty") in exe_bytes or COMMIT.encode() not in exe_bytes:
    raise SystemExit("%s was not built from commit %s as it stands - build it again" % (EXE, COMMIT[:12]))
# A developer build carries the whole workshop - every harvest and bench
# command and every switch - and must never be what people download
with io.open(EXE, "rb") as f:
    exe_bytes = f.read()
for tell in (b"--inkprobe", b"--labelboot", b"SCMINER_GLYPH_TRACE", b"--steadybench"):
    if tell in exe_bytes:
        raise SystemExit("%s is a developer build (it carries %s) - pack the release configuration" % (EXE, tell.decode()))
shutil.rmtree(out, ignore_errors=True)

# --- the runtime DLLs come from the newest package already made ------------
# The newest folder that HAS them, not simply the newest. A package unzipped
# into dist/ for testing nests one folder inside another, and the outer one
# holds no DLLs at all; taking it would pack the exe with nothing to run it.
def dll_count(folder):
    return sum(1 for f in os.listdir(folder) if f.lower().endswith(".dll"))


candidates = sorted((d for d in os.listdir(DIST)
                     if os.path.isdir(os.path.join(DIST, d)) and d.startswith("Bedrock_")
                     and dll_count(os.path.join(DIST, d)) > 0),
                    key=lambda d: os.path.getmtime(os.path.join(DIST, d)))

os.makedirs(os.path.join(out, "debug"))
dlls_from = None
if candidates:
    source = os.path.join(DIST, candidates[-1])
    for dll in sorted(os.listdir(source)):
        # not "name": that is the package's own name, and shadowing it put every
        # file in the zip under "zstd.dll/" instead of "Bedrock_<date>_v<x.y.z>/"
        if dll.lower().endswith(".dll"):
            shutil.copy2(os.path.join(source, dll), os.path.join(out, dll))
    dlls_from = candidates[-1]
else:
    # No unpacked package left - the newest zip that holds DLLs is as good a
    # source
    zips = sorted((z for z in os.listdir(DIST) if z.startswith("Bedrock_") and z.endswith(".zip")),
                  key=lambda z: os.path.getmtime(os.path.join(DIST, z)))
    for z in reversed(zips):
        with zipfile.ZipFile(os.path.join(DIST, z)) as archive:
            dll_members = [m for m in archive.namelist() if m.lower().endswith(".dll")]
            if not dll_members:
                continue
            for m in dll_members:
                with archive.open(m) as src, open(os.path.join(out, os.path.basename(m)), "wb") as dst:
                    shutil.copyfileobj(src, dst)
            dlls_from = z
            break
    if dlls_from is None:
        raise SystemExit("no earlier package in dist/ with runtime DLLs in it - folder or zip")
shutil.copy2(EXE, os.path.join(out, "Bedrock.exe"))
# The app token lies beside the exe, not in it (CMakeLists.txt): a release
# without it would report nothing to UEX
token_file = os.path.join(os.path.dirname(EXE), "uex_token.dat")
if not os.path.exists(token_file):
    raise SystemExit("no uex_token.dat beside %s - put the app token in uex_token.txt and run cmake again" % EXE)
shutil.copy2(token_file, os.path.join(out, "uex_token.dat"))


# --- every DLL the exe needs, all the way down ------------------------------
# The DLLs above are the last package's. A build that needs one more would
# pack without it and fail on every machine but this one. So the imports are
# read out of the exe and out of every DLL it pulls in, and whatever is
# missing is taken from the folder the exe was built in. Anything that is in
# neither and not part of Windows stops the build.
def pe_imports(path):
    """The DLL names a PE file imports, delay-loaded ones included."""
    import struct
    data = io.open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("%s is not a PE file" % path)
    sections, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    dirs = opt + (112 if struct.unpack_from("<H", data, opt)[0] == 0x20B else 96)
    table = [struct.unpack_from("<IIII", data, opt + opt_size + 40 * i + 8)[:4] for i in range(sections)]

    def at(rva):   # file offset of a relative virtual address
        for vsize, va, rsize, raw in table:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        return None

    def name_at(rva):
        o = at(rva)
        return data[o:data.index(b"\0", o)].decode("ascii", "replace") if o is not None else None

    names = []
    for entry, size, name_field in ((1, 20, 12), (13, 32, 4)):   # imports, delay imports
        rva = struct.unpack_from("<I", data, dirs + 8 * entry)[0]
        o = at(rva) if rva else None
        while o is not None:
            fields = data[o:o + size]
            if len(fields) < size or not any(fields):
                break
            n = name_at(struct.unpack_from("<I", data, o + name_field)[0])
            if n:
                names.append(n)
            o += size
    return names


build_dir = os.path.dirname(EXE)
system32 = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
in_package = {f.lower(): f for f in os.listdir(out)}
in_build = {f.lower(): f for f in os.listdir(build_dir) if f.lower().endswith(".dll")}
todo, seen, taken, missing = ["Bedrock.exe"], set(), [], []
while todo:
    for dll in pe_imports(os.path.join(out, todo.pop())):
        key = dll.lower()
        if key in seen:
            continue
        seen.add(key)
        if key in in_package:
            todo.append(in_package[key])
        elif key in in_build:
            shutil.copy2(os.path.join(build_dir, in_build[key]), os.path.join(out, in_build[key]))
            in_package[key] = in_build[key]
            taken.append(in_build[key])
            todo.append(in_build[key])
        elif not (key.startswith("api-ms-win-") or key.startswith("ext-ms-") or
                  os.path.exists(os.path.join(system32, dll))):
            missing.append(dll)
if missing:
    shutil.rmtree(staging, ignore_errors=True)
    raise SystemExit("the exe needs %s, which is neither in the last package, nor beside %s, nor part of "
                     "Windows" % (", ".join(sorted(missing)), EXE))
if taken:
    print("added   : %s  (needed by this build, taken from %s)" % (", ".join(sorted(taken)), build_dir))
unused = sorted(f for k, f in in_package.items() if k.endswith(".dll") and k not in seen)
# ...and what nothing imports any more stays behind: carried over from the
# last package, a DLL would otherwise travel on for ever once the exe stops
# needing it.
if unused:
    for gone in unused:   # not "name" - that is the package's own, see above
        os.remove(os.path.join(out, gone))
    print("dropped : %s  (nothing imports them any more)" % ", ".join(unused))

# Text files go into the package with Windows line ends, whatever the
# checkout has, so the package does not depend on it
def _copy_text(source_file, target_file):
    text = io.open(source_file, "rb").read().decode("utf-8").replace("\r\n", "\n")
    io.open(target_file, "wb").write(text.replace("\n", "\r\n").encode("utf-8"))


_copy_text(os.path.join(DIST, "README.txt"), os.path.join(out, "README.txt"))
# The licence and the third-party notices are not decoration: every Apache,
# BSD and MIT library in the package is used on condition that its notice
# travels with the binary. A package without them is one that may not be
# handed out at all, so their absence stops the build rather than being
# noticed by somebody else later.
for required in ("LICENSE", "THIRD-PARTY-NOTICES.txt"):
    source_file = os.path.join(ROOT, required)
    if not os.path.exists(source_file):
        raise SystemExit("no %s - run tools/gen_third_party_notices.py" % required)
    _copy_text(source_file, os.path.join(out, required))
_copy_text(os.path.join(DIST, "debug_README.txt"), os.path.join(out, "debug", "README.txt"))

notes_file = sys.argv[1] if len(sys.argv) > 1 else None
notes = io.open(notes_file, encoding="utf-8").read() if notes_file else (
    "Bedrock - Mining Companion for Star Citizen (hand-out build)\r\n"
    "Version %s, %s\r\n\r\n"
    "(no release notes were given to tools/make_release.py)\r\n" % (version, stamp))
notes = notes.rstrip("\r\n") + ("\r\n\r\nBuilt from commit %s\r\n"
                                    "https://github.com/SC-Ravaxx/Bedrock/tree/%s\r\n" % (COMMIT, COMMIT))
notes = notes.replace("\r\n", "\n").replace("\n", "\r\n")
io.open(os.path.join(out, "VERSION.txt"), "w", newline="").write(notes)

# --- does the package actually run? ---------------------------------------
# Not a substitute for starting it, but it catches a package built from a
# folder that had no DLLs to give.
packed_dlls = dll_count(out)
if packed_dlls < 15:
    shutil.rmtree(staging, ignore_errors=True)
    raise SystemExit("only %d DLLs in the package (expected around 21) - "
                     "the folder they were taken from was %s" % (packed_dlls, source))

# --- ...and does it do its work there? --------------------------------------
# The self-test, run from a copy of the package folder - the one place that is
# what a player gets: no tessdata, no tests/, no glyphs folder, nothing of the
# build tree beside the exe. A skipped check fails too: the scan panel checks
# skip themselves where their pictures are missing, and a green run would hide
# a rock view that never reads a rock on a fresh install. A copy, because
# whatever the run writes beside itself must not end up in the zip.
import subprocess
trial = os.path.join(staging, "_selftest")
shutil.rmtree(trial, ignore_errors=True)
shutil.copytree(out, trial)
try:
    run = subprocess.run([os.path.join(trial, "Bedrock.exe"), "--selftest"], cwd=trial,
                         capture_output=True, text=True, errors="replace", timeout=900)
    said = run.stdout + run.stderr
except Exception as problem:   # could not even start: a DLL short, most likely
    said = "could not run: %s" % problem
shutil.rmtree(trial, ignore_errors=True)
if "Self-test PASSED" not in said or "[skip]" in said:
    shutil.rmtree(staging, ignore_errors=True)
    lines = [l for l in said.splitlines() if "FAIL" in l or "[skip]" in l or "could not run" in l]
    raise SystemExit("the self-test does not pass from the package folder:\n  " +
                     "\n  ".join(lines[:20] or said.splitlines()[-8:]))
print("selftest: passed from the package folder (%d checks)" % said.count("[PASS]"))

# --- the zip, and its checksum --------------------------------------------
# What every file in the package is, so a build of the same commit can be
# compared file by file; and what it was built with.
def _sha(path):
    return hashlib.sha256(io.open(path, "rb").read()).hexdigest()


def _cache_value(key):
    cache = os.path.join(os.path.dirname(os.path.dirname(EXE)), "CMakeCache.txt")
    if os.path.exists(cache):
        m = re.search(r"^%s:[A-Z]+=(.*)$" % re.escape(key), io.open(cache, encoding="utf-8").read(), re.M)
        if m:
            return m.group(1).strip()
    return ""


def _compiler_version():
    build = os.path.dirname(os.path.dirname(EXE))
    for root, _, files in os.walk(os.path.join(build, "CMakeFiles")):
        if "CMakeCXXCompiler.cmake" in files:
            m = re.search(r'CMAKE_CXX_COMPILER_VERSION "([^"]+)"',
                          io.open(os.path.join(root, "CMakeCXXCompiler.cmake"), encoding="utf-8").read())
            if m:
                return m.group(1)
    return "?"


def _sdk_version():
    build = os.path.dirname(os.path.dirname(EXE))
    for f in os.listdir(build):
        if f.endswith(".vcxproj"):
            m = re.search(r"<WindowsTargetPlatformVersion>([^<]+)<", io.open(os.path.join(build, f), encoding="utf-8").read())
            if m:
                return m.group(1)
    return "?"


_vcpkg = re.search(r'"builtin-baseline"\s*:\s*"([0-9a-f]+)"',
                   io.open(os.path.join(ROOT, "vcpkg.json"), encoding="utf-8").read()).group(1)
buildinfo = [
    "Bedrock %s, commit %s" % (version, COMMIT),
    "",
    "Bedrock.exe was built with:",
    "  MSVC (cl) %s, Windows SDK %s" % (_compiler_version(), _sdk_version()),
    "  vcpkg at builtin-baseline %s (vcpkg.json), triplet x64-windows" % _vcpkg,
    "  cmake -DBEDROCK_UPDATE_URL=%s, Release" % UPDATE_URL,
    "",
    "Bedrock.exe sha256 %s" % _sha(os.path.join(out, "Bedrock.exe")),
    "",
    "The DLLs are vcpkg's builds of those libraries and Microsoft's runtime; their",
    "hashes are in SHA256SUMS.txt. How to build the same Bedrock.exe: README.md,",
    "\"Reproducing a release\", at https://github.com/SC-Ravaxx/Bedrock",
]
io.open(os.path.join(out, "BUILDINFO.txt"), "w", newline="\r\n").write("\n".join(buildinfo) + "\n")
listing = []
for root, _, files in os.walk(out):
    for f in files:
        full = os.path.join(root, f)
        rel = os.path.relpath(full, out).replace(os.sep, "/")
        if rel != "SHA256SUMS.txt":
            listing.append("%s  %s" % (_sha(full), rel))
io.open(os.path.join(out, "SHA256SUMS.txt"), "w", newline="\n").write("\n".join(sorted(listing, key=lambda l: l[66:])) + "\n")

ZIP_TIME = datetime.datetime.fromtimestamp(max(COMMIT_TIME, 315532800), datetime.timezone.utc).timetuple()[:6]
entries = []
for root, _, files in os.walk(out):
    for f in files:
        full = os.path.join(root, f)
        entries.append(((name + "/" + os.path.relpath(full, out).replace(os.sep, "/")), full))
with zipfile.ZipFile(zpath, "w") as z:
    for arcname, full in sorted(entries):
        info = zipfile.ZipInfo(arcname, date_time=ZIP_TIME)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.create_system = 0
        info.external_attr = 0x20
        z.writestr(info, io.open(full, "rb").read(), compresslevel=9)

digest = hashlib.sha256(io.open(zpath, "rb").read()).hexdigest()
# The zip lives beside the version file, so its address follows from the one
# the build was made with - no hand-editing, and no chance of the two drifting.
base = UPDATE_URL.rsplit("/", 1)[0] + "/"
line = sys.argv[2] if len(sys.argv) > 2 else ""
# The licence the package carries, so the copy on the website can be checked
# against it rather than trusted: the file inside the package is the one that
# counts - it travels with the binary and cannot be corrected afterwards for
# anybody who already downloaded it. Unknown keys are ignored by every version
# of the updater (Updater.cpp reads four and drops the rest), so this is safe
# to add.
licence_sha = hashlib.sha256(io.open(os.path.join(ROOT, "LICENSE"), "rb").read()).hexdigest()
manifest = [
    "# What Bedrock fetches to find out whether it is out of date.",
    "# https only, and it unpacks nothing whose SHA-256 does not match.",
    "version=" + version,
    "url=" + base + os.path.basename(zpath),
    "sha256=" + digest,
    "notes=" + line,
    "licence=" + licence_sha,
    "commit=" + COMMIT,
]
# The Linux package, when it lies in dist/ beside the zip: the Linux build
# reads url_linux= and sha256_linux= and passes over the zip's lines, the
# Windows build the other way round (Updater.h). Made by
# tools/linux/build_package.sh in the jammy root and copied here; a release
# without one leaves the Linux build saying there is nothing for it.
appimage = os.path.join(DIST, "Bedrock-%s-x86_64.AppImage" % version)
if os.path.exists(appimage):
    linux_sha = hashlib.sha256(io.open(appimage, "rb").read()).hexdigest()
    manifest += ["url_linux=" + base + os.path.basename(appimage), "sha256_linux=" + linux_sha]
    print("linux   : %s  (%.1f MB)  sha256 %s" % (appimage, os.path.getsize(appimage) / 1048576.0, linux_sha))
else:
    print("linux   : no %s in dist/ - the version file names no Linux package" % os.path.basename(appimage))
io.open(os.path.join(DIST, "bedrock-latest.txt"), "w", newline=chr(10)).write(
    chr(10).join(manifest) + chr(10))

shutil.rmtree(staging, ignore_errors=True)
print("packed  : %s  (%d DLLs from %s)" % (name, packed_dlls, dlls_from))
print("zip     : %s  (%.1f MB)" % (zpath, os.path.getsize(zpath) / 1048576.0))
print("sha256  : %s" % digest)
print("version file: %s  <- upload both to the same folder" %
      os.path.join(DIST, "bedrock-latest.txt"))
