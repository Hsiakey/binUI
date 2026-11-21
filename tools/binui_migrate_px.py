import io, os, re, glob

ROOT = r"D:\projects\binUI"

# 长度属性：无单位数值补 px（binUI 一直把无单位当 px，浏览器里非 0 无单位是非法值、
# 整条声明会被丢掉 —— 这就是"源码在浏览器里散架"的第 3 条根因）。
# line-height **不在**这里：CSS 的无单位 line-height 是"倍数"，补 px 会改变显示。
LENGTH_PROPS = {
    "width", "height", "min-width", "min-height", "max-width", "max-height",
    "left", "top", "right", "bottom", "margin", "margin-left", "margin-top",
    "margin-right", "margin-bottom", "padding", "padding-left", "padding-top",
    "padding-right", "padding-bottom", "border-radius", "font-size", "gap",
    "row-gap", "column-gap", "border-width", "outline-width", "letter-spacing",
    "text-indent", "min-inline-size", "max-inline-size",
}
NUM = re.compile(r"^[+-]?(\d+(\.\d*)?|\.\d+)$")
DECL = re.compile(r"([A-Za-z-]+)\s*:\s*([^;{}]+)")


def read(p):
    return io.open(p, encoding="utf-8").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)


def px_fix_val(v):
    s = v.strip()
    if not s:
        return v
    if "(" in s or "%" in s:
        return v
    if " " in s:                      # 简写：padding: 8 12 → 每个值单独补
        parts = s.split()
        return " ".join(px_fix_val(p) if NUM.match(p) else p for p in parts)
    return s + "px" if NUM.match(s) else v


def fix_decls(text):
    def repl(m):
        prop, val = m.group(1), m.group(2)
        if prop.lower() in LENGTH_PROPS:
            return prop + ": " + px_fix_val(val)
        return m.group(0)
    return DECL.sub(repl, text)


def fix_style_attr(value):
    out = []
    for d in value.split(";"):
        if ":" not in d:
            out.append(d)
            continue
        prop, val = d.split(":", 1)
        prop_s = prop.strip()
        # 自定义属性（--w 之类）与长度属性同样处理
        is_len = prop_s.lower() in LENGTH_PROPS or prop_s.startswith("--")
        if is_len:
            val = " " + px_fix_val(val) if False else px_fix_val(val)
        out.append(prop_s + ":" + val.strip() if val.strip() else prop_s)
    return ";".join(out)


TAG_RE = re.compile(r"<([A-Za-z][\w-]*)((?:\s[^<>]*?)?)(/?)>")
STYLE_BLOCK = re.compile(r"(?s)(<style[^>]*>)(.*?)(</style>)")


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


def convert(html):
    html = STYLE_BLOCK.sub(lambda m: m.group(1) + fix_decls(m.group(2)) + m.group(3), html)

    def fix_tag(m):
        name, raw, self_close = m.group(1), m.group(2), m.group(3)
        if "style=" not in raw:
            return m.group(0)
        pairs = split_attrs(raw)
        changed = False
        for kv in pairs:
            if kv[0].lower() == "style" and kv[1]:
                new = fix_style_attr(kv[1])
                if new != kv[1]:
                    kv[1] = new
                    changed = True
        if not changed:
            return m.group(0)
        return "<" + name + fmt(pairs) + (self_close or "") + ">"

    return TAG_RE.sub(fix_tag, html)


files = sorted(glob.glob(os.path.join(ROOT, "examples", "**", "*.html"), recursive=True) +
               glob.glob(os.path.join(ROOT, "components", "**", "*.html"), recursive=True))
n = 0
for p in files:
    t = read(p)
    a = convert(t)
    if a != t:
        write(p, a)
        n += 1
        print("补 px：", os.path.relpath(p, ROOT))
print("共 %d 个文件" % n)
