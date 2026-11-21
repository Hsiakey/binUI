import io, os, re, glob

ROOT = r"D:\projects\binUI"
LENGTH_PROPS = {
    "width", "height", "min-width", "min-height", "max-width", "max-height",
    "left", "top", "right", "bottom", "margin", "margin-left", "margin-top",
    "margin-right", "margin-bottom", "padding", "padding-left", "padding-top",
    "padding-right", "padding-bottom", "border-radius", "font-size", "gap",
    "row-gap", "column-gap", "line-height", "border-width",
}
MUST = re.compile(r"\{\{\s*([A-Za-z_][\w-]*)\s*(?:=\s*([^}]*))?\}\}")


def read(p):
    return io.open(p, encoding="utf-8").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)


def px_fix(v):
    s = v.strip()
    if not s:
        return v
    low = s.lower()
    if "(" in s or "%" in s or low in ("auto", "content", "max-content", "min-content",
                                        "fit-content", "inherit", "initial", "unset"):
        return v
    if re.fullmatch(r"[+-]?(\d+(\.\d*)?|\.\d+)", s):
        return s + "px"
    return v


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


def frag_name(m):
    return m.group(2) if m.group(2) is not None else ""


def convert_fragment(html):
    """组件片段：style 里的 {{}} -> var()；纯文本 {{}} -> 默认文本 + data-param。"""
    # 1) 标签属性：style 里的 mustache -> var()
    def fix_tag(m):
        name, raw, self_close = m.group(1), m.group(2), m.group(3)
        if "{{" not in raw:
            return m.group(0)
        pairs = split_attrs(raw)
        plain = False
        for kv in pairs:
            k = kv[0].lower()
            if kv[1] is None or "{{" not in kv[1]:
                continue
            if k == "style":
                def one(mm):
                    pname = mm.group(1)
                    dflt = mm.group(2) if mm.group(2) is not None else ""
                    d = px_fix(dflt) if dflt else ""
                    return "var(--%s, %s)" % (pname, d) if d else "var(--%s)" % pname
                kv[1] = MUST.sub(one, kv[1])
                plain = True
        # 其余属性（id / class）里的 {{}} 保留：那是属性插值，归 M5
        return "<" + name + fmt(pairs) + (self_close or "") + ">"

    html = TAG_RE.sub(fix_tag, html)

    # 2) 纯文本 mustache（文本节点只有占位，没有别的字）
    def fix_text(m):
        open_tag, text, close_tag = m.group(1), m.group(2), m.group(3)
        stripped = text.strip()
        mm = MUST.fullmatch(stripped)
        if not mm:
            return m.group(0)
        pname = mm.group(1)
        dflt = mm.group(2) if mm.group(2) is not None else ""
        tag_name = re.match(r"<([A-Za-z][\w-]*)", open_tag).group(1)
        if 'data-param=' in open_tag:
            return m.group(0)
        new_open = open_tag[:-1].rstrip() + ' data-param="%s">' % pname
        return new_open + dflt + close_tag

    html = re.sub(r"(<(?!/)[A-Za-z][^<>]*>)([^<>]*)(</[A-Za-z][\w-]*>)", fix_text, html)
    return html


def collect_named_params(html):
    """片段里用到哪些参数名（用于给宿主补 --name）。"""
    names = set()
    for m in MUST.finditer(html):
        names.add(m.group(1))
    for m in re.finditer(r'data-param="([^"]+)"', html):
        names.add(m.group(1))
    return names


def main():
    comps = sorted(glob.glob(os.path.join(ROOT, "examples", "**", "*.bum.html"), recursive=True) +
                   glob.glob(os.path.join(ROOT, "components", "**", "*.bum.html"), recursive=True))
    changed = 0
    frag_params = {}
    for c in comps:
        before = read(c)
        after = convert_fragment(before)
        frag_params[os.path.normpath(c).lower()] = collect_named_params(after)
        if after != before:
            write(c, after)
            changed += 1
            print("片段改写", os.path.relpath(c, ROOT))

    # 宿主：给每个 data-component 宿主补 --name: 值（片段里的 var() 从宿主继承）
    hosts = sorted(glob.glob(os.path.join(ROOT, "examples", "**", "*.html"), recursive=True) +
                   glob.glob(os.path.join(ROOT, "components", "**", "*.html"), recursive=True))
    host_changed = 0
    for h in hosts:
        html = read(h)
        if "data-component=" not in html:
            continue

        def fix_host(m):
            name, raw, self_close = m.group(1), m.group(2), m.group(3)
            pairs = split_attrs(raw)
            d = {kv[0].lower(): kv[1] for kv in pairs}
            src = None
            for kv in pairs:
                if kv[0].lower() == "data-component":
                    src = (kv[1] or "").strip()
            if not src:
                return m.group(0)
            params = {}
            for kv in pairs:
                k = kv[0].lower()
                if k.startswith("data-") and k != "data-component":
                    params[k[5:]] = kv[1] or ""
            if not params:
                return m.group(0)
            # style 里补 --name（已有的不覆盖）
            style = d.get("style", "") or ""
            adds = []
            for k, v in params.items():
                token = "--" + k + ":"
                if token in style:
                    continue
                adds.append("%s:%s" % ("--" + k, px_fix(v) if v else ""))
            if not adds:
                return m.group(0)
            new_style = ";".join([x for x in ([style] if style else []) + adds if x])
            for kv in pairs:
                if kv[0].lower() == "style":
                    kv[1] = new_style
                    break
            else:
                pairs.append(["style", new_style])
            return "<" + name + fmt(pairs) + (self_close or "") + ">"

        after = TAG_RE.sub(fix_host, html)
        if after != html:
            write(h, after)
            host_changed += 1
            print("宿主补 --var", os.path.relpath(h, ROOT))
    print("片段改写 %d 个，宿主补变量 %d 个" % (changed, host_changed))


if __name__ == "__main__":
    main()
