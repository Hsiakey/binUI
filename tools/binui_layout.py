#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""binui_layout —— 布局属性对齐门禁：binUI 的节点盒 vs 浏览器元素的盒，逐元素比几何。

像素比（`pixel_diff`）说明"看起来差多少"，说不出"哪条属性不对"。这个工具走另一条路：
两端各导一份**几何表**，按元素比 x/y/w/h，差多少一目了然。

    python tools/binui_layout.py examples/appshow/main.html
    python tools/binui_layout.py examples/flexshow/main.html --tolerance 1.0 --json out/layout.json

两步数据来源（都已经有现成轮子，这里只是串起来）：
  · binUI 侧：`draw_bench --frames 1 --dump-nodes` 的节点表（编译期算好的盒）
  · 浏览器侧：把页面从 `binui_dev` 的 `/file/` 路由喂给 Edge headless，
    页面里的 `tools/binui-preview.js` 在 `?layout=1` 时把所有元素的盒写成
    `<pre id="binui-layout">`，用 `--dump-dom` 抓回来

匹配规则：先按元素 id 一一对应（探针页每个盒子都有 id），剩下的按几何值做贪心匹配
（±tolerance）。真实页面里 binUI 会有文字节点、浏览器会有纯文本包裹元素，数量本就不等 ——
所以结论看两个数：**按 id 匹配上的偏差**，和**几何能对上的比例**。

退出码：0 = 偏差都在 tolerance 内；1 = 有元素超差；2 = 环境问题（缺二进制 / 没 Edge / 页面读不到）。
"""

import argparse
import json
import os
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import binui_dev as dev            # noqa: E402  复用 /file 静态镜像 + 注入 preview.js

EDGE_CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]


def find_edge(explicit=None):
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    for p in EDGE_CANDIDATES:
        if os.path.isfile(p):
            return p
    return None


class StaticHost:
    """起一个只读的临时服务，复用 binui_dev 的 Handler（/file/** + /__binui/preview.js）。"""

    def __init__(self, port=0):
        dev.Handler.page = os.path.join(dev.ROOT, "examples", "appshow", "main.html")
        dev.Handler.builder = None
        dev.Handler.shot_only = False
        dev.Handler.started_at = time.time()
        self.httpd = ThreadingHTTPServer(("127.0.0.1", port), dev.Handler)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, kwargs={"poll_interval": 0.1},
                                       daemon=True)
        self.thread.start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def dump_binui(page, out_json):
    import subprocess
    exe = os.path.join(dev.BIN_DIR, "draw_bench.exe")
    if not os.path.isfile(exe):
        raise RuntimeError("缺少 %s（先 make tools）" % os.path.relpath(exe, dev.ROOT))
    proc = subprocess.run([exe, page, "--frames", "1", "--dump-nodes", out_json],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0 or not os.path.isfile(out_json):
        tail = proc.stdout.decode("utf-8", "replace").strip().splitlines()[-1:] or [""]
        raise RuntimeError("draw_bench --dump-nodes 失败（rc=%d）：%s"
                           % (proc.returncode, tail[0][:160]))
    with open(out_json, encoding="utf-8") as f:
        doc = json.load(f)
    out = []
    for n in doc.get("nodes", []):
        out.append(dict(name=(n.get("name") or "").strip(), type=n.get("type", ""),
                        x=float(n.get("x", 0)), y=float(n.get("y", 0)),
                        w=float(n.get("w", 0)), h=float(n.get("h", 0))))
    return out


def dump_browser(edge, port, page_rel, timeout=60):
    import subprocess
    url = "http://127.0.0.1:%d/file/%s?noreload=1&layout=1" % (port, page_rel)
    tmp = tempfile.mkdtemp(prefix="binui-layout-")
    proc = subprocess.run(
        [edge, "--headless", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
         "--virtual-time-budget=6000", "--user-data-dir=" + tmp, "--dump-dom", url],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=timeout)
    html = proc.stdout.decode("utf-8", "replace")
    marker = '<pre id="binui-layout"'
    at = html.find(marker)
    if at < 0:
        raise RuntimeError("浏览器里没有布局 JSON —— 页面可能没注入 preview.js，或脚本报错")
    start = html.find(">", at) + 1
    end = html.find("</pre>", start)
    raw = html[start:end]
    for a, b in (("&quot;", '"'), ("&lt;", "<"), ("&gt;", ">"), ("&amp;", "&")):
        raw = raw.replace(a, b)
    return json.loads(raw)


def compare(binui_nodes, browser_els, tol):
    by_id = {n["name"]: n for n in binui_nodes if n["name"]}
    id_rows, unmatched = [], []
    used = set()
    for b in browser_els:
        key = b["id"]
        n = by_id.get(key) if not key.startswith("#") else None
        if n is None:
            unmatched.append(b)
            continue
        used.add(id(n))
        id_rows.append((key, n, b))
    free_nodes = [n for n in binui_nodes if id(n) not in used]

    rows = []
    for key, n, b in id_rows:
        dx, dy = n["x"] - b["x"], n["y"] - b["y"]
        dw, dh = n["w"] - b["w"], n["h"] - b["h"]
        rows.append((key, dx, dy, dw, dh, max(abs(dx), abs(dy), abs(dw), abs(dh))))

    # 几何贪心匹配：给真实页面用（元素没 id）
    pool = list(unmatched)
    matched = 0
    for n in free_nodes:
        best, bestd = None, None
        for b in pool:
            d = max(abs(n["x"] - b["x"]), abs(n["y"] - b["y"]),
                    abs(n["w"] - b["w"]), abs(n["h"] - b["h"]))
            if d <= tol and (bestd is None or d < bestd):
                best, bestd = b, d
        if best is not None:
            pool.remove(best)
            matched += 1
    return rows, matched, len(free_nodes), pool


def main(argv=None):
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(prog="binui_layout.py",
                                 description="布局属性对齐：binUI 节点盒 vs 浏览器元素盒")
    ap.add_argument("page", help="页面 html，例如 examples/appshow/main.html")
    ap.add_argument("--tolerance", type=float, default=1.0, help="允许偏差（像素），默认 1.0")
    ap.add_argument("--edge", help="msedge.exe 路径（默认自动找）")
    ap.add_argument("--json", help="把逐元素结果写成 JSON")
    args = ap.parse_args(argv)

    page = os.path.abspath(args.page)
    if not os.path.isfile(page):
        print("找不到页面：%s" % args.page, file=sys.stderr)
        return 2
    edge = find_edge(args.edge)
    if not edge:
        print("找不到 msedge.exe（用 --edge 指定）", file=sys.stderr)
        return 2

    page_rel = os.path.relpath(page, dev.ROOT).replace("\\", "/")
    host = StaticHost()
    try:
        tmp_json = os.path.join(tempfile.mkdtemp(prefix="binui-nodes-"), "nodes.json")
        nodes = dump_binui(page, tmp_json)
        els = dump_browser(edge, host.port, page_rel)
    except RuntimeError as exc:
        print("失败：%s" % exc, file=sys.stderr)
        return 2
    finally:
        host.close()

    rows, matched, free_total, leftover = compare(nodes, els, args.tolerance)
    known = [r for r in rows if r[0].startswith("known-")]
    real = [r for r in rows if not r[0].startswith("known-")]
    over = [r for r in real if r[5] > args.tolerance]

    print("页面 %s" % page_rel)
    print("binUI 节点 %d · 浏览器元素 %d" % (len(nodes), len(els)))
    print()
    if real:
        print("%-24s %8s %8s %8s %8s" % ("id", "Δx", "Δy", "Δw", "Δh"))
        print("-" * 60)
        for key, dx, dy, dw, dh, worst in sorted(real, key=lambda r: -r[5]):
            if worst <= 0.01:
                continue
            print("%-24s %8.2f %8.2f %8.2f %8.2f" % (key, dx, dy, dw, dh))
        if not over:
            print("（全部在 %.1f px 内）" % args.tolerance)
        print()
    if known:
        print("已知语义差异（不计入退出码，见 docs/issues/15 §4）：")
        for key, dx, dy, dw, dh, worst in sorted(known, key=lambda r: -r[5]):
            if worst <= args.tolerance:
                continue
            print("  %-24s Δx %+8.2f Δy %+8.2f Δw %+8.2f Δh %+8.2f" % (key, dx, dy, dw, dh))
        print()
    print("有 id 的元素 %d 个（其中已知差异 %d 个），超差 %d 个；"
          "没有 id 的 binUI 节点 %d 个，几何能对上 %d 个（±%.1f px）"
          % (len(rows), len(known), len(over), free_total, matched, args.tolerance))
    if real:
        bands = [(0.1, 0), (0.5, 0), (1.0, 0), (2.0, 0), (8.0, 0), (1e9, 0)]
        for key, dx, dy, dw, dh, worst in real:
            for i, (lim, _) in enumerate(bands):
                if worst <= lim:
                    bands[i] = (lim, bands[i][1] + 1)
                    break
        print("偏差分布：" + " · ".join(
            ["≤%.1fpx %d" % (lim if lim < 1e9 else 999, n) for lim, n in bands[:-1]] +
            ["更大 %d" % bands[-1][1]]))
        worst_rows = sorted(real, key=lambda r: -r[5])[:5]
        if worst_rows and worst_rows[0][5] > 0.5:
            print("最大的几个：")
            for key, dx, dy, dw, dh, worst in worst_rows:
                if worst <= 0.5:
                    break
                print("  %-24s Δx %+7.2f Δy %+7.2f Δw %+7.2f Δh %+7.2f" % (key, dx, dy, dw, dh))
    print("（对上不了的多半是文字节点 / 伪元素 / 窗口根，浏览器里没有对应的盒）")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"page": page_rel,
                       "id_rows": [dict(id=k, dx=dx, dy=dy, dw=dw, dh=dh, worst=w)
                                   for k, dx, dy, dw, dh, w in rows],
                       "matched_no_id": matched, "no_id_nodes": free_total,
                       "tolerance": args.tolerance}, f, ensure_ascii=False, indent=1)
        print("逐元素结果：%s" % args.json)

    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main())
