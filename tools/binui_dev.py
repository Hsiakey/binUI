#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""binui_dev —— 本地静态服务 + 文件监听 + 热刷新（结构查看器在线版）。

把"看布局"从三条命令变成"存一下就变"：

    python tools/binui_dev.py examples/buttonshow/main.html
    http://127.0.0.1:8765/         ← stdout 第一行就是这个 URL（给脚本抓）

页面是两栏：左边 `tools/binui_nodes.py` 生成的结构查看器，右边 `bin2d_shot` 的真实渲染 PNG。
每次重建走的是现成二进制，服务本身不解析布局：

    build-mingw\\bin\\bin2d_shot.exe   <page> out\\dev\\<name>.png
    build-mingw\\bin\\draw_bench.exe   <page> --frames 2 --dump-nodes out\\dev\\<name>.json
    python tools\\binui_nodes.py       <json> <png> -o out\\dev\\<name>.view.html

监听范围 = 页面 + 它引用的组件（`<use src>` / `data-component`）+ 被引用的 .css/.js，
mtime 每秒轮询（不引 watchdog）。变了就通过 SSE（`/events`）推一条 reload，页面自己刷新；
SSE 断了浏览器会自动重连（服务端也发了 retry: 1000）。

只读源码：产物全部写 `out/dev/`（已在 .gitignore 里）。只用标准库。
"""

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

# ------------------------------------------------------------------ 路径

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN_DIR = os.path.join(ROOT, "build-mingw", "bin")
TOOLS_DIR = os.path.join(ROOT, "tools")
OUT_DIR = os.path.join(ROOT, "out", "dev")

BIN2D_SHOT = os.path.join(BIN_DIR, "bin2d_shot.exe")
DRAW_BENCH = os.path.join(BIN_DIR, "draw_bench.exe")
NODES_PY = os.path.join(TOOLS_DIR, "binui_nodes.py")
PREVIEW_JS = os.path.join(TOOLS_DIR, "binui-preview.js")

# 组件引用：<use src="..."> / data-component="..."；样式脚本：<link href> / <script src>
RE_USE = re.compile(r"""<\s*use\b[^>]*?\bsrc\s*=\s*["']([^"']+)["']""", re.I)
RE_COMPONENT = re.compile(r"""\bdata-component\s*=\s*["']([^"']+)["']""", re.I)
RE_ASSET = re.compile(r"""\b(?:href|src)\s*=\s*["']([^"']+\.(?:css|js))["']""", re.I)

WATCH_INTERVAL = 1.0     # 秒：mtime 轮询
WATCH_DEPTH = 3          # 组件里再引组件，最多跟这么多层
SSE_PING_INTERVAL = 15.0 # 秒：SSE 心跳（保持连接不被中间设备掐断）
MAX_PORT_TRIES = 20      # 端口占用时 +1 重试的次数


# ------------------------------------------------------------------ 日志

class Log:
    """带时间戳的 stdout 日志。启动阶段先缓存，URL 打出来之后再倒出来。"""

    def __init__(self):
        self.buffered = True
        self.pending = []

    def __call__(self, msg):
        line = time.strftime("[%H:%M:%S] ") + msg
        if self.buffered:
            self.pending.append(line)
            return
        print(line, flush=True)

    def flush_after_url(self):
        self.buffered = False
        for line in self.pending:
            print(line, flush=True)
        self.pending = []


log = Log()


# ------------------------------------------------------------------ 子进程

def run_tool(cmd, what, timeout=180):
    """跑一个外部工具；遇到 Permission denied（Lead 正在重编二进制）重试一次。

    返回 (ok, 输出文本)。输出统一进日志，出问题时也能在页面状态栏看到。
    """
    for attempt in (1, 2):
        t0 = time.time()
        try:
            proc = subprocess.run(
                cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=timeout,
            )
        except FileNotFoundError:
            return False, "%s 不存在：%s" % (what, cmd[0])
        except subprocess.TimeoutExpired:
            return False, "%s 超时（%d ms）：%s" % (what, timeout * 1000, " ".join(cmd[1:]))
        except PermissionError:
            out = "Permission denied"
            proc = None
        out = "" if proc is None else proc.stdout.decode("utf-8", "replace")
        rc = 1 if proc is None else proc.returncode
        ms = int((time.time() - t0) * 1000)
        if rc == 0:
            first = out.strip().splitlines()[-1] if out.strip() else "ok"
            log("  %s ok（%d ms）：%s" % (what, ms, first[:160]))
            return True, out
        locked = "permission denied" in out.lower() or "拒绝访问" in out
        if attempt == 1 and locked:
            log("  %s 被占用（Lead 可能正在重编），0.4 s 后重试一次" % what)
            time.sleep(0.4)
            continue
        log("  %s 失败（rc=%d，%d ms）：%s" % (what, rc, ms, out.strip()[:300]))
        return False, out
    return False, "重试后仍失败"


# ------------------------------------------------------------------ 监听范围

def resolve(path, base_dir):
    if not path:
        return None
    if os.path.isabs(path):
        return os.path.normpath(path)
    return os.path.normpath(os.path.join(base_dir, path))


def collect_watch_paths(page):
    """页面 + 它引用的组件 + 被引用的 css/js（组件再用 <use> 引用别的组件也跟上）。"""
    page = os.path.abspath(page)
    seen = set()
    ordered = []
    queue = [(page, 0)]
    while queue:
        path, depth = queue.pop(0)
        if path in seen or not os.path.isfile(path):
            continue
        seen.add(path)
        ordered.append(path)
        if depth >= WATCH_DEPTH:
            continue
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        base = os.path.dirname(path)
        for ref in RE_USE.findall(text) + RE_COMPONENT.findall(text) + RE_ASSET.findall(text):
            child = resolve(ref, base)
            if child and child not in seen:
                queue.append((child, depth + 1))
    # 同目录下的 .css/.js：只在"页面里确实引了 .css/.js"时才加（见 RE_ASSET），
    # 这里再补一条兜底：页面同目录的样式/脚本文件全算上 —— 改样式是最常见的动作。
    page_dir = os.path.dirname(page)
    try:
        for name in sorted(os.listdir(page_dir)):
            if os.path.splitext(name)[1].lower() in (".css", ".js"):
                p = os.path.join(page_dir, name)
                if os.path.isfile(p) and p not in seen:
                    seen.add(p)
                    ordered.append(p)
    except OSError:
        pass
    return ordered


def signature(paths):
    sig = []
    for p in paths:
        try:
            st = os.stat(p)
            sig.append((p, st.st_mtime_ns, st.st_size))
        except OSError:
            sig.append((p, 0, -1))
    return tuple(sig)


# ------------------------------------------------------------------ 构建

class Builder:
    """按需重建：PNG + 节点 JSON + 查看器 HTML。串行化，缓存按监听签名。"""

    def __init__(self, page, shot_only):
        self.page = os.path.abspath(page)
        self.shot_only = shot_only
        stem = os.path.splitext(os.path.basename(self.page))[0]
        self.stem = stem
        self.png = os.path.join(OUT_DIR, stem + ".png")
        self.nodes_json = os.path.join(OUT_DIR, stem + ".json")
        self.view_html = os.path.join(OUT_DIR, stem + ".view.html")
        self.lock = threading.Lock()
        self.key = None
        self.status = "尚未生成"
        self.ok = False
        self.last_ok_at = 0.0
        self.builds = 0

    def ensure(self, force=False):
        """保证产物存在且与源码同步。返回 (ok, status)。"""
        with self.lock:
            key = signature(collect_watch_paths(self.page))
            fresh = (key == self.key and os.path.exists(self.png)
                     and (self.shot_only or os.path.exists(self.view_html)))
            if not force and fresh:
                log("  命中缓存，跳过重建（%d 次重建 / 本次进程）" % self.builds)
                return self.ok, self.status
            self.key = key
            return self._build()

    def _build(self):
        t0 = time.time()
        rel = os.path.relpath(self.page, ROOT).replace("\\", "/")
        log("重建 %s" % rel)

        ok, out = run_tool([BIN2D_SHOT, self.page, self.png], "bin2d_shot")
        if not ok:
            self.ok = False
            self.status = "bin2d_shot 失败：" + out.strip().splitlines()[-1][:200] if out.strip() else "bin2d_shot 失败"
            return False, self.status
        shot_line = out.strip().splitlines()[-1] if out.strip() else ""

        if self.shot_only:
            self.ok = True
            self.builds += 1
            self.last_ok_at = time.time()
            self.status = "%s（只出图，%.0f ms）" % (shot_line, (time.time() - t0) * 1000)
            return True, self.status

        ok, out = run_tool([DRAW_BENCH, self.page, "--frames", "2", "--dump-nodes",
                            self.nodes_json], "draw_bench --dump-nodes")
        if not ok:
            self.ok = False
            self.status = "draw_bench 失败（{0}）".format(
                out.strip().splitlines()[-1][:200] if out.strip() else "无输出")
            return False, self.status
        bench_line = ""
        for line in out.splitlines():
            if "节点" in line and "窗口" in line:
                bench_line = line.strip()

        ok, out = run_tool([sys.executable, NODES_PY, self.nodes_json, self.png,
                            "-o", self.view_html], "binui_nodes")
        if not ok:
            self.ok = False
            self.status = "binui_nodes 失败（{0}）".format(
                out.strip().splitlines()[-1][:200] if out.strip() else "无输出")
            return False, self.status

        self.ok = True
        self.builds += 1
        self.last_ok_at = time.time()
        ms = int((time.time() - t0) * 1000)
        self.status = "%s · %s · 重建 %d ms" % (shot_line, bench_line.split("  ")[1] if "  " in bench_line else "", ms)
        return True, self.status


# ------------------------------------------------------------------ 浏览器镜像
#
# /file/<相对路径> 把仓库当静态站直接喂给浏览器（**源码本身**），并在 HTML 里注入
# tools/binui-preview.js —— 组件内部结构由它在浏览器里展开（M2）。这样"看布局"有两份：
# 左边 PNG 是 binUI 的真实像素（权威），右边是浏览器对同一份源码的解释（写法校验）。
#
# /__binui/c/<相对路径>?zoom=4&grid=1 是单个组件的独立视图（棋盘格 + 居中 + 缩放）。

MIME = {
    ".html": "text/html; charset=utf-8", ".htm": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8", ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8", ".svg": "image/svg+xml",
    ".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg",
    ".gif": "image/gif", ".webp": "image/webp", ".ico": "image/x-icon",
    ".woff2": "font/woff2", ".woff": "font/woff", ".ttf": "font/ttf",
    ".mp4": "video/mp4", ".webm": "video/webm",
    ".txt": "text/plain; charset=utf-8",
}

PREVIEW_TAG = '<script src="/__binui/preview.js" data-binui-dev></script>'

COMPONENT_SHELL = """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>binui dev · 组件独立视图</title>
</head><body>
<script src="/__binui/preview.js" data-binui-dev data-binui-standalone></script>
</body></html>
"""

# 组件文件解析顺序（与编译器的 ProbeAsset 同口径：无扩展名时依次探测）
COMPONENT_SUFFIXES = ("", ".html", ".xml", ".bum.html", ".bum.xml", ".bui.html", ".bui.xml")


def inject_preview(html_text):
    """把 preview.js 的 <script> 塞进 </body> 之前（没有 body 就追加到末尾）。"""
    if PREVIEW_TAG in html_text or "data-binui-preview-off" in html_text:
        return html_text
    low = html_text.lower()
    at = low.rfind("</body>")
    if at < 0:
        at = low.rfind("</html>")
    if at < 0:
        return html_text + "\n" + PREVIEW_TAG + "\n"
    return html_text[:at] + PREVIEW_TAG + "\n" + html_text[at:]


def safe_path(rel):
    """相对路径 → ROOT 下的绝对路径；越界（.. 爬出仓库）返回 None。"""
    rel = unquote(rel).replace("\\", "/").lstrip("/")
    if not rel:
        return None
    p = os.path.normpath(os.path.join(ROOT, rel))
    if p != ROOT and not p.startswith(ROOT + os.sep):
        return None
    return p


def resolve_component(src):
    """组件引用 → (相对路径, 源码文本, 错误文本)。二进制 .bum 不支持（新语法只认源码）。"""
    path = urlparse(src).path if "://" in (src or "") else (src or "")
    if path.startswith("/file/"):
        path = path[len("/file/"):]
    base = safe_path(path)
    if not base:
        return None, None, "组件路径不合法：%s" % (src or "")
    for suffix in COMPONENT_SUFFIXES:
        cand = base + suffix
        if not os.path.isfile(cand):
            continue
        if cand.lower().endswith((".bum", ".bui")):
            return None, None, "组件是二进制布局（%s），预览需要 .bum.html 源码" % os.path.basename(cand)
        try:
            with open(cand, "r", encoding="utf-8", errors="replace") as f:
                return os.path.relpath(cand, ROOT).replace("\\", "/"), f.read(), None
        except OSError as exc:
            return None, None, "读不到组件文件：%s" % exc
    return None, None, "读不到组件文件：%s（.bum / .bum.html / .bum.xml 都不存在）" % path


# ------------------------------------------------------------------ SSE 广播

class EventHub:
    def __init__(self):
        self.lock = threading.Lock()
        self.queues = []
        self.seq = 0

    def subscribe(self):
        q = []
        with self.lock:
            self.queues.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.queues:
                self.queues.remove(q)

    def publish(self, payload):
        with self.lock:
            self.seq += 1
            for q in self.queues:
                q.append(payload)
            n = len(self.queues)
        log("[sse] 推送 %r → %d 个客户端" % (payload, n))


hub = EventHub()
hub_count = lambda: len(hub.queues)  # noqa: E731


# ------------------------------------------------------------------ 页面

PAGE_TMPL = """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>binUI dev · {title}</title>
<style>
  :root {{ --bg:#10141a; --panel:#171d26; --line:#232c39; --fg:#dfe6f0; --dim:#7d8b9e;
           --ok:#3fbf7f; --warn:#ffb648; --bad:#ff6b6b; }}
  * {{ box-sizing:border-box; }}
  html,body {{ margin:0; height:100%; background:var(--bg); color:var(--fg);
    font:13px/1.5 "Segoe UI","Microsoft YaHei",sans-serif; }}
  #bar {{ display:flex; align-items:center; gap:10px; padding:6px 12px;
    background:var(--panel); border-bottom:1px solid var(--line); height:40px; }}
  #bar b {{ font-weight:600; }}
  #bar .dim {{ color:var(--dim); }}
  #bar button {{ background:#243040; color:var(--fg); border:1px solid var(--line);
    border-radius:5px; padding:3px 10px; cursor:pointer; font:inherit; }}
  #bar button:hover {{ background:#2e3d51; }}
  #dot {{ width:8px; height:8px; border-radius:50%; background:var(--ok); }}
  #dot.off {{ background:var(--bad); }}
  #dot.wait {{ background:var(--warn); }}
  #spacer {{ flex:1; }}
  #main {{ display:flex; height:calc(100% - 40px); }}
  {panes}
  iframe {{ border:0; width:100%; height:100%; background:#fff; }}
  .pane {{ position:relative; min-width:0; height:100%; display:flex; flex-direction:column; }}
  .pane .h {{ padding:4px 10px; background:var(--panel); border-bottom:1px solid var(--line);
    color:var(--dim); font-size:12px; flex:0 0 auto; }}
  .pane .body {{ flex:1; min-height:0; position:relative; overflow:auto; }}
  #shotw {{ width:100%; height:100%; display:block; object-fit:contain; background:#0b0e13; }}
  #err {{ position:absolute; left:0; right:0; bottom:0; background:#3a1d1d; color:#ffd9d9;
    padding:6px 10px; font:12px/1.5 monospace; white-space:pre-wrap; display:none; }}
</style>
</head><body>
<div id="bar">
  <div id="dot"></div><b>{title}</b>
  <span class="dim" id="st">连接中…</span>
  <span id="spacer"></span>
  <span class="dim" id="beat"></span>
  <button id="re">重新生成</button>
  <button id="rf">刷新视图</button>
</div>
<div id="main">
{panes_html}
</div>
<div id="err"></div>
<script>
(function () {{
  var dot = document.getElementById('dot'), st = document.getElementById('st'),
      beat = document.getElementById('beat'), err = document.getElementById('err');
  var shot = document.getElementById('shot'), view = document.getElementById('view');
  var browser = document.getElementById('browser');
  var n = 0;
  function bust(u) {{ return u + (u.indexOf('?') < 0 ? '?' : '&') + 't=' + Date.now(); }}
  function showErr(msg) {{
    if (!msg) {{ err.style.display = 'none'; return; }}
    err.textContent = msg; err.style.display = 'block';
  }}
  function reloadAll(why) {{
    n++;
    if (view) view.src = bust('/view');
    if (shot) shot.src = bust('/shot.png');
    if (browser) browser.src = bust(browser.dataset.base || '/');
    beat.textContent = '第 ' + n + ' 次刷新 · ' + new Date().toLocaleTimeString() + (why ? ' · ' + why : '');
  }}
  var es = new EventSource('/events');
  if (/[?&]noreload\b/.test(location.search)) {{ es.close(); st.textContent = 'noreload：不连 SSE'; }}
  es.onopen = function () {{ dot.className = ''; st.textContent = '已连接，改源码保存即刷新'; }};
  es.onerror = function () {{
    dot.className = 'off'; st.textContent = 'SSE 断开，自动重连中…';
  }};
  es.onmessage = function (ev) {{
    var msg = (ev.data || '').trim();
    if (msg.indexOf('status ') === 0) {{ st.textContent = msg.slice(7); return; }}
    if (msg === 'reload') {{ dot.className = ''; st.textContent = '检测到改动，已重建'; reloadAll('文件变化'); }}
  }};
  document.getElementById('rf').onclick = function () {{ reloadAll('手动'); }};
  document.getElementById('re').onclick = function () {{
    st.textContent = '重建中…'; dot.className = 'wait';
    fetch('/api/rebuild').then(function (r) {{ return r.json(); }}).then(function (j) {{
      if (j.ok) {{ reloadAll('手动重建'); st.textContent = j.status; }}
      else {{ showErr(j.status); st.textContent = '重建失败'; dot.className = 'off'; }}
    }});
  }};
  st.textContent = '初始状态：{status}';
}})();
</script>
</body></html>
"""

PANES_BOTH = """  <div class="pane" style="flex:0 0 26%">
    <div class="h">结构查看器（draw_bench --dump-nodes → binui_nodes）</div>
    <div class="body"><iframe id="view" src="/view"></iframe></div>
  </div>
  <div class="pane" style="flex:1; border-left:1px solid var(--line)">
    <div class="h">bin2d_shot 真实渲染（像素权威）</div>
    <div class="body"><img id="shot" src="/shot.png" alt="shot"></div>
  </div>
  <div class="pane" style="flex:1; border-left:1px solid var(--line)">
    <div class="h">浏览器（源码本身 + binui-preview.js）
      <a id="srcopen" href="/file/{page_rel}" target="_blank">新窗口</a>
      <a id="rawopen" href="/file/{page_rel}?raw=1" target="_blank">无注入</a></div>
    <div class="body"><iframe id="browser" src="/file/{page_rel}"
                              data-base="/file/{page_rel}"></iframe></div>
  </div>"""

PANES_SHOT_ONLY = """  <div class="pane" style="flex:1">
    <div class="h">bin2d_shot 真实渲染（--shot-only）</div>
    <div class="body"><img id="shot" src="/shot.png" alt="shot"></div>
  </div>"""


# ------------------------------------------------------------------ HTTP

class DevServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False   # 端口占用要能被探测到（自动 +1）

    def handle_error(self, request, client_address):
        """浏览器刷新/断线是常态，别把 ConnectionAborted 打成整段 traceback。"""
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionAbortedError, ConnectionResetError, BrokenPipeError)):
            return
        log("[http] %s: %r" % (client_address[0], exc))


class Handler(BaseHTTPRequestHandler):
    server_version = "binui_dev/1.0"
    protocol_version = "HTTP/1.1"

    # 服务端上下文（main 里注入）
    page = None
    shot_only = False
    builder = None
    started_at = 0.0

    def log_message(self, fmt, *args):   # 安静：默认的每请求一行太吵
        pass

    def _send(self, code, body, ctype="text/html; charset=utf-8", extra=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store, max-age=0")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_HEAD(self):
        self.do_GET(head_only=True)

    def do_GET(self, head_only=False):
        parts = urlparse(self.path)
        path = parts.path
        if path == "/":
            self.builder.ensure()
            self._send(200, self._page())
        elif path == "/events":
            self._sse()
        elif path == "/view":
            if not os.path.exists(self.builder.view_html):
                self._send(404, "还没有生成查看器（先刷新一次）", "text/plain; charset=utf-8")
                return
            with open(self.builder.view_html, "rb") as f:
                data = f.read()
            self._send(200, data, "text/html; charset=utf-8")
        elif path == "/shot.png":
            if not os.path.exists(self.builder.png):
                self._send(404, "还没有生成 PNG", "text/plain; charset=utf-8")
                return
            with open(self.builder.png, "rb") as f:
                data = f.read()
            self._send(200, data, "image/png")
        elif path == "/__binui/preview.js":
            if not os.path.isfile(PREVIEW_JS):
                self._send(404, "缺少 tools/binui-preview.js", "text/plain; charset=utf-8")
                return
            with open(PREVIEW_JS, "rb") as f:
                data = f.read()
            self._send(200, data, "text/javascript; charset=utf-8")
        elif path == "/__binui/component":
            q = parse_qs(parts.query)
            rel, text, err = resolve_component((q.get("src") or [""])[0])
            payload = json.dumps({"ok": err is None, "path": rel, "text": text, "error": err},
                                 ensure_ascii=False)
            self._send(200, payload, "application/json; charset=utf-8")
        elif path.startswith("/__binui/c/"):
            self._send(200, COMPONENT_SHELL)
        elif path.startswith("/file/"):
            q = parse_qs(parts.query)
            self._serve_file(path[len("/file/"):], raw=bool(q.get("raw")))
        elif path == "/api/rebuild":
            ok, status = self.builder.ensure(force=True)
            if ok:
                hub.publish("reload")
            payload = json.dumps({"ok": ok, "status": status,
                                  "watch": [os.path.relpath(p, ROOT) for p in
                                            collect_watch_paths(self.page)],
                                  "builds": self.builder.builds,
                                  "clients": hub_count()}, ensure_ascii=False)
            self._send(200, payload, "application/json; charset=utf-8")
        elif path == "/api/state":
            payload = json.dumps({"ok": self.builder.ok, "status": self.builder.status,
                                  "builds": self.builder.builds, "clients": hub_count(),
                                  "page": self.page_rel(), "components": self.component_list(),
                                  "uptime": round(time.time() - self.started_at, 1)},
                                 ensure_ascii=False)
            self._send(200, payload, "application/json; charset=utf-8")
        elif path == "/favicon.ico":
            self._send(204, b"", "image/x-icon")
        else:
            self._send(404, "not found: " + path, "text/plain; charset=utf-8")

    def do_POST(self):
        self.do_GET()

    def page_rel(self):
        return os.path.relpath(self.page, ROOT).replace("\\", "/")

    def component_list(self):
        """页面用到的组件（带 .bum.html / .bum.xml 后缀的那些）—— 给独立视图做入口。"""
        out = []
        for p in collect_watch_paths(self.page):
            if p.lower().endswith((".bum.html", ".bum.xml")):
                out.append(os.path.relpath(p, ROOT).replace("\\", "/"))
        return out

    def _serve_file(self, rel, raw=False):
        """/file/<相对路径>：把仓库当静态站喂给浏览器；HTML 里注入 preview.js。"""
        p = safe_path(rel)
        if not p or not os.path.isfile(p):
            self._send(404, "not found: /file/" + rel, "text/plain; charset=utf-8")
            return
        ext = os.path.splitext(p)[1].lower()
        try:
            with open(p, "rb") as f:
                data = f.read()
        except OSError as exc:
            self._send(500, "读不到文件：%s" % exc, "text/plain; charset=utf-8")
            return
        if ext in (".html", ".htm") and not raw:
            data = inject_preview(data.decode("utf-8", "replace")).encode("utf-8")
        self._send(200, data, MIME.get(ext, "application/octet-stream"))

    def _page(self):
        title = os.path.relpath(self.page, ROOT).replace("\\", "/")
        panes = PANES_SHOT_ONLY if self.shot_only else PANES_BOTH.format(page_rel=self.page_rel())
        return PAGE_TMPL.format(title=title, panes="", panes_html=panes,
                                status=self.builder.status.replace('"', "&quot;"))

    def _sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        q = hub.subscribe()
        self.last_ping = time.time()
        try:
            self.wfile.write(b"retry: 1000\n\n")
            self.wfile.flush()
            log("[sse] 客户端接入（%d 个）" % hub_count())
            while True:
                try:
                    payload = q.pop(0) if q else None
                except IndexError:
                    payload = None
                if payload is not None:
                    data = payload.replace("\n", " ")
                    self.wfile.write(("data: %s\n\n" % data).encode("utf-8"))
                    self.wfile.flush()
                    continue
                time.sleep(0.2)          # 轮询队列（比阻塞 Queue 更容易在关服务时退出）
                if time.time() - self.last_ping >= SSE_PING_INTERVAL:
                    self.last_ping = time.time()
                    self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            hub.unsubscribe(q)
            log("[sse] 客户端断开（剩 %d 个）" % hub_count())


# ------------------------------------------------------------------ 监听线程

class Watcher(threading.Thread):
    def __init__(self, builder, stop_event):
        super().__init__(daemon=True)
        self.builder = builder
        self.stop_event = stop_event
        self.paths = collect_watch_paths(builder.page)
        self.last = signature(self.paths)

    def run(self):
        while not self.stop_event.is_set():
            time.sleep(WATCH_INTERVAL)
            self.setup_paths()
            sig = signature(self.paths)
            if sig == self.last:
                continue
            changed = []
            old = {p: (m, s) for p, m, s in self.last}
            for p, m, s in sig:
                if old.get(p) != (m, s):
                    changed.append(os.path.relpath(p, ROOT))
            self.last = sig
            log("[watch] 变化：%s" % ", ".join(changed))
            ok, status = self.builder.ensure(force=True)
            log("[watch] 重建 %s：%s" % ("成功" if ok else "失败", status))
            hub.publish("reload" if ok else "status " + status)

    def setup_paths(self):
        """组件清单可能变了（加了 <use>）→ 重新算监听集合。"""
        new_paths = collect_watch_paths(self.builder.page)
        if new_paths != self.paths:
            added = [p for p in new_paths if p not in self.paths]
            if added:
                log("[watch] 监听集合变化，新增 %s" % ", ".join(
                    os.path.relpath(p, ROOT) for p in added))
            self.paths = new_paths
            self.last = signature(self.paths)


# ------------------------------------------------------------------ 入口

def main(argv=None):
    # 控制台代码页可能是 936/437：统一按 UTF-8 输出，中文日志不乱码
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(
        prog="binui_dev.py",
        description="binUI 本地开发服务：结构查看器 + 真实渲染 + 保存即刷新")
    ap.add_argument("page", help="页面 html，例如 examples/buttonshow/main.html")
    ap.add_argument("--port", type=int, default=8765, help="起始端口（占用则 +1），默认 8765")
    ap.add_argument("--shot-only", action="store_true",
                    help="只出 bin2d_shot 的 PNG，不做结构查看器")
    ap.add_argument("--no-initial-build", action="store_true",
                    help="启动时先不重建（第一次请求再建）")
    args = ap.parse_args(argv)

    page = args.page
    if not os.path.isabs(page):
        page = os.path.join(os.getcwd(), page)
    if not os.path.isfile(page):
        print("找不到页面：%s" % args.page, file=sys.stderr)
        return 2
    page = os.path.normpath(page)
    for exe in (BIN2D_SHOT, DRAW_BENCH):
        if not os.path.isfile(exe):
            print("缺少二进制：%s（先 make tools）" % os.path.relpath(exe, ROOT), file=sys.stderr)
            return 2
    os.makedirs(OUT_DIR, exist_ok=True)

    builder = Builder(page, args.shot_only)

    # 端口：占用就 +1
    httpd = None
    port = args.port
    for attempt in range(MAX_PORT_TRIES):
        try:
            httpd = DevServer(("127.0.0.1", port), Handler)
            break
        except OSError as exc:
            # 10048 = WSAEADDRINUSE / 98 = EADDRINUSE
            if getattr(exc, "errno", None) in (98, 10048) or "in use" in str(exc).lower():
                log("端口 %d 被占用，改试 %d" % (port, port + 1))
                port += 1
                continue
            raise
    if httpd is None:
        print("连续 %d 个端口都被占用，放弃" % MAX_PORT_TRIES, file=sys.stderr)
        return 2

    httpd.daemon_threads = True
    Handler.page = page
    Handler.shot_only = args.shot_only
    Handler.builder = builder
    Handler.started_at = time.time()

    url = "http://127.0.0.1:%d/" % port
    print(url, flush=True)          # ← stdout 第一行：方便脚本抓 URL
    log.flush_after_url()
    log("页面 %s" % os.path.relpath(page, ROOT))
    log("产物 %s" % os.path.relpath(OUT_DIR, ROOT))
    log("端口 %d%s" % (port, "" if port == args.port else "（%d 被占用，自动 +%d）"
                       % (args.port, port - args.port)))
    if args.shot_only:
        log("模式：--shot-only（只有 PNG，没有结构查看器）")

    if not args.no_initial_build:
        ok, status = builder.ensure()
        log("首屏 %s：%s" % ("就绪" if ok else "失败", status))
    else:
        log("跳过首屏重建（--no-initial-build）")

    stop_event = threading.Event()
    watcher = Watcher(builder, stop_event)
    log("[watch] 监听 %d 个文件（每 %.0f s 轮询 mtime）：" % (len(watcher.paths), WATCH_INTERVAL))
    for p in watcher.paths:
        log("        " + os.path.relpath(p, ROOT))
    watcher.start()
    log("服务已起，Ctrl+C 退出")
    try:
        httpd.serve_forever(poll_interval=0.3)
    except KeyboardInterrupt:
        log("收到 Ctrl+C，退出")
    finally:
        stop_event.set()
        httpd.shutdown()
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
