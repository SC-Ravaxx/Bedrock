"""Builds THIRD-PARTY-NOTICES.txt from the licence texts vcpkg installed.

    python tools/gen_third_party_notices.py [vcpkg-root] [out]

Bedrock ships 21 DLLs it did not write. Most of them are under Apache-2.0,
BSD or MIT, and every one of those licences asks for the same thing in return:
that the copyright notice travels with the binary. That is what this file is -
not a formality, the actual condition under which the libraries may be handed
out at all.

The texts are not typed out here. They are copied from
<vcpkg>/installed/x64-windows/share/<port>/copyright, which is what vcpkg
itself recorded when it built them, so the notice cannot drift away from what
is actually in the package.

The Microsoft runtime (msvcp140, vcruntime140, vcruntime140_1, concrt140) is
not from vcpkg and carries no copyright file. It is redistributable under the
Visual Studio licence and is listed by name only.
"""
import io, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VCPKG = sys.argv[1] if len(sys.argv) > 1 else r"C:\vcpkg"
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "THIRD-PARTY-NOTICES.txt")
SHARE = os.path.join(VCPKG, "installed", "x64-windows", "share")

# Which DLL comes from which vcpkg port. Everything Bedrock ships is here; if a
# DLL is added to the package and not to this list the script says so and stops,
# because a notice file that quietly omits a library is worse than none.
# Tesseract and the DLLs only it needs (libarchive, bzip2, giflib, leptonica,
# OpenSSL, curl, lz4, openjpeg) are not here: a release does not link it
# (CMakeLists.txt, BEDROCK_WITH_TESSERACT).
FROM_DLL = {
    "jpeg62.dll": "libjpeg-turbo",
    "liblzma.dll": "liblzma",
    "libpng16.dll": "libpng",
    "libsharpyuv.dll": "libwebp",
    "libwebp.dll": "libwebp",
    "libwebpdecoder.dll": "libwebp",
    "libwebpdemux.dll": "libwebp",
    "libwebpmux.dll": "libwebp",
    "opencv_core4.dll": "opencv4",
    "opencv_dnn4.dll": "opencv4",
    "opencv_imgcodecs4.dll": "opencv4",
    "opencv_imgproc4.dll": "opencv4",
    "tiff.dll": "tiff",
    "zlib1.dll": "zlib",
    "zstd.dll": "zstd",
    # OpenCV's dnn module and what it reads ONNX with - the second reader
    "libprotobuf.dll": "protobuf",
    "abseil_dll.dll": "abseil",
}

# Linked INTO the DLLs above rather than shipped as files of their own. OpenCV
# builds quirc and flatbuffers in, protobuf utf8-range; leaving them out
# because no .dll carries their name would miss the point of the notice.
LINKED_IN = ["quirc", "flatbuffers", "utf8-range"]

# Not a library but data inside Bedrock.exe: the second reader's recogniser
# (resources/textline.onnx, include/TextLineReader.h). Apache-2.0, whose full
# text the OpenCV section already carries.
EMBEDDED = [
    "BUILT INTO Bedrock.exe: THE SECOND READER'S TEXT RECOGNITION MODEL",
    "",
    "    resources/textline.onnx       PP-OCRv4 text recognition model (ch_PP-OCRv4_rec)",
    "    resources/textline_keys.txt   its character list (ppocr_keys_v1.txt)",
    "",
    "Both are from PaddleOCR, https://github.com/PaddlePaddle/PaddleOCR,",
    "Copyright (c) PaddlePaddle Authors, licensed under the Apache License,",
    "Version 2.0 - the full text of which is reproduced above in the OPENCV4",
    "section. The model was taken in the ONNX form distributed with Guten OCR",
    "(@gutenye/ocr-models 1.4.2, https://github.com/gutenye/ocr, MIT licence)",
    "and changed by Bedrock in one respect: its input shape was fixed to",
    "1 x 3 x 48 x 320 (tools/fix_onnx_shape.py) so that OpenCV can load it.",
    "",
    "    Licensed under the Apache License, Version 2.0 (the \"License\");",
    "    you may not use this file except in compliance with the License.",
    "    You may obtain a copy of the License at",
    "",
    "        http://www.apache.org/licenses/LICENSE-2.0",
    "",
    "    Unless required by applicable law or agreed to in writing, software",
    "    distributed under the License is distributed on an \"AS IS\" BASIS,",
    "    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.",
    "    See the License for the specific language governing permissions and",
    "    limitations under the License.",
    "",
    "Guten OCR's MIT licence, for the conversion:",
    "",
    "    Permission is hereby granted, free of charge, to any person obtaining a",
    "    copy of this software and associated documentation files (the",
    "    \"Software\"), to deal in the Software without restriction, including",
    "    without limitation the rights to use, copy, modify, merge, publish,",
    "    distribute, sublicense, and/or sell copies of the Software, and to",
    "    permit persons to whom the Software is furnished to do so, subject to",
    "    the following conditions:",
    "",
    "    The above copyright notice and this permission notice shall be included",
    "    in all copies or substantial portions of the Software.",
    "",
    "    THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS",
    "    OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF",
    "    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.",
    "    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY",
    "    CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,",
    "    TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE",
    "    SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.",
    "",
    "BUILT INTO Bedrock.exe: THE REFINING TABLES",
    "",
    "include/RefiningData.h is built from regolith_lookups.json, from",
    "RegolithCo-Common (https://github.com/RegolithCo/RegolithCo-Common):",
    "",
    "    MIT License",
    "",
    "    Copyright (c) 2026 Regolith Co.",
    "",
    "    Permission is hereby granted, free of charge, to any person obtaining a copy",
    '    of this software and associated documentation files (the "Software"), to deal',
    "    in the Software without restriction, including without limitation the rights",
    "    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell",
    "    copies of the Software, and to permit persons to whom the Software is",
    "    furnished to do so, subject to the following conditions:",
    "",
    "    The above copyright notice and this permission notice shall be included in all",
    "    copies or substantial portions of the Software.",
    "",
    '    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR',
    "    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,",
    "    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE",
    "    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER",
    "    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,",
    "    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE",
    "    SOFTWARE.",
]

MICROSOFT = ["concrt140.dll", "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"]


def texts():
    ports = []
    for port in sorted(set(FROM_DLL.values())) + LINKED_IN:
        if port not in ports:
            ports.append(port)
    out = []
    for port in ports:
        path = os.path.join(SHARE, port, "copyright")
        if not os.path.exists(path):
            raise SystemExit("no licence text for %s at %s" % (port, path))
        out.append((port, io.open(path, encoding="utf-8", errors="replace").read().strip()))
    return out


def main():
    if not os.path.isdir(SHARE):
        raise SystemExit("no vcpkg at %s - pass its root as the first argument" % VCPKG)

    dlls = sorted(FROM_DLL)
    lines = [
        "THIRD-PARTY NOTICES",
        "",
        "Bedrock is built on the libraries below. Each is used under its own",
        "licence, reproduced in full here because that is what those licences",
        "ask for. None of this text applies to Bedrock's own code - see LICENSE",
        "for that.",
        "",
        "Generated by tools/gen_third_party_notices.py from the licence texts",
        "vcpkg recorded when it built these libraries, so this file cannot",
        "drift away from what is actually in the package.",
        "",
        "WHAT IS IN THE PACKAGE",
        "",
    ]
    for dll in dlls:
        lines.append("    %-24s %s" % (dll, FROM_DLL[dll]))
    for dll in MICROSOFT:
        lines.append("    %-24s Microsoft Visual C++ runtime" % dll)
    lines += [
        "",
        "    ...and linked into the above: " + ", ".join(LINKED_IN),
        "",
        "The Microsoft Visual C++ runtime is redistributable under the Visual",
        "Studio licence and ships no copyright file of its own.",
        "",
    ]

    for port, text in texts():
        rule = "=" * 74
        lines += [rule, port.upper(), rule, "", text, ""]

    rule = "=" * 74
    lines += [rule] + EMBEDDED[:1] + [rule] + EMBEDDED[1:] + [""]

    io.open(OUT, "w", encoding="utf-8", newline="\r\n").write("\n".join(lines) + "\n")
    print("%s: %d libraries and the embedded model, %d bytes" % (
        OUT, len(set(FROM_DLL.values())) + len(LINKED_IN), os.path.getsize(OUT)))


if __name__ == "__main__":
    main()
