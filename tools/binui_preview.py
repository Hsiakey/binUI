#!/usr/bin/env python3
# binui_preview.py —— 把 binUI 布局源码摊成"浏览器能打开的镜像页"（P0 原型）
#
# 为什么需要它：binUI 的源码是**编译期布局语言**（{{参数}}、无单位 px、<use> 内联），
# 浏览器直接打开必然散架。这个工具不参与渲染，只生成一份近似 HTML：
#   1) <use src=...> 递归内联（按 src 相对该文件解析，与编译器一致）
#   2) {{name}} / {{name=默认}} 用 <use> 上的属性替换
#   3) 无单位长度补 px（CSS 里除 0 外必须有单位）
#   4) 自定义标签（<titlebar> / <use> / <app_window>）映射成 <div data-binui=...>
#   5) <style> 原样保留（binUI 的 CSS 是子集，浏览器能认大半）
#
# 明确不是像素权威：字体度量、文字换行、阴影/渐变细节会和 bin2d_shot 有差；
# 它的用途是"看结构、看盒子、看层级"。像素请用 bin2d_shot。
#
# 用法: python tools/binui_preview.py <in.html> [out.html]
import html
import os
import re
import sys
from html.parser import HTMLParser

VOID = {"img", "input", "br", "hr", "meta", "link", "source", "col", "area", "base", "wbr", "embed"}
TAGMAP = {"app_window": "div", "binui": "div", "html": "div", "body": "div",
          "titlebar": "div", "panel": "div", "statusbar": "div"}
LENGTH_PROPS = {
    "width", "height", "left", "top", "right", "bottom",
    "min-width", "min-height", "max-width", "max-height",
    "margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
    "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
    "border-radius", "border-width", "font-size", "letter-spacing", "word-spacing",
    "gap", "row-gap", "column-gap", "text-indent", "outline-offset", "flex-basis",
}


class Node:
    def __init__(self, tag, attrs=None, parent=None):
        self.tag = tag
        self.attrs = list(attrs or [])
        self.children = []
        self.parent = parent
        self.is_text = tag is None

    def attr(self, name):
        for k, v in self.attrs:
            if k.lower() == name:
                return v
        return None

    def set_attr(self, name, value):
        for i, (k, _) in enumerate(self.attrs):
            if k.lower() == name:
                self.attrs[i] = (k, value)
                return
        self.attrs.append((name, value))


class Builder(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=False)
        self.root = Node("#root")
        self.cur = self.root
        self.raw = None          # <style>/<script> 里收原文
        self.raw_buf = None

    def handle_starttag(self, tag, attrs):
        tag = tag.lower()
        node = Node(tag, attrs, self.cur)
        self.cur.children.append(node)
        if tag in VOID:
            return
        if tag in ("style", "script"):
            self.raw = node
            self.raw_buf = []
        self.cur = node

    def handle_startendtag(self, tag, attrs):
        tag = tag.lower()
        self.cur.children.append(Node(tag, attrs, self.cur))

    def handle_endtag(self, tag):
        tag = tag.lower()
        if self.raw is not None and tag in ("style", "script"):
            self.raw.children.append(Node(None, parent=self.raw))
            self.raw.children[-1].text = "".join(self.raw_buf or [])
            self.raw = None
            self.raw_buf = None
        n = self.cur
        while n is not self.root and n.tag != tag:
            n = n.parent
        if n is not self.root:
            self.cur = n.parent

    def handle_data(self, data):
        if self.raw_buf is not None:
            self.raw_buf.append(data)
            return
        self.cur.children.append(Node(None, parent=self.cur))
        self.cur.children[-1].text = data

    def handle_entityref(self, name):
        self.handle_data("&%s;" % name)

    def handle_charref(self, name):
        self.handle_data("&#%s;" % name)


def parse(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        src = f.read()
    b = Builder()
    b.feed(src)
    b.close()
    return b.root, src


def px_fix_value(value):
    parts = value.strip().split()
    if not parts:
        return value
    out = []
    for p in parts:
        if re.fullmatch(r"-?\d+(\.\d+)?", p):
            out.append(p + "px")
        else:
            out.append(p)
    return " ".join(out)


def css_fix(block):
    """只处理 { } 里的声明；选择器原样保留。"""
    out, i, n = [], 0, len(block)
    while i < n:
        j = block.find("{", i)
        if j < 0:
            out.append(block[i:])
            break
        out.append(block[i:j + 1])
        k = block.find("}", j)
        if k < 0:
            out.append(block[j + 1:])
            break
        body = block[j + 1:k]
        def repl(m):
            prop = m.group(1).lower()
            if prop in LENGTH_PROPS:
                return m.group(1) + ":" + px_fix_value(m.group(2))
            return m.group(0)
        body = re.sub(r"([A-Za-z-]+)\s*:\s*([^;]+)(?=;|\s*$)", repl, body)
        out.append(body + "}")
        i = k + 1
    return "".join(out)


def subst(text, params):
    def repl(m):
        body = m.group(1)
        if "=" in body:
            name, default = body.split("=", 1)
        else:
            name, default = body, ""
        return params.get(name.strip().lower(), default)
    return re.sub(r"\{\{([^{}]*)\}\}", repl, text)


def fix_style_attr(value, params):
    value = subst(value, params)
    decls = []
    for d in value.split(";"):
        if ":" not in d:
            continue
        prop, val = d.split(":", 1)
        if prop.strip().lower() in LENGTH_PROPS:
            val = px_fix_value(val)
            # binUI 的 width="content" = 按文字宽度；浏览器里最接近的是 max-content
            if val.strip() == "content":
                val = "max-content"
        decls.append(prop.strip() + ":" + val.strip())
    return ";".join(decls)


# 容器标签上的尺寸/底色不是 CSS 属性，预览时折进 style，否则浏览器里没有盒子
CONTAINER_ATTRS = {"app_window", "html", "body", "binui"}


def container_attrs_to_style(node, orig_tag=None):
    """<app_window width="780" height="600" bg="#F4F7FC"> → style 里的 width/height/background。"""
    if (orig_tag or node.tag) not in CONTAINER_ATTRS:
        return
    extra = []
    for name, prop in (("width", "width"), ("height", "height"), ("bg", "background")):
        v = node.attr(name)
        if v is None:
            continue
        if prop in LENGTH_PROPS and re.fullmatch(r"-?\d+(\.\d+)?", v.strip()):
            v = v.strip() + "px"
        extra.append(prop + ":" + v)
    if extra:
        st = node.attr("style") or ""
        node.set_attr("style", (st + ";" if st else "") + ";".join(extra))


def expand(node, base_dir, out_dir, depth=0, seen=None):
    """返回新节点（不修改输入）。"""
    seen = seen or []
    if depth > 8:
        return Node("div", [("data-binui-error", "too-deep")])
    if node.is_text:
        n = Node(None)
        # 独立预览组件时没有 <use> 传参：按默认值代入（{{k=v}} → v）
        n.text = subst(node.text, {})
        return n
    if node.tag == "#root":
        n = Node("#root")
        n.children = [expand(c, base_dir, out_dir, depth, seen) for c in node.children]
        return n
    if node.tag == "use":
        src = node.attr("src")
        if not src:
            return Node("div", [("data-binui-error", "use-without-src")])
        path = os.path.join(base_dir, src)
        if not os.path.exists(path):
            n = Node("div", [("data-binui-error", "missing:" + src)])
            return n
        root, _ = parse(path)
        params = {}
        for k, v in node.attrs:
            if k.lower() not in ("src", "x", "y", "w", "h", "z", "external", "id", "class"):
                params[k.lower()] = v
        # 组件文件可能有多根：套一个 div
        tops = [c for c in root.children if not c.is_text or (c.text or "").strip()]
        comp_root = tops[0] if len(tops) == 1 else None
        if comp_root is None:
            comp_root = Node("div")
            comp_root.children = tops
        tagname = comp_root.tag
        comp_root = expand_root(comp_root, os.path.dirname(path), out_dir, depth + 1, seen + [path], params)
        # 宿主子元素：binUI 的语义是"落进组件根容器的流式末尾"（panel 靠这条）
        for ch in node.children:
            comp_root.children.append(expand_component_child(ch, base_dir, out_dir, depth, seen, params))
        # 位置/尺寸：<use x y w h> → 根上的绝对定位
        pos = []
        if node.attr("x") is not None:
            pos.append("left:" + node.attr("x") + "px")
        if node.attr("y") is not None:
            pos.append("top:" + node.attr("y") + "px")
        if node.attr("w") is not None:
            pos.append("width:" + node.attr("w") + "px")
        if node.attr("h") is not None:
            pos.append("height:" + node.attr("h") + "px")
        if pos:
            style = comp_root.attr("style") or ""
            comp_root.set_attr("style", (style + (";" if style else "") + ";".join(pos) + ";position:absolute"))
        comp_root.set_attr("data-binui-component", tagname)
        return comp_root
    n = Node(TAGMAP.get(node.tag, node.tag), [], None)
    if n.tag == node.tag and node.tag not in TAGMAP:
        if not re.fullmatch(r"[a-z][a-z0-9-]*", node.tag):
            n = Node("div", [("data-binui-tag", node.tag)])
    if node.tag in TAGMAP and node.tag not in ("html", "body"):
        n.set_attr("data-binui", node.tag)
    for k, v in node.attrs:
        n.attrs.append((k, subst(v, {})))
    st = n.attr("style")
    if st:
        n.set_attr("style", fix_style_attr(st, {}))
    container_attrs_to_style(n, node.tag)
    n.children = [expand(c, base_dir, out_dir, depth, seen) for c in node.children]
    if n.tag in ("style", "script"):
        for c in n.children:
            if c.is_text:
                c.text = css_fix(c.text) if n.tag == "style" else c.text
    return n


def expand_root(node, base_dir, out_dir, depth, seen, params):
    """组件根：应用参数替换，不套 use 的定位（由调用方加）。"""
    n = Node(TAGMAP.get(node.tag, node.tag), [], None)
    for k, v in node.attrs:
        n.attrs.append((k, subst(v, params)))
    n.set_attr("data-binui-component-root", node.tag)
    st = n.attr("style")
    if st:
        n.set_attr("style", fix_style_attr(st, params))
    for c in node.children:
        n.children.append(expand_component_child(c, base_dir, out_dir, depth, seen, params))
    return n


def expand_component_child(node, base_dir, out_dir, depth, seen, params):
    if node.is_text:
        t = Node(None)
        t.text = subst(node.text, params)
        return t
    if node.tag == "use":
        # 组件内嵌套 use：先替换属性里的参数，再走常规展开
        clone = Node("use", [(k, subst(v, params)) for k, v in node.attrs])
        return expand(clone, base_dir, out_dir, depth, seen)
    n = Node(TAGMAP.get(node.tag, node.tag), [], None)
    for k, v in node.attrs:
        n.attrs.append((k, subst(v, params)))
    st = n.attr("style")
    if st:
        n.set_attr("style", fix_style_attr(st, params))
    container_attrs_to_style(n, node.tag)
    n.children = [expand_component_child(c, base_dir, out_dir, depth, seen, params) for c in node.children]
    if node.tag == "style":
        for c in n.children:
            if c.is_text:
                c.text = css_fix(c.text or "")
    return n


def emit(node, out):
    if node.is_text:
        out.append(html.escape(node.text, quote=False) if "{{" not in (node.text or "") else node.text)
        return
    if node.tag in ("#root",):
        for c in node.children:
            emit(c, out)
        return
    attrs = "".join(' %s="%s"' % (k, html.escape(v or "", quote=True)) for k, v in node.attrs)
    if node.tag == "style" or node.tag == "script":
        out.append("<%s%s>" % (node.tag, attrs))
        for c in node.children:
            if c.is_text:
                out.append(c.text or "")
        out.append("</%s>" % node.tag)
        return
    if node.tag in VOID:
        out.append("<%s%s>" % (node.tag, attrs))
        return
    out.append("<%s%s>" % (node.tag, attrs))
    for c in node.children:
        emit(c, out)
    out.append("</%s>" % node.tag)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    if not args:
        print("usage: python tools/binui_preview.py <in.html> [out.html] [--stage]")
        print("  --stage  组件/页面独立视图：棋盘底、居中、1x/2x/4x/8x 缩放、盒边界")
        return 1
    src = args[0]
    dst = args[1] if len(args) > 1 else os.path.splitext(src)[0] + (".stage.html" if "--stage" in flags else ".preview.html")
    root, _ = parse(src)
    expanded = expand(root, os.path.dirname(os.path.abspath(src)), os.path.dirname(os.path.abspath(dst)))
    body = []
    emit(expanded, body)
    content = "".join(body)
    stage = "--stage" in flags
    if stage:
        page = """<!doctype html>
<html><head><meta charset="utf-8">
<title>binUI · %s</title>
<style>
  html,body{margin:0;height:100%%;font-family:"Segoe UI","Microsoft YaHei",sans-serif;}
  button,input{font:inherit;border:0;padding:0;background:transparent;color:inherit;}
  body{display:flex;align-items:center;justify-content:center;padding:72px 48px 48px;
    background-color:#fff;
    background-image:linear-gradient(45deg,#eceff4 25%%,transparent 25%%,transparent 75%%,#eceff4 75%%),
                     linear-gradient(45deg,#eceff4 25%%,transparent 25%%,transparent 75%%,#eceff4 75%%);
    background-size:16px 16px;background-position:0 0,8px 8px;}
  #wrap{position:relative}
  #stage{transform-origin:top left;background:#fff}
  #stage.outline{outline:1px dashed #3E8BFF;outline-offset:0}
  #bar{position:fixed;left:50%%;top:16px;transform:translateX(-50%%);display:flex;gap:8px;align-items:center;
    background:#1f2937ee;color:#fff;padding:6px 10px;border-radius:8px;font:12px/1.4 monospace;z-index:99999}
  #bar button{background:#374151;color:#fff;border:0;border-radius:4px;padding:3px 8px;cursor:pointer;font:inherit}
  #bar button:hover{background:#4b5563}
  #bar label{display:flex;gap:4px;align-items:center;cursor:pointer}
  #zinfo{opacity:.75}
</style>
</head><body>
<div id="bar">
  <span>缩放</span>
  <button data-z="1">1x</button><button data-z="2">2x</button><button data-z="4">4x</button><button data-z="8">8x</button>
  <label><input type="checkbox" id="ol"> 盒边界</label>
  <span id="zinfo"></span>
</div>
<div id="wrap"><div id="stage">%s</div></div>
<script>
(function(){
  var stage=document.getElementById('stage'), wrap=document.getElementById('wrap'),
      info=document.getElementById('zinfo'), ol=document.getElementById('ol');
  var z=parseFloat(new URLSearchParams(location.search).get('zoom')||'1')||1;
  function apply(){
    stage.style.transform='scale('+z+')';
    wrap.style.width=(stage.offsetWidth*z)+'px';
    wrap.style.height=(stage.offsetHeight*z)+'px';
    info.textContent=Math.round(z*100)+'%% · '+stage.offsetWidth+'×'+stage.offsetHeight;
  }
  document.querySelectorAll('#bar button[data-z]').forEach(function(b){
    b.onclick=function(){z=parseFloat(b.dataset.z);apply();};
  });
  ol.onchange=function(){stage.classList.toggle('outline',ol.checked);};
  addEventListener('keydown',function(e){
    if(e.key==='0'){z=1;apply();}
    if(e.key==='+'||e.key==='='){z=Math.min(8,z*2);apply();}
    if(e.key==='-'){z=Math.max(0.5,z/2);apply();}
  });
  apply();
})();
</script>
</body></html>
""" % (os.path.basename(src), content)
    else:
        page = """<!doctype html>
<html><head><meta charset="utf-8">
<title>binUI preview — %s</title>
<style>
  html,body{margin:0;padding:0;background:#fff;font-family:"Segoe UI","Microsoft YaHei",sans-serif;}
  button,input{font:inherit;border:0;padding:0;background:transparent;color:inherit;}
  .binui-preview-note{position:fixed;right:8px;bottom:8px;background:#000a;color:#fff;
    font:12px/1.4 monospace;padding:4px 8px;border-radius:4px;z-index:99999}
</style>
</head><body>
%s
<div class="binui-preview-note">binUI 镜像预览（近似布局；像素请用 bin2d_shot）</div>
</body></html>
""" % (os.path.basename(src), content)
    with open(dst, "w", encoding="utf-8") as f:
        f.write(page)
    print("wrote", dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
