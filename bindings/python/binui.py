"""binui - binUI 的 Python 绑定（ctypes，零第三方依赖）

binUI 引擎本身是 C++ 库；跨语言的那层是 include/binui_c.h 的 C ABI。
这个模块只做三件事：声明一遍 C 原型、把结构体映射成 ctypes、把
"句柄 + 轮询"包装成顺手用的类。

用法:
    import binui

    layout = binui.compile_source(open("ui.html", encoding="utf-8").read())
    with binui.App(layout) as app:
        app.open(title="Python 驱动")
        while app.pump():
            while (ev := app.poll()) is not None:
                if ev.type == binui.CLICK and app.text(ev.node_id) == "离开":
                    app.request_exit()

构建（先把 DLL 产出来）:
    mingw32-make SHELL=sh capi        # -> build-mingw/bin/binui_c.dll
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterator, Optional

ABI_VERSION = 1

# ---------------------------------------------------------------- 常量
CLICK, INPUT, HOVER, RESIZE, KEY, CHAR, WHEEL, CLOSE = range(1, 9)
EVENT_NAMES = {
    CLICK: "click", INPUT: "input", HOVER: "hover", RESIZE: "resize",
    KEY: "key", CHAR: "char", WHEEL: "wheel", CLOSE: "close",
}

ROOT, LABEL, IMAGE, BUTTON, INPUT_NODE, LIST, LINE, SVG, CANVAS, VIDEO = range(10)
NODE_TYPE_NAMES = {
    ROOT: "root", LABEL: "label", IMAGE: "image", BUTTON: "button",
    INPUT_NODE: "input", LIST: "list", LINE: "line", SVG: "svg",
    CANVAS: "canvas", VIDEO: "video",
}

NF_VISIBLE, NF_DISABLED, NF_INTERACTIVE = 1 << 0, 1 << 1, 1 << 2
NF_SCROLLABLE, NF_ABSOLUTE, NF_TITLEBAR = 1 << 3, 1 << 4, 1 << 5
NF_TEXT_CENTER, NF_TEXT_RIGHT = 1 << 6, 1 << 7

OPEN_RESIZABLE, OPEN_TRANSPARENT, OPEN_NO_VSYNC = 1 << 0, 1 << 1, 1 << 2

KEY_BACK, KEY_TAB, KEY_RETURN, KEY_ESCAPE = 0x08, 0x09, 0x0D, 0x1B
KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN = 0x25, 0x26, 0x27, 0x28
KEY_DELETE, KEY_HOME, KEY_END = 0x2E, 0x24, 0x23


class BinUIError(RuntimeError):
    """引擎侧返回失败时抛出（消息来自 binui_last_error()）。"""


# ---------------------------------------------------------------- 结构体
class CNode(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_int32),
        ("parent_id", ctypes.c_int32),
        ("type", ctypes.c_int32),
        ("flags", ctypes.c_int32),
        ("x", ctypes.c_float), ("y", ctypes.c_float),
        ("width", ctypes.c_float), ("height", ctypes.c_float),
        ("color", ctypes.c_uint32),
        ("image_index", ctypes.c_int32),
        ("font_size", ctypes.c_int32),
        ("padding", ctypes.c_int32),
        ("radius", ctypes.c_int32),
        ("z_index", ctypes.c_int32),
    ]


class CEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int32),
        ("node_id", ctypes.c_int32),
        ("x", ctypes.c_float), ("y", ctypes.c_float), ("dy", ctypes.c_float),
        ("key", ctypes.c_int32),
        ("text", ctypes.c_char_p),
        ("text_len", ctypes.c_int32),
        ("width", ctypes.c_int32), ("height", ctypes.c_int32),
    ]


# ---------------------------------------------------------------- 载入 DLL
def _candidates() -> Iterator[Path]:
    env = os.environ.get("BINUI_C_DLL")
    if env:
        yield Path(env)
    here = Path(__file__).resolve().parent
    names = ["binui_c.dll", "libbinui_c.dll"]
    for base in (here, here / "bin", here.parent, here.parent / "bin"):
        for n in names:
            yield base / n
    # 仓库内开发：从 bindings/python 往上找 build-mingw/bin
    for up in (here, here.parent, here.parent.parent, here.parent.parent.parent):
        for n in names:
            yield up / "build-mingw" / "bin" / n
            yield up / "build" / n


def _load() -> ctypes.CDLL:
    for p in _candidates():
        if p.is_file():
            return ctypes.CDLL(str(p), winmode=0)  # winmode=0：保留进程搜索路径
    found = ctypes.util.find_library("binui_c")
    if found:
        return ctypes.CDLL(found)
    raise BinUIError(
        "找不到 binui_c.dll —— 先构建（mingw32-make SHELL=sh capi）"
        "或用环境变量 BINUI_C_DLL 指定路径"
    )


lib = _load()

lib.binui_version.restype = ctypes.c_char_p
lib.binui_abi_version.restype = ctypes.c_int32
lib.binui_last_error.restype = ctypes.c_char_p
lib.binui_free.argtypes = [ctypes.c_void_p]

lib.binui_compile_file.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
lib.binui_compile_file.restype = ctypes.c_int32
lib.binui_compile_memory.argtypes = [ctypes.c_char_p, ctypes.c_int32,
                                     ctypes.POINTER(ctypes.c_void_p),
                                     ctypes.POINTER(ctypes.c_int32)]
lib.binui_compile_memory.restype = ctypes.c_int32

lib.binui_app_create.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_char_p]
lib.binui_app_create.restype = ctypes.c_void_p
lib.binui_app_open.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32,
                               ctypes.c_uint32, ctypes.c_char_p]
lib.binui_app_open.restype = ctypes.c_int32
lib.binui_app_open_child.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32,
                                     ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
                                     ctypes.c_uint32]
lib.binui_app_open_child.restype = ctypes.c_int32
lib.binui_render.argtypes = [ctypes.c_void_p]
lib.binui_render.restype = ctypes.c_int32
lib.binui_resize.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32]
lib.binui_resize.restype = None
lib.binui_app_hwnd.argtypes = [ctypes.c_void_p]
lib.binui_app_hwnd.restype = ctypes.c_void_p
lib.binui_pump.argtypes = [ctypes.c_void_p, ctypes.c_int32]
lib.binui_pump.restype = ctypes.c_int32
lib.binui_poll_event.argtypes = [ctypes.c_void_p, ctypes.POINTER(CEvent)]
lib.binui_poll_event.restype = ctypes.c_int32
lib.binui_app_destroy.argtypes = [ctypes.c_void_p]
lib.binui_app_destroy.restype = None

lib.binui_node_count.argtypes = [ctypes.c_void_p]
lib.binui_node_count.restype = ctypes.c_int32
lib.binui_node_at.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.POINTER(CNode)]
lib.binui_node_at.restype = ctypes.c_int32
lib.binui_node_by_id.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.POINTER(CNode)]
lib.binui_node_by_id.restype = ctypes.c_int32
lib.binui_find_node.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.binui_find_node.restype = ctypes.c_int32
lib.binui_node_text.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_char_p, ctypes.c_int32]
lib.binui_node_text.restype = ctypes.c_int32

lib.binui_set_text.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_char_p, ctypes.c_int32]
lib.binui_set_text.restype = ctypes.c_int32
lib.binui_set_visible.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32]
lib.binui_set_visible.restype = ctypes.c_int32
lib.binui_set_checked.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32]
lib.binui_set_checked.restype = ctypes.c_int32
lib.binui_invalidate_node.argtypes = [ctypes.c_void_p, ctypes.c_int32]
lib.binui_invalidate_node.restype = ctypes.c_int32
lib.binui_invalidate_all.argtypes = [ctypes.c_void_p]
lib.binui_invalidate_all.restype = None
lib.binui_hit_test.argtypes = [ctypes.c_void_p, ctypes.c_float, ctypes.c_float]
lib.binui_hit_test.restype = ctypes.c_int32
lib.binui_app_size.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int32),
                               ctypes.POINTER(ctypes.c_int32)]
lib.binui_app_size.restype = ctypes.c_int32

lib.binui_request_exit.argtypes = [ctypes.c_void_p]
lib.binui_request_exit.restype = None
lib.binui_minimize.argtypes = [ctypes.c_void_p]
lib.binui_minimize.restype = None
lib.binui_maximize.argtypes = [ctypes.c_void_p]
lib.binui_maximize.restype = None
lib.binui_begin_window_drag.argtypes = [ctypes.c_void_p]
lib.binui_begin_window_drag.restype = None

lib.binui_set_image_raw.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32,
                                    ctypes.c_int32, ctypes.c_void_p]
lib.binui_set_image_raw.restype = ctypes.c_int32
lib.binui_set_image.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_void_p, ctypes.c_int32]
lib.binui_set_image.restype = ctypes.c_int32
lib.binui_mount_components.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.binui_mount_components.restype = ctypes.c_int32


def version() -> str:
    return (lib.binui_version() or b"").decode("utf-8", "replace")


def last_error() -> str:
    return (lib.binui_last_error() or b"").decode("utf-8", "replace")


def _utf8(s: str) -> bytes:
    return s.encode("utf-8")


# ---------------------------------------------------------------- 数据类
@dataclass(frozen=True)
class Node:
    id: int
    parent_id: int
    type: int
    flags: int
    x: float
    y: float
    width: float
    height: float
    color: int
    image_index: int
    font_size: int
    padding: int
    radius: int
    z_index: int

    @property
    def type_name(self) -> str:
        return NODE_TYPE_NAMES.get(self.type, f"type{self.type}")

    @property
    def visible(self) -> bool:
        return bool(self.flags & NF_VISIBLE)

    @property
    def interactive(self) -> bool:
        return bool(self.flags & NF_INTERACTIVE)


@dataclass(frozen=True)
class Event:
    type: int
    node_id: int
    x: float
    y: float
    dy: float
    key: int
    text: str
    width: int
    height: int

    @property
    def type_name(self) -> str:
        return EVENT_NAMES.get(self.type, f"type{self.type}")


# ---------------------------------------------------------------- 编译
def compile_file(src: str | os.PathLike, out: str | os.PathLike) -> None:
    """把 .html / .xml 源码编成二进制布局（.bui）。"""
    if lib.binui_compile_file(_utf8(os.fspath(src)), _utf8(os.fspath(out))) != 1:
        raise BinUIError(last_error())


def compile_source(source: str) -> bytes:
    """把 UI 源码字符串编成布局字节（不必落盘）。"""
    data = ctypes.c_void_p()
    size = ctypes.c_int32()
    if lib.binui_compile_memory(_utf8(source), len(_utf8(source)),
                                ctypes.byref(data), ctypes.byref(size)) != 1:
        raise BinUIError(last_error())
    try:
        return ctypes.string_at(data, size.value)
    finally:
        lib.binui_free(data)


# ---------------------------------------------------------------- 应用
class App:
    """一个 binUI 窗口。同一线程创建、pump、销毁（Win32 窗口亲和）。"""

    def __init__(self, layout: bytes, asset_dir: str | os.PathLike | None = None):
        buf = ctypes.create_string_buffer(layout, len(layout))
        handle = lib.binui_app_create(buf, len(layout),
                                      _utf8(os.fspath(asset_dir)) if asset_dir else None)
        if not handle:
            raise BinUIError(last_error())
        self._app: Optional[int] = handle
        self._opened = False
        self._child = False

    # ---- 生命周期 ----
    def open(self, width: int = 0, height: int = 0, title: str = "binUI",
             flags: int = OPEN_RESIZABLE) -> "App":
        if lib.binui_app_open(self._h, width, height, flags, _utf8(title)) != 1:
            raise BinUIError(last_error())
        self._opened = True
        return self

    def pump(self, timeout_ms: int = 8) -> bool:
        """推进一帧；返回 False 表示窗口已关闭。"""
        return lib.binui_pump(self._h, timeout_ms) == 0

    def open_child(self, parent_hwnd: int, x: int, y: int, width: int, height: int,
                   flags: int = 0) -> "App":
        """以子窗口形式挂进宿主窗口（parent_hwnd）的客户区。

        和 open() 的差别：不碰进程 DPI 策略、不抢焦点、**不自己收消息** ——
        宿主 DispatchMessage 会把鼠标/键盘投递给本窗口，事件照旧从 poll() 出来。
        flags 只认 OPEN_NO_VSYNC（透明/可缩放对子窗口无意义）。
        """
        if lib.binui_app_open_child(self._h, parent_hwnd, x, y, width, height, flags) != 1:
            raise BinUIError(last_error())
        self._opened = True
        self._child = True
        return self

    def render(self) -> bool:
        """只出一帧（推进动画 + 绘制脏区），不碰宿主的消息队列。

        子窗口模式每帧调这个；返回 False 表示窗口已关闭（宿主销毁了父窗口）。
        """
        return lib.binui_render(self._h) == 0

    def resize(self, width: int, height: int) -> None:
        """宿主改了本区域尺寸时调用（等价于收到 WM_SIZE）。"""
        lib.binui_resize(self._h, width, height)

    @property
    def hwnd(self) -> int:
        """本窗口的 HWND（子窗口模式下宿主布局要用到）。"""
        return lib.binui_app_hwnd(self._h) or 0

    def poll(self) -> Optional[Event]:
        ev = CEvent()
        if lib.binui_poll_event(self._h, ctypes.byref(ev)) != 1:
            return None
        text = ev.text.decode("utf-8", "replace") if ev.text else ""
        return Event(ev.type, ev.node_id, ev.x, ev.y, ev.dy, ev.key, text,
                     ev.width, ev.height)

    def close(self) -> None:
        if self._app:
            lib.binui_app_destroy(self._app)
            self._app = None
            self._opened = False

    def __enter__(self) -> "App":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    @property
    def _h(self) -> int:
        if not self._app:
            raise BinUIError("app 已销毁")
        return self._app

    # ---- 事件循环 ----
    def run(self, on_event: Callable[["App", Event], None] | None = None,
            timeout_ms: int = 8) -> None:
        """标准主循环：pump 一帧，排空事件，交给回调。"""
        while self.pump(timeout_ms):
            while True:
                ev = self.poll()
                if ev is None:
                    break
                if on_event is not None:
                    on_event(self, ev)

    # ---- 查询 ----
    def nodes(self) -> list[Node]:
        out = []
        for i in range(lib.binui_node_count(self._h)):
            c = CNode()
            if lib.binui_node_at(self._h, i, ctypes.byref(c)):
                out.append(_node_of(c))
        return out

    def node(self, node_id: int) -> Optional[Node]:
        c = CNode()
        if not lib.binui_node_by_id(self._h, node_id, ctypes.byref(c)):
            return None
        return _node_of(c)

    def find(self, name: str) -> int:
        """按 HTML id / XML name 查节点 id；找不到返回 -1。"""
        return lib.binui_find_node(self._h, _utf8(name))

    def text(self, node_id: int) -> str:
        need = lib.binui_node_text(self._h, node_id, None, 0)
        if need < 0:
            return ""
        buf = ctypes.create_string_buffer(need + 1)
        lib.binui_node_text(self._h, node_id, buf, need + 1)
        return buf.value.decode("utf-8", "replace")

    def size(self) -> tuple[int, int]:
        w, h = ctypes.c_int32(), ctypes.c_int32()
        lib.binui_app_size(self._h, ctypes.byref(w), ctypes.byref(h))
        return w.value, h.value

    def hit_test(self, x: float, y: float) -> int:
        return lib.binui_hit_test(self._h, x, y)

    # ---- 修改 ----
    def set_text(self, node_id: int, text: str) -> None:
        data = _utf8(text)
        if lib.binui_set_text(self._h, node_id, data, len(data)) != 1:
            raise BinUIError(f"set_text 失败：节点 #{node_id} 不存在")

    def set_visible(self, node_id: int, visible: bool) -> None:
        lib.binui_set_visible(self._h, node_id, 1 if visible else 0)

    def set_checked(self, node_id: int, checked: bool) -> None:
        lib.binui_set_checked(self._h, node_id, 1 if checked else 0)

    def invalidate(self, node_id: int) -> None:
        lib.binui_invalidate_node(self._h, node_id)

    def invalidate_all(self) -> None:
        lib.binui_invalidate_all(self._h)

    # ---- 窗口 ----
    def minimize(self) -> None:
        lib.binui_minimize(self._h)

    def maximize(self) -> None:
        lib.binui_maximize(self._h)

    def request_exit(self) -> None:
        lib.binui_request_exit(self._h)

    # ---- 资源 ----
    def set_image_rgba(self, index: int, width: int, height: int, rgba: bytes) -> None:
        if len(rgba) != width * height * 4:
            raise ValueError("rgba 长度必须是 width*height*4")
        buf = ctypes.create_string_buffer(rgba, len(rgba))
        if lib.binui_set_image_raw(self._h, index, width, height, buf) != 1:
            raise BinUIError(last_error())

    def mount_components(self, layout_dir: str | os.PathLike | None = None) -> int:
        """挂载 <use ... external> 组件，返回挂载数量。"""
        d = _utf8(os.fspath(layout_dir)) if layout_dir else None
        n = lib.binui_mount_components(self._h, d)
        if n < 0:
            raise BinUIError(last_error())
        return n


def _node_of(c: CNode) -> Node:
    return Node(c.id, c.parent_id, c.type, c.flags, c.x, c.y, c.width, c.height,
                c.color, c.image_index, c.font_size, c.padding, c.radius, c.z_index)


if __name__ == "__main__":
    print(f"binui_c {version()}  abi={lib.binui_abi_version()}  python={sys.version.split()[0]}")
    if lib.binui_abi_version() != ABI_VERSION:
        print(f"警告：ABI 版本不一致（库 {lib.binui_abi_version()} / 绑定 {ABI_VERSION}）")
