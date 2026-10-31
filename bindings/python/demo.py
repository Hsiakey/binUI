"""binui Python 演示：整窗 UI 由 binUI 渲染，业务逻辑全在 Python 里。

跑之前先构建 C ABI（仓库根目录）:
    mingw32-make SHELL=sh capi
然后:
    python bindings/python/demo.py

界面在 demo_ui.html，改样式/加控件直接改它，不用动这个文件。
"""

from __future__ import annotations

import sys
from pathlib import Path

# 控制台可能是 GBK：不及时刷新看不到输出，而且中文/emoji 直接 print 会抛
# UnicodeEncodeError（Windows 中文环境默认 cp936）。演示优先稳定，用 UTF-8 + replace。
sys.stdout.reconfigure(line_buffering=True, encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import binui  # noqa: E402


def main() -> int:
    print(f"binui_c {binui.version()} / python {sys.version.split()[0]}")

    ui_path = HERE / "demo_ui.html"
    layout = binui.compile_source(ui_path.read_text(encoding="utf-8"))
    print(f"{ui_path.name} -> {len(layout)} 字节布局")

    with binui.App(layout, asset_dir=HERE) as app:
        app.open(title="binUI · Python 绑定")

        ids = {n: app.find(n) for n in
               ("count", "inc", "reset", "toggle", "quit", "name", "echo")}
        missing = [k for k, v in ids.items() if v < 0]
        if missing:
            print(f"警告：节点没找到 {missing}", file=sys.stderr)
        by_id = {v: k for k, v in ids.items() if v >= 0}

        state = {"count": 0, "on": False}

        def show_count() -> None:
            app.set_text(ids["count"], str(state["count"]))

        def on_event(a: binui.App, ev: binui.Event) -> None:
            if ev.type == binui.CLICK:
                who = by_id.get(ev.node_id, f"#{ev.node_id}")
                print(f"click: {who}")
                if ev.node_id == ids["inc"]:
                    state["count"] += 1
                    show_count()
                elif ev.node_id == ids["reset"]:
                    state["count"] = 0
                    show_count()
                elif ev.node_id == ids["toggle"]:
                    state["on"] = not state["on"]
                    a.set_checked(ids["toggle"], state["on"])
                    a.set_text(ids["toggle"], "已开" if state["on"] else "开关")
                elif ev.node_id == ids["quit"]:
                    a.request_exit()
            elif ev.type == binui.INPUT:
                print(f"input: {ev.text!r}")
                a.set_text(ids["echo"], f"输入框内容：{ev.text}" if ev.text else "…")
            elif ev.type == binui.CLOSE:
                print("窗口关闭")

        show_count()
        app.run(on_event)

    print("退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
