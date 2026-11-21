import io, os, re, glob

ROOT = r"D:\projects\binUI"
MUST = re.compile(r"\{\{\s*([A-Za-z_][\w-]*)\s*(?:=\s*([^}]*))?\}\}")


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


def fmt(pairs):
    buf = []
    for k, v in pairs:
        if v is None:
            buf.append(" " + k)
        else:
            q = '"' if '"' not in v else "'"
            buf.append(' %s=%s%s%s' % (k, q, v, q))
    return "".join(buf)


TAG_RE = re.compile(r"<([A-Za-z][\w-]*)((?:\s[^<>]*?)?)(/?)>")

# 1) 叶组件片段里残留的顶层文本占位 -> 默认文本（编译期这些顶层文本本来就会被丢掉，
#    只为浏览器里能直接打开看清默认文案）
for p in glob.glob(os.path.join(ROOT, "examples", "**", "*.bum.html"), recursive=True) + \
         glob.glob(os.path.join(ROOT, "components", "**", "*.bum.html"), recursive=True):
    t = read(p)
    orig = t

    def repl_line(m):
        d = m.group(2) if m.group(2) is not None else ""
        return m.group(1) + d

    t = re.sub(r"(?m)^(\s*)\{\{\s*[A-Za-z_][\w-]*\s*(?:=\s*([^}]*))?\}\}\s*$", repl_line, t)
    if t != orig:
        write(p, t)
        print("顶层文本占位 -> 默认文本：", os.path.relpath(p, ROOT))

# 2) 宿主 style 里上一趟误加的自定义属性：data-param 被当成参数、值里还带占位
for h in glob.glob(os.path.join(ROOT, "examples", "**", "*.html"), recursive=True) + \
         glob.glob(os.path.join(ROOT, "components", "**", "*.html"), recursive=True):
    html = read(h)
    if "data-component=" not in html:
        continue

    def fix_tag(m):
        name, raw, self_close = m.group(1), m.group(2), m.group(3)
        if "data-component" not in raw:
            return m.group(0)
        pairs = split_attrs(raw)
        changed = False
        for kv in pairs:
            if kv[0].lower() != "style" or not kv[1]:
                continue
            decls = [d.strip() for d in kv[1].split(";") if d.strip()]
            keep = []
            for d in decls:
                if not d.startswith("--"):
                    keep.append(d)
                    continue
                prop, _, val = d.partition(":")
                pname = prop[2:].strip()
                # 参数名本身不是合法标识符来源（data-param 被误当参数）、或值仍带占位 -> 丢掉
                if "{{" in val or "}}" in val or pname in ("param",):
                    changed = True
                    continue
                keep.append(d)
            if changed:
                kv[1] = ";".join(keep)
        if not changed:
            return m.group(0)
        return "<" + name + fmt(pairs) + (self_close or "") + ">"

    after = TAG_RE.sub(fix_tag, html)
    if after != html:
        write(h, after)
        print("宿主 style 清理：", os.path.relpath(h, ROOT))
print("done")
