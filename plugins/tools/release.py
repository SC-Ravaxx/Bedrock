"""Packs and signs every plugin; writes dist/ with index.json.

    python tools/release.py <Bedrock.exe>

Bedrock.exe: developer build. Signing: ssh-keygen -Y sign with the
"Bedrock plugin key" from 1Password's SSH agent (1Password asks once).
"""
import io, json, os, subprocess, sys

here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
exe = sys.argv[1]
pub = os.path.join(here, "tools", "bedrock-plugin-key.pub")
keygen = r"C:\Windows\System32\OpenSSH\ssh-keygen.exe"
if not os.path.exists(keygen):
    keygen = "ssh-keygen"
dist = os.path.join(here, "dist")
os.makedirs(dist, exist_ok=True)

plugins = []
for name in sorted(os.listdir(here)):
    folder = os.path.join(here, name)
    if not os.path.isfile(os.path.join(folder, "plugin.json")):
        continue
    info = json.load(io.open(os.path.join(folder, "plugin.json"), encoding="utf-8"))
    package = f"{info['id']}-{info['version']}.bplugin"
    signed = os.path.join(dist, info["id"] + ".signed")
    for old in (signed, signed + ".sig"):
        if os.path.exists(old):
            os.remove(old)
    subprocess.run([exe, "--plugin-manifest", folder, signed], check=True)
    subprocess.run([keygen, "-Y", "sign", "-f", pub, "-n", "bedrock-plugin", "-O", "hashalg=sha512", signed], check=True)
    done = subprocess.run([exe, "--pack-plugin", folder, os.path.join(dist, package), signed + ".sig"],
                          capture_output=True, text=True)
    print(done.stdout.strip())
    if done.returncode != 0:
        sys.exit(f"{name}: not packed")
    os.remove(signed)
    os.remove(signed + ".sig")
    plugins.append({
        "id": info["id"],
        "name": info.get("name", info["id"]),
        "version": info["version"],
        "bedrock": info.get("bedrock", ""),
        "description": info.get("description", ""),
        "url": package,
    })

with io.open(os.path.join(dist, "index.json"), "w", encoding="utf-8", newline="\n") as fh:
    json.dump({"plugins": plugins}, fh, ensure_ascii=False, indent=1)
    fh.write("\n")
print(f"{len(plugins)} plugins -> {dist}")
