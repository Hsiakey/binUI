#!/usr/bin/env python3
"""binui_nodes.py - 把 draw_bench --dump-nodes 的 JSON + bin2d_shot 的 PNG 合成一个单页查看器。

用法:
    python tools/binui_nodes.py build-mingw/tmp/buttonshow.nodes.json \
        build-mingw/tmp/buttonshow.png -o out/nodes/buttonshow.html

    # 只要结构不要图（PNG 可省）
    python tools/binui_nodes.py nodes.json -o view.html

为什么要有这个：`bin2d_shot` 回答"看起来对不对"，这份查看器回答"**布局对不对**" ——
数据来自 renderer 排完之后的 getNodes()（不是猜的、不是按源码重算的），
所以它上面的盒子位置就是 binUI 真渲染时的盒子位置。

产物是自包含 HTML（JSON 与 PNG 都内联成 base64），`file://` 直接打开即可，
不需要起服务。答案对应的 issue：docs/issues/14-browser-preview-dx.md §3 P1。
"""
import base64
import json
import os
import sys

# Windows 控制台默认 cp936，中文输出在管道里会变乱码（UTF-8 读方看到的是问号）；
# 统一按 UTF-8 打印，和仓库里其它工具（C++ 侧 printf 的 UTF-8 字面量）一致。
try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

USAGE = """用法: python tools/binui_nodes.py <nodes.json> [shot.png] [-o out.html]

  nodes.json  draw_bench <ui> --dump-nodes 的产物
  shot.png    可省；bin2d_shot <ui> 的产物
  -o          输出 HTML（默认 <nodes.json 同名>.view.html）
"""

TYPE_COLORS = {
    "root": "#7A8699", "label": "#4C8DFF", "image": "#9B59B6", "button": "#2ECC71",
    "input": "#E67E22", "list": "#16A085", "line": "#95A5A6", "svg": "#D35400",
    "canvas": "#C0392B", "video": "#8E44AD", "path": "#F39C12",
}


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;"))


def main(argv):
    args, out = [], None
    i = 1
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            print(USAGE)
            return 0
        if a == "-o" and i + 1 < len(argv):
            out = argv[i + 1]
            i += 2
            continue
        args.append(a)
        i += 1
    if not args:
        print(USAGE)
        return 1

    nodes_path = args[0]
    shot_path = args[1] if len(args) > 1 and not args[1].endswith(".json") else None
    if out is None:
        out = os.path.splitext(nodes_path)[0] + ".view.html"

    with open(nodes_path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    nodes = doc.get("nodes") or []
    if not nodes:
        print("binui_nodes: JSON 里没有 nodes")
        return 1

    png_b64 = ""
    if shot_path and os.path.exists(shot_path):
        with open(shot_path, "rb") as f:
            png_b64 = base64.b64encode(f.read()).decode("ascii")

    # 树：按 parent 关系摊平成"深度 + 行"，DOM 顺序即原顺序（draw_bench 导的就是这个顺序）
    by_id = {}
    children = {}
    for n in nodes:
        by_id[n["id"]] = n
        pid = n.get("parent", 65535)
        children.setdefault(pid, []).append(n)

    rows = []

    def walk(pid, depth):
        for n in children.get(pid, []):
            rows.append((n, depth))
            walk(n["id"], depth + 1)

    roots = [n for n in nodes if n.get("parent", 65535) not in by_id]
    for r in roots:
        rows.append((r, 0))
        walk(r["id"], 1)

    row_html = []
    for n, depth in rows:
        t = n.get("type", "?")
        color = TYPE_COLORS.get(t, "#888")
        name = n.get("name") or ""
        label = esc(t)
        if name:
            label += '<span class="nm">#' + esc(name) + "</span>"
        else:
            label += '<span class="id">#' + str(n["id"]) + "</span>"
        box = "%g×%g" % (n.get("w", 0), n.get("h", 0))
        cls = []
        if not n.get("shown", True):
            cls.append("hidden-node")
        elif n.get("clips"):
            cls.append("clipped")
        if n.get("scrollable"):
            cls.append("scroll")
        row_html.append(
            '<div class="row %s" data-id="%d" style="padding-left:%dpx">'
            '<span class="dot" style="background:%s"></span>'
            '<span class="lb">%s</span><span class="bx">%s</span>'
            '<span class="xy">%g,%g</span></div>'
            % (" ".join(cls), n["id"], 6 + depth * 14, color, label, box,
               n.get("x", 0), n.get("y", 0)))

    doc_json = json.dumps(doc, ensure_ascii=False)
    ui_name = esc(doc.get("ui", ""))
    win = doc.get("window") or {}
    title = "%s · %s" % (ui_name, "%g×%g" % (win.get("w", 0), win.get("h", 0)))

    html = TEMPLATE.format(
        title=esc(title),
        ui=ui_name,
        node_count=len(nodes),
        rows="\n".join(row_html),
        doc_json=doc_json,
        png_b64=png_b64,
        has_shot="1" if png_b64 else "0",
    )
    d = os.path.dirname(os.path.abspath(out))
    if d:
        os.makedirs(d, exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(html)
    print("binui_nodes: %d 节点 -> %s%s" % (len(nodes), out, "" if png_b64 else "（无截图）"))
    return 0


TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>binUI 结构查看器 · {title}</title>
<style>
  :root {{ --bg:#10141a; --panel:#171d26; --line:#232c39; --fg:#dfe6f0; --dim:#7d8b9e; --sel:#ffd54a; }}
  * {{ box-sizing: border-box; }}
  html, body {{ height:100%; margin:0; }}
  body {{ display:flex; background:var(--bg); color:var(--fg);
         font:12px/1.5 "Cascadia Mono","Consolas",monospace; overflow:hidden; }}
  #left {{ width:392px; min-width:392px; height:100%; display:flex; flex-direction:column;
           background:var(--panel); border-right:1px solid var(--line); }}
  #head {{ padding:10px 12px; border-bottom:1px solid var(--line); }}
  #head .t1 {{ font-size:13px; color:#fff; }}
  #head .t2 {{ color:var(--dim); margin-top:2px; word-break:break-all; }}
  #tools {{ padding:8px 12px; border-bottom:1px solid var(--line); display:flex; gap:10px;
            flex-wrap:wrap; align-items:center; color:var(--dim); }}
  #tools input[type=text] {{ flex:1 1 100%; background:#0d1117; color:var(--fg);
            border:1px solid var(--line); border-radius:4px; padding:4px 6px; font:inherit; }}
  #tree {{ flex:1; overflow:auto; padding:6px 0; }}
  .row {{ display:flex; align-items:center; gap:6px; height:20px; padding-right:8px;
          white-space:nowrap; cursor:pointer; border-left:2px solid transparent; }}
  .row:hover {{ background:#1e2632; }}
  .row.sel {{ background:#2a2f1c; border-left-color:var(--sel); }}
  .row.anc {{ background:#152330; }}
  .row.clipanc {{ background:#2b1620; }}
  .row.hidden-node {{ color:#5d6775; text-decoration:line-through; }}
  .row.clipped .bx {{ color:#ffb74d; }}
  .row.scroll .lb::after {{ content:" ⇕"; color:#4dd0e1; }}
  .dot {{ width:6px; height:6px; border-radius:2px; flex:0 0 6px; }}
  .lb {{ color:#cfd8e3; }}
  .nm {{ color:#7fd1ff; }}
  .id {{ color:#5d6775; }}
  .bx {{ margin-left:auto; color:#8aa0b8; }}
  .xy {{ color:#4d5a6b; width:74px; text-align:right; }}
  #right {{ flex:1; position:relative; overflow:auto; background:
        repeating-conic-gradient(#0f1319 0% 25%, #141922 0% 50%) 50% / 16px 16px; }}
  #stage {{ position:relative; margin:24px; width:max-content; }}
  #shot {{ display:block; image-rendering:pixelated; }}
  #ov {{ position:absolute; left:0; top:0; pointer-events:none; }}
  #noshot {{ margin:24px; padding:16px; border:1px dashed var(--line); color:var(--dim); }}
  #detail {{ position:fixed; right:12px; bottom:12px; width:360px; background:#0d1117ee;
             border:1px solid var(--line); border-radius:6px; padding:10px 12px;
             white-space:pre-wrap; word-break:break-all; display:none; }}
  #detail b {{ color:var(--sel); }}
  #zoom {{ position:fixed; right:12px; top:12px; display:flex; gap:6px; }}
  #zoom button {{ background:#1b2431; color:var(--fg); border:1px solid var(--line);
                  border-radius:4px; padding:3px 8px; font:inherit; cursor:pointer; }}
  #legend {{ display:flex; gap:12px; margin-top:6px; color:var(--dim); font-size:11px; }}
  .lg {{ display:flex; align-items:center; gap:4px; }}
  .sw {{ width:10px; height:10px; border-radius:2px; }}
</style></head><body>

<div id="left">
  <div id="head">
    <div class="t1">binUI 结构查看器</div>
    <div class="t2">{ui}</div>
    <div class="t2">{node_count} 个节点 · 数据来自 renderer 排完的盒子（不是按源码重算）</div>
    <div id="legend">
      <span class="lg"><span class="sw" style="background:#ffd54a"></span>选中</span>
      <span class="lg"><span class="sw" style="background:#4dd0e1"></span>祖先</span>
      <span class="lg"><span class="sw" style="background:#ff5d73"></span>裁剪祖先</span>
      <span class="lg"><span class="sw" style="background:#5d6775"></span>display:none</span>
    </div>
  </div>
  <div id="tools">
    <input type="text" id="q" placeholder="过滤：类型 / #名字 / 宽高（如 button、#ok、120×38）">
    <label><input type="checkbox" id="allbox"> 全部盒</label>
    <label><input type="checkbox" id="shownbox" checked> 只列显示中</label>
  </div>
  <div id="tree">{rows}</div>
</div>

<div id="right">
  <div id="zoom">
    <button data-z="0.5">0.5×</button><button data-z="1">1×</button>
    <button data-z="2">2×</button><button data-z="4">4×</button>
  </div>
  <div id="stage">
    <img id="shot" alt="bin2d_shot">
    <canvas id="ov"></canvas>
    <div id="noshot" style="display:none">没有截图：只画盒。<br>
      出图请跑 <code>bin2d_shot &lt;ui&gt; out.png</code> 再把 PNG 传给本工具。</div>
  </div>
</div>

<div id="detail"></div>

<script>
const DOC = {doc_json};
const HAS_SHOT = "{has_shot}" === "1";
const PNG = "data:image/png;base64,{png_b64}";
const NODES = DOC.nodes;
const BY = {{}};
for (const n of NODES) BY[n.id] = n;
const WIN = DOC.window || {{}};
const W = WIN.w || 1, H = WIN.h || 1;

const img = document.getElementById('shot');
const ov = document.getElementById('ov');
const stage = document.getElementById('stage');
const detail = document.getElementById('detail');
const tree = document.getElementById('tree');

let sel = -1, anc = new Set(), clipAnc = new Set(), zoom = 1;

if (HAS_SHOT) {{ img.src = PNG; }} else {{
  document.getElementById('noshot').style.display = 'block';
  img.style.width = W + 'px'; img.style.height = H + 'px';
}}
function layout() {{
  const w = HAS_SHOT ? (img.naturalWidth || W) : W;
  const h = HAS_SHOT ? (img.naturalHeight || H) : H;
  img.style.width = (w * zoom) + 'px';
  img.style.height = (h * zoom) + 'px';
  ov.width = w * zoom; ov.height = h * zoom;
  ov.style.width = (w * zoom) + 'px'; ov.style.height = (h * zoom) + 'px';
  ov.style.left = img.offsetLeft + 'px'; ov.style.top = img.offsetTop + 'px';
  stage.style.width = (w * zoom) + 'px';
  stage.style.height = (h * zoom) + 'px';
  draw();
}}
img.addEventListener('load', layout);

function hex(c) {{ return '#' + (c || 'FFFFFFFF').slice(-6); }}

function box(ctx, n, color, dash, label) {{
  const x = n.x * zoom, y = n.y * zoom, w = Math.max(1, n.w * zoom), h = Math.max(1, n.h * zoom);
  ctx.save();
  ctx.strokeStyle = color; ctx.lineWidth = 1; if (dash) ctx.setLineDash([4, 3]);
  ctx.strokeRect(x + .5, y + .5, w, h);
  if (label) {{
    ctx.setLineDash([]);
    ctx.font = '10px Consolas,monospace';
    const tw = ctx.measureText(label).width + 6;
    ctx.fillStyle = color === '#5d6775' ? '#00000099' : color + 'e6';
    ctx.fillRect(x, y - 13, tw, 12);
    ctx.fillStyle = (color === '#ffd54a') ? '#10141a' : '#fff';
    ctx.fillText(label, x + 3, y - 3.5);
  }}
  ctx.restore();
}}

function draw() {{
  if (!ov.width) return;
  const ctx = ov.getContext('2d');
  ctx.clearRect(0, 0, ov.width, ov.height);
  if (document.getElementById('allbox').checked) {{
    for (const n of NODES) {{
      if (!n.shown) {{ box(ctx, n, '#5d6775', true, null); continue; }}
      box(ctx, n, '#2f4157', false, null);
    }}
  }}
  for (const id of clipAnc) {{ const n = BY[id]; if (n) box(ctx, n, '#ff5d73', false, '裁剪 #' + id); }}
  for (const id of anc) {{ const n = BY[id]; if (n) box(ctx, n, '#4dd0e1', true, '祖先 #' + id); }}
  const s = BY[sel];
  if (s) box(ctx, s, s.shown ? '#ffd54a' : '#5d6775', !s.shown,
             (s.type || '?') + ' #' + s.id + (s.name ? ' · #' + s.name : ''));
}}

function describe(n) {{
  const f = [];
  if (n.visible) f.push('visible');
  if (!n.shown) f.push('display:none(祖先或自身)');
  if (n.absolute) f.push('absolute');
  if (n.scrollable) f.push('scrollable');
  if (n.clipOnly) f.push('clipOnly');
  const lines = [
    '<b>' + (n.type || '?') + ' #' + n.id + (n.name ? '  · #' + n.name : '') + '</b>',
    '盒子   x=' + n.x + '  y=' + n.y + '  w=' + n.w + '  h=' + n.h,
    '父     ' + (n.parent === 65535 ? '（无）' : '#' + n.parent + (BY[n.parent] ? ' (' + BY[n.parent].type + ')' : '')),
    '层叠   z=' + n.z + '   圆角 r=' + n.radius + '   内边距 ' + n.padding + '   字号 ' + n.fontSize,
    '颜色   ' + hex(n.color) + '   标志 ' + n.flags + ' [' + f.join(', ') + ']',
    '裁剪   祖先链 [' + (n.clips || []).map(i => '#' + i).join(', ') + ']' + (n.clipOnly ? '（自身 overflow:hidden）' : ''),
  ];
  if (n.scrollable) lines.push('滚动内容 ' + n.contentW + ' × ' + n.contentH + '（可视 ' + n.w + ' × ' + n.h + '）');
  if (n.text) lines.push('文本   ' + JSON.stringify(n.text));
  return lines.join('\n');
}}

function select(id) {{
  sel = id;
  anc = new Set(); clipAnc = new Set();
  const n = BY[id];
  if (n) {{
    let p = n.parent;
    while (p !== 65535 && BY[p]) {{ anc.add(p); p = BY[p].parent; }}
    for (const c of (n.clips || [])) clipAnc.add(c);
  }}
  for (const el of tree.querySelectorAll('.row')) {{
    el.classList.toggle('sel', +el.dataset.id === id);
    el.classList.toggle('anc', anc.has(+el.dataset.id));
    el.classList.toggle('clipanc', clipAnc.has(+el.dataset.id));
  }}
  detail.innerHTML = n ? describe(n) : '';
  detail.style.display = n ? 'block' : 'none';
  draw();
}}

tree.addEventListener('click', e => {{
  const row = e.target.closest('.row');
  if (row) select(+row.dataset.id);
}});
tree.addEventListener('mouseover', e => {{
  const row = e.target.closest('.row');
  if (!row) return;
  const n = BY[+row.dataset.id];
  if (!n) return;
  detail.innerHTML = describe(n);
  detail.style.display = 'block';
}});

// 图上点选：命中最深（面积最小）的显示中节点
img.addEventListener('click', e => {{
  const r = img.getBoundingClientRect();
  const x = (e.clientX - r.left) / zoom, y = (e.clientY - r.top) / zoom;
  let best = -1, bestA = Infinity;
  for (const n of NODES) {{
    if (!n.shown) continue;
    if (x >= n.x && y >= n.y && x <= n.x + n.w && y <= n.y + n.h) {{
      const a = Math.max(0.001, n.w * n.h);
      if (a <= bestA) {{ bestA = a; best = n.id; }}
    }}
  }}
  if (best >= 0) select(best);
}});

document.getElementById('q').addEventListener('input', e => {{
  const q = e.target.value.trim().toLowerCase();
  const onlyShown = document.getElementById('shownbox').checked;
  for (const el of tree.querySelectorAll('.row')) {{
    const n = BY[+el.dataset.id];
    let ok = true;
    if (onlyShown && n && !n.shown) ok = false;
    if (ok && q) {{
      const hay = (n.type + ' #' + n.id + ' ' + (n.name || '') + ' ' + n.w + '×' + n.h + ' ' + (n.text || '')).toLowerCase();
      ok = hay.includes(q);
    }}
    el.style.display = ok ? '' : 'none';
  }}
}});
document.getElementById('shownbox').addEventListener('change', () => {{
  const ev = new Event('input'); document.getElementById('q').dispatchEvent(ev);
}});
document.getElementById('allbox').addEventListener('change', draw);
document.getElementById('zoom').addEventListener('click', e => {{
  const b = e.target.closest('button'); if (!b) return;
  zoom = parseFloat(b.dataset.z); layout();
}});

layout();
// 深链：view.html#sel=17&all=1 —— 分享"看这个盒子"的现场，热刷新/重开都不丢
const params = new URLSearchParams(location.hash.slice(1) || location.search.slice(1));
if (params.get('all') === '1') document.getElementById('allbox').checked = true;
const wantSel = params.get('sel');
if (wantSel !== null && BY[+wantSel]) select(+wantSel);
else if (NODES.length) select(NODES[0].id);
draw();
</script>
</body></html>
"""

if __name__ == "__main__":
    sys.exit(main(sys.argv))
