#!/usr/bin/env python3
"""binui_migrate.py - 存量布局从"编译期方言"迁到合法 HTML（issue 15 的 M3 codemod）。

它只做一件有语义依据的事：**把老 `<use>` 的展开结果写回源码**。

老编译器处理 `<use src="x.bum.html" x= y= w= h= label= …/>` 时，产出的替换节点是
「组件**根元素**的标签 + 属性（参数已代入）+ 组件内部子元素」。所以：

  · 组件文件：摘掉根元素 → 组件变成"片段"（只留内部结构与内部选择器）；
  · 调用点：`<use …/>` → `<RootTag class=… style=… data-component="src"
     data-<参数>=…>默认文本</RootTag>`，根上的 class/style/文本按实例参数代入。

两边合起来与老展开逐字节等价 → 像素不变，但源码变成合法 HTML（宿主元素即组件根）。
组件内部的 `{{name}}`（`id="{{bid}}-body"` 这种逐实例唯一的）留给编译器按宿主
`data-<name>` 代入（`ExpandDataComponents` 里走与 `<use>` 同一套 `SubstituteParams`）。

用法:
    python tools/binui_migrate.py --check     # 只统计
    python tools/binui_migrate.py             # 就地改写
"""
import os
import re
import sys

# Windows 控制台默认 cp936，中文输出在管道里会变乱码；统一按 UTF-8 打印。
try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET_DIRS = ["examples", "components"]
SKIP_FILES = {"examples/valid-html/main.html", "examples/valid-html/button.html"}

LENGTH_PROPS = {
    "width", "height", "min-width", "min-height", "max-width", "max-height",
    "left", "top", "right", "bottom", "margin", "margin-left", "margin-top",
    "margin-right", "margin-bottom", "padding", "padding-left", "padding-top",
    "padding-right", "padding-bottom", "border-radius", "font-size", "gap",
    "row-gap", "column-gap", "line-height", "border-width",
}

MUSTACHE = re.compile(r"\{\{\s*([A-Za-z_][\w-]*)\s*(?:=\s*([^}]*))?\}\}")


# ------------------------------------------------------------------ 属性
def split_attrs(text):
    out = []
    p, n = 0, len(text)
    while p < n:
        while p < n and text[p] in " \t\r\n":
            p += 1
        if p >= n:
            break
        start = p
        while p < n and text[p] not in " \t\r\n=":
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
                while p < n and text[p] not in " \t\r\n>/":
                    p += 1
                val = text[vstart:p]
            out.append((key, val))
        else:
            out.append((key, None))
    return out


def fmt_attrs(pairs):
    buf = []
    for k, v in pairs:
        if v is None:
            buf.append(" " + k)
        else:
            q = '"' if '"' not in v else "'"
            buf.append(' %s=%s%s%s' % (k, q, v, q))
    return "".join(buf)


def px_fix(value):
    s = value.strip()
    if not s:
        return value
    low = s.lower()
    if "(" in s or "%" in s or low in ("auto", "content", "max-content", "min-content",
                                        "fit-content", "inherit", "initial", "unset"):
        return value
    if re.fullmatch(r"[+-]?(\d+(\.\d*)?|\.\d+)", s):
        return s + "px"
    return value


def fix_style(style):
    decls = []
    for d in style.split(";"):
        if ":" not in d:
            if d.strip():
                decls.append(d.strip())
            continue
        prop, val = d.split(":", 1)
        prop_s = prop.strip()
        val_s = val.strip()
        if prop_s.lower() in LENGTH_PROPS:
            val_s = px_fix(val_s)
        decls.append("%s:%s" % (prop_s, val_s) if val_s else prop_s)
    return ";".join(decls)


def style_has(style, prop):
    return re.search(r"(^|;)\s*%s\s*:" % re.escape(prop), style) is not None


def substitute(text, params):
    def repl(m):
        name = m.group(1)
        default = m.group(2) if m.group(2) is not None else ""
        for k, v in params.items():
            if k.lower() == name.lower() and v:
                return v
        return default
    return MUSTACHE.sub(repl, text)


# ------------------------------------------------------------------ 元素扫描
def find_root(html):
    """组件文件里唯一的根元素（跳过注释 / <style> / 文档壳）。"""
    i, n = 0, len(html)
    while i < n:
        lt = html.find("<", i)
        if lt < 0:
            return None
        if html.startswith("<!--", lt):
            e = html.find("-->", lt)
            if e < 0:
                return None
            i = e + 3
            continue
        if html.startswith("<!", lt) or html.startswith("<?", lt):
            e = html.find(">", lt)
            i = e + 1
            continue
        gt = html.find(">", lt)
        if gt < 0:
            return None
        raw = html[lt + 1:gt]
        name = raw.strip().lstrip("/").split()[0].lower() if raw.strip() else ""
        if name in ("html", "head", "body", "style", "script", "title", "meta", "link"):
            i = gt + 1
            continue
        if raw.startswith("/"):
            i = gt + 1
            continue
        attrs = split_attrs(raw.rstrip().rstrip("/"))
        tag = attrs[0][0].lower() if attrs else name
        pairs = attrs[1:]
        selfclose = raw.rstrip().endswith("/")
        if selfclose:
            return dict(tag=tag, attrs=pairs, inner="", start=lt, end=gt + 1)
        depth = 1
        j = gt + 1
        while j < n and depth:
            nlt = html.find("<", j)
            if nlt < 0:
                break
            if html.startswith("<!--", nlt):
                e = html.find("-->", nlt)
                j = (e + 3) if e >= 0 else n
                continue
            ngt = html.find(">", nlt)
            if ngt < 0:
                break
            inner = html[nlt + 1:ngt].strip()
            if inner.startswith("/"):
                if inner[1:].strip().split()[0].lower() == tag:
                    depth -= 1
                    if depth == 0:
                        return dict(tag=tag, attrs=pairs, inner=html[gt + 1:nlt],
                                    start=lt, end=ngt + 1)
            elif not inner.endswith("/"):
                nm = inner.split()[0].lower() if inner else ""
                if nm == tag:
                    depth += 1
            j = ngt + 1
        return None
    return None


def find_uses(html):
    out = []
    i, n = 0, len(html)
    while i < n:
        lt = html.find("<use", i)
        if lt < 0:
            break
        cstart = html.rfind("<!--", 0, lt)
        cend = html.rfind("-->", 0, lt)
        if cstart > cend:                     # 在注释里：跳过整段注释
            e = html.find("-->", lt)
            i = (e + 3) if e >= 0 else n
            continue
        gt = html.find(">", lt)
        if gt < 0:
            break
        raw = html[lt + 4:gt]
        selfclose = raw.rstrip().endswith("/")
        pairs = split_attrs(raw.rstrip().rstrip("/"))
        if selfclose:
            out.append(dict(attrs=pairs, inner="", start=lt, end=gt + 1))
            i = gt + 1
            continue
        close = html.find("</use>", gt)
        if close < 0:
            out.append(dict(attrs=pairs, inner="", start=lt, end=gt + 1))
            i = gt + 1
            continue
        out.append(dict(attrs=pairs, inner=html[gt + 1:close], start=lt, end=close + 6))
        i = close + 6
    return out


# ------------------------------------------------------------------ 迁移
def probe_component(host_dir, src):
    """按编译器的探测顺序找组件文件：.bum.html / .html / .bum / .bum.xml。"""
    base = os.path.normpath(os.path.join(host_dir, src))
    cands = []
    if base.lower().endswith((".html", ".htm", ".xml", ".bum")):
        cands.append(base)
        if base.lower().endswith(".bum"):
            cands += [base + ".html", base + ".xml"]
    else:
        cands += [base + ".bum.html", base + ".html", base + ".bum", base + ".bum.xml"]
    for c in cands:
        if os.path.isfile(c):
            return os.path.normpath(c)
    return None


def expand_uses(html, host_dir, roots):
    """把一个文件里的 <use> 全部换成宿主元素（返回新文本, 展开个数）。"""
    uses = find_uses(html)
    if not uses:
        return html, 0
    out = []
    cursor = 0
    count = 0
    for u in uses:
        params = {}
        src = ""
        for k, v in u["attrs"]:
            if k.lower() == "src":
                src = (v or "").strip()
                continue
            params[k] = v if v is not None else ""
        is_external = any(k.lower() == "external" or
                          (k.lower() == "mode" and (v or "").strip().lower() == "external")
                          for k, v in u["attrs"])
        comp = probe_component(host_dir, src) if src else None
        out.append(html[cursor:u["start"]])
        cursor = u["end"]
        if is_external:
            # 运行期挂载（ex::MountAll）与编译期内联是两条路：新语法暂时只覆盖后者，
            # 这条原样保留（宿主锚点 + 组件引用段），归 M5 再设计 data-binui-mount。
            out.append(html[u["start"]:u["end"]])
            continue
        if comp is None or comp.lower() not in roots:
            out.append(html[u["start"]:u["end"]])   # 找不到组件：原样保留，交给编译器报警
            continue
        root = roots[comp.lower()]
        pairs = []
        cls = None
        style = None
        for k, v in root["attrs"]:
            kl = k.lower()
            if kl == "class":
                cls = substitute(v or "", params)
            elif kl == "style":
                style = substitute(v or "", params)
        style = fix_style(style) if style else ""
        # 几何：<use> 的 x/y/w/h 老编译器走属性，这里折进 style
        geo = []
        if not style_has(style, "position") and ("x" in params or "y" in params):
            geo.append("position:absolute")
        for prop, key in (("left", "x"), ("top", "y"), ("width", "w"), ("height", "h")):
            if key in params and params[key] and not style_has(style, prop):
                geo.append("%s:%s" % (prop, px_fix(params[key])))
        full_style = ";".join([s for s in geo + ([style] if style else []) if s])
        if cls:
            pairs.append(("class", cls))
        if full_style:
            pairs.append(("style", full_style))
        pairs.append(("data-component", src))
        for k, v in root["attrs"]:
            kl = k.lower()
            if kl in ("class", "style"):
                continue
            val = substitute(v or "", params)
            if val == "":
                continue
            pairs.append((k, val))
        for k, v in params.items():
            pairs.append(("data-" + k, v))
        # 宿主**不内联**组件内部结构：它的子节点由编译器按 data-component 展开
        # （内联一份 + data-component 再展开一份 = 每个组件出现两次，且第二次是默认值）。
        # 宿主文本只承载"根元素自己的文本"（叶组件如 button 的默认文案就靠它）。
        root_text = root["inner"].strip()
        if u["inner"].strip():
            body = u["inner"]                       # <use> 自己带子元素：原样留在宿主里
        elif "<" in root_text:
            body = ""                               # 根有子元素：内部结构由编译器按 data-component 展开
        else:
            body = substitute(root_text, params)    # 叶组件：根自己的文本就是宿主文本
        out.append("<%s%s>%s</%s>" % (root["tag"], fmt_attrs(pairs), body, root["tag"]))
        count += 1
    out.append(html[cursor:])
    return "".join(out), count


def main(argv):
    check_only = "--check" in argv
    roots = {}           # 组件绝对路径(小写) -> root dict
    files = []
    for d in TARGET_DIRS:
        base = os.path.join(ROOT, d)
        for dirpath, _, names in os.walk(base):
            for nm in names:
                if not nm.lower().endswith((".html", ".htm")):
                    continue
                p = os.path.join(dirpath, nm)
                rel = os.path.relpath(p, ROOT).replace("\\", "/")
                if rel in SKIP_FILES:
                    continue
                files.append(p)

    def read(p):
        return open(p, encoding="utf-8").read()

    # 1) 收集组件根（.bum.html）
    for p in files:
        if ".bum." not in os.path.basename(p).lower():
            continue
        root = find_root(read(p))
        if root:
            roots[p.lower()] = root

    n_use = 0
    n_root = 0
    changed = []

    # 1.5) external（运行期挂载）引用的组件不摘根：它不像内联组件那样被拆成片段，
    #      而是整棵子树挂到宿主锚点下，根节点就是那棵子树的容器。
    keep_root = set()
    for p in files:
        for u in find_uses(read(p)):
            names = {k.lower(): (v or "") for k, v in u["attrs"]}
            if "external" in names or names.get("mode", "").strip().lower() == "external":
                comp = probe_component(os.path.dirname(p), names.get("src", "").strip())
                if comp:
                    keep_root.add(comp.lower())

    # 2) 摘掉组件根 -> 片段
    for p in files:
        if p.lower() not in roots or p.lower() in keep_root:
            continue
        html = read(p)
        root = find_root(html)
        new = html[:root["start"]] + root["inner"] + html[root["end"]:]
        if new != html:
            n_root += 1
            changed.append((os.path.relpath(p, ROOT), "摘根 <%s>" % root["tag"]))
            if not check_only:
                open(p, "w", encoding="utf-8", newline="\n").write(new)

    # 3) 每个文件里把 <use> 展开成宿主元素。
    #    要跑多趟：展开一个组件时，它的内部结构里可能还有一层 <use>（嵌套组件），
    #    那一层是**上一趟插入的文本**，同一趟扫不到。
    n_use = 0
    for _pass in range(5):
        pass_count = 0
        for p in files:
            html = read(p)
            new_html, cnt = expand_uses(html, os.path.dirname(p), roots)
            if cnt == 0:
                continue
            n_use += cnt
            pass_count += cnt
            if new_html != html:
                changed.append((os.path.relpath(p, ROOT), "%d 处 <use>" % cnt))
                if not check_only:
                    open(p, "w", encoding="utf-8", newline="\n").write(new_html)
        if pass_count == 0:
            break

    print("组件根摘除：%d 个文件（external 保留根的：%d）" % (n_root, len(keep_root)))
    print("<use> 展开：%d 处" % n_use)
    print("改动文件：%d 个" % len(changed))
    for rel, why in changed:
        print("  %-46s %s" % (rel, why))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
