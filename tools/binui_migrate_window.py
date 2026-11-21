import io, os, re, glob

ROOT = r"D:\projects\binUI"
PAGES = sorted(glob.glob(os.path.join(ROOT, "examples", "**", "main.html"), recursive=True)) + \
        [os.path.join(ROOT, "components", "preview.html")]

WINDOW_ATTR_MAP = [
    ("width", "data-width"), ("height", "data-height"), ("bg", "data-bg"),
    ("title", "data-title"), ("window_resize", "data-window-resize"),
    ("ui_scale", "data-ui-scale"), ("resizable", "data-resizable"),
    ("transparent", "data-transparent"), ("aspect", "data-aspect"),
]


def read(p):
    return io.open(p, encoding="utf-8").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)


def split_attrs(text):
    out = []
    p, n = 0, len(text)
    while p < n:
        while p < n and text[p] in " \t\r\n/":
            p += 1
        if p >= n:
            break
        start = p
        while p < n and text[p] not in " \t\r\n=/":
            p += 1
        key = text[start:p]
        while p < n and text[p] in " \t\r\n":
            p += 1
        if p < n and text[p] == "=":
            p += 1
            while p < n and text[p] in " \t\r\n":
                p += 1
            if p < n and text[p] in "\"'":
                q = text[p]
                p += 1
                vstart = p
                while p < n and text[p] != q:
                    p += 1
                val = text[vstart:p]
                p += 1
            else:
                vstart = p
                while p < n and text[p] not in " \t\r\n>":
                    p += 1
                val = text[vstart:p]
            out.append([key, val])
        elif key:
            out.append([key, None])
    return out


def attrs_to_str(pairs):
    buf = []
    for k, v in pairs:
        if v is None:
            buf.append(" " + k)
            continue
        q = '"' if '"' not in (v or "") else "'"
        buf.append(' %s=%s%s%s' % (k, q, v or "", q))
    return "".join(buf)


def migrate(path):
    html = read(path)
    if "data-binui-window" in html:
        return False
    m = re.search(r"<app_window\b([^>]*)>", html)
    if not m:
        return False
    win_attrs = split_attrs(m.group(1))
    rest = html[m.end():]

    head_inner = ""
    hm = re.search(r"(?s)<head>(.*?)</head>", rest)
    if hm:
        head_inner = hm.group(1)
        rest = rest[:hm.start()] + rest[hm.end():]

    # 去掉文档壳标签（<html …> / </html> / <body …> / </body> / </app_window>）
    rest = re.sub(r"<html\b[^>]*>", "", rest, count=1)
    rest = re.sub(r"</html>", "", rest)
    rest = re.sub(r"<body\b[^>]*>", "", rest, count=1)
    rest = re.sub(r"</body>", "", rest)
    rest = re.sub(r"</app_window>", "", rest)

    win = [["data-binui-window", None]]
    for k, v in win_attrs:
        if v is None:
            continue
        for src, target in WINDOW_ATTR_MAP:
            if k.lower() == src:
                win.append([target, v])
                break

    head = "<head>" + head_inner + "</head>" if head_inner.strip() else \
           '<head><meta charset="UTF-8"></head>'
    out = "<!DOCTYPE html>\n<html>\n" + head + "\n<body>\n"
    out += "<div" + attrs_to_str(win) + ">\n" + rest.strip("\n") + "\n</div>\n"
    out += "</body>\n</html>\n"
    write(path, out)
    return True


changed = 0
for p in PAGES:
    if migrate(p):
        changed += 1
        print("窗口壳迁移", os.path.relpath(p, ROOT))
print("共迁移 %d 个页面" % changed)
