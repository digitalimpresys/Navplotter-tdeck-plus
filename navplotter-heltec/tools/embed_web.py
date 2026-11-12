# Pre-build step: embeds web/index.html into the firmware as a gzip-compressed byte array (src/web_page.h).
# The weather, MOB and anchor modules (web/navplotter-meteo.js, web/navplotter-mob.js, web/navplotter-ancora.js, the same files as web/ of
# Navplotter-Linux) are put inside the page, after its own script, so the node serves a single file and
# web/index.html stays the same file as in the other NavPlotters.
import gzip, os

try:
    Import("env")  # provided by PlatformIO
    root = env["PROJECT_DIR"]
except NameError:
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

src = os.path.join(root, "web", "index.html")
dst = os.path.join(root, "src", "web_page.h")
MODULES = ("navplotter-meteo.js", "navplotter-mob.js", "navplotter-ancora.js")   # in this order: MOB after the weather module, anchor last

with open(src, "rb") as f:
    page = f.read()
inline = b""
for name in MODULES:
    path = os.path.join(root, "web", name)
    if os.path.exists(path):
        with open(path, "rb") as f:
            inline += b"<script>\n" + f.read() + b"\n</script>\n"
end = page.rfind(b"</body>")
if inline and end >= 0:
    page = page[:end] + inline + page[end:]
data = gzip.compress(page, 9, mtime=0)

lines = ["// Generated from web/index.html and the web/*.js modules by tools/embed_web.py - do not edit.",
         "#pragma once", "#include <Arduino.h>", "",
         "const size_t INDEX_HTML_GZ_LEN = %d;" % len(data),
         "const uint8_t INDEX_HTML_GZ[] PROGMEM = {"]
for i in range(0, len(data), 20):
    lines.append("  " + ",".join(str(b) for b in data[i:i + 20]) + ",")
lines.append("};")
text = "\n".join(lines) + "\n"

if not os.path.exists(dst) or open(dst).read() != text:
    with open(dst, "w") as f:
        f.write(text)
    print("embed_web: %s -> %d bytes gzip" % (os.path.basename(src), len(data)))
