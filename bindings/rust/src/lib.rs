//! binUI 的 Rust 绑定：把 `include/binui_c.h` 的 C ABI 包成安全的类型。
//!
//! 设计与 C ABI 一致：**没有回调**，宿主自己 pump + poll。所以这里不需要
//! `Send`/`Sync` 的纠结 —— 但 Win32 窗口有线程亲和：`App` 定死在那一个线程上调，
//! 因此刻意**不实现** `Send`（编译期就挡住"丢进别的线程"）。
//!
//! ```no_run
//! use binui::App;
//!
//! let layout = binui::compile_str("<html width=\"300\" height=\"200\"><body>\
//!     <button id=\"ok\" style=\"left:20;top:20;width:120;height:40\">点我</button>\
//!     </body></html>").unwrap();
//! let app = App::new(&layout, None).unwrap();
//! app.open(0, 0, binui::OPEN_RESIZABLE, "Rust 驱动").unwrap();
//! while app.pump(8) {
//!     while let Some(ev) = app.poll() {
//!         if ev.kind == binui::EventKind::Click {
//!             println!("clicked node {}", ev.node_id);
//!             app.request_exit();
//!         }
//!     }
//! }
//! ```
//!
//! 构建前先在仓库根目录产出 C ABI：`mingw32-make SHELL=sh capi`。
//! 运行期需要 `binui_c.dll` 在 PATH 上或 exe 同目录。

use std::ffi::{c_char, c_int, c_void, CStr, CString};

pub const ABI: i32 = 1;

// 窗口选项
pub const OPEN_RESIZABLE: u32 = 1 << 0;
pub const OPEN_TRANSPARENT: u32 = 1 << 1;
pub const OPEN_NO_VSYNC: u32 = 1 << 2;

// 节点类型
pub const NODE_ROOT: i32 = 0;
pub const NODE_LABEL: i32 = 1;
pub const NODE_IMAGE: i32 = 2;
pub const NODE_BUTTON: i32 = 3;
pub const NODE_INPUT: i32 = 4;
pub const NODE_LIST: i32 = 5;
pub const NODE_LINE: i32 = 6;
pub const NODE_SVG: i32 = 7;
pub const NODE_CANVAS: i32 = 8;
pub const NODE_VIDEO: i32 = 9;

// 节点标志位
pub const NF_VISIBLE: u32 = 1 << 0;
pub const NF_DISABLED: u32 = 1 << 1;
pub const NF_INTERACTIVE: u32 = 1 << 2;
pub const NF_SCROLLABLE: u32 = 1 << 3;
pub const NF_ABSOLUTE: u32 = 1 << 4;
pub const NF_TITLEBAR: u32 = 1 << 5;

/// C 侧节点快照（定长，字段顺序必须与 binui_c.h 完全一致）
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct RawNode {
    pub id: i32,
    pub parent_id: i32,
    pub node_type: i32,
    pub flags: i32,
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
    pub color: u32,
    pub image_index: i32,
    pub font_size: i32,
    pub padding: i32,
    pub radius: i32,
    pub z_index: i32,
}

/// C 侧事件（text 指针在下一次 poll/pump 前有效）
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RawEvent {
    pub kind: i32,
    pub node_id: i32,
    pub x: f32,
    pub y: f32,
    pub dy: f32,
    pub key: i32,
    pub text: *const c_char,
    pub text_len: i32,
    pub width: i32,
    pub height: i32,
}

/// 事件类型（对应 BINUI_EV_*）
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EventKind {
    Click,
    Input,
    Hover,
    Resize,
    Key,
    Char,
    Wheel,
    Close,
    Other(i32),
}

impl EventKind {
    fn from_i32(v: i32) -> EventKind {
        match v {
            1 => EventKind::Click,
            2 => EventKind::Input,
            3 => EventKind::Hover,
            4 => EventKind::Resize,
            5 => EventKind::Key,
            6 => EventKind::Char,
            7 => EventKind::Wheel,
            8 => EventKind::Close,
            other => EventKind::Other(other),
        }
    }
}

/// 一次输入/状态变化
#[derive(Clone, Debug)]
pub struct Event {
    pub kind: EventKind,
    pub node_id: i32,
    pub x: f32,
    pub y: f32,
    pub dy: f32,
    pub key: i32,
    pub text: String,
    pub width: i32,
    pub height: i32,
}

/// 节点快照（带便捷判定）
#[derive(Clone, Copy, Debug)]
pub struct Node(pub RawNode);

impl Node {
    pub fn id(&self) -> i32 { self.0.id }
    pub fn parent_id(&self) -> i32 { self.0.parent_id }
    pub fn kind(&self) -> i32 { self.0.node_type }
    pub fn flags(&self) -> u32 { self.0.flags as u32 }
    pub fn rect(&self) -> (f32, f32, f32, f32) {
        (self.0.x, self.0.y, self.0.width, self.0.height)
    }
    pub fn color(&self) -> u32 { self.0.color }
    pub fn visible(&self) -> bool { self.flags() & NF_VISIBLE != 0 }
    pub fn interactive(&self) -> bool { self.flags() & NF_INTERACTIVE != 0 }
}

/// 错误：消息来自 `binui_last_error()`
#[derive(Debug, Clone)]
pub struct Error(pub String);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

type Result<T> = std::result::Result<T, Error>;

#[link(name = "binui_c")]
extern "C" {
    fn binui_version() -> *const c_char;
    fn binui_abi_version() -> i32;
    fn binui_last_error() -> *const c_char;
    fn binui_free(p: *mut c_void);

    fn binui_compile_file(src: *const c_char, out: *const c_char) -> i32;
    fn binui_compile_memory(
        src: *const c_char,
        len: i32,
        out: *mut *mut c_void,
        out_len: *mut i32,
    ) -> i32;

    fn binui_app_create(layout: *const c_void, len: i32, asset_dir: *const c_char) -> *mut c_void;
    fn binui_app_open(
        app: *mut c_void,
        w: i32,
        h: i32,
        flags: u32,
        title: *const c_char,
    ) -> i32;
    fn binui_pump(app: *mut c_void, timeout_ms: i32) -> i32;
    fn binui_app_open_child(
        app: *mut c_void,
        parent_hwnd: *mut c_void,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        flags: u32,
    ) -> i32;
    fn binui_render(app: *mut c_void) -> i32;
    fn binui_resize(app: *mut c_void, w: i32, h: i32);
    fn binui_app_hwnd(app: *mut c_void) -> *mut c_void;
    fn binui_poll_event(app: *mut c_void, out: *mut RawEvent) -> i32;
    fn binui_app_destroy(app: *mut c_void);

    fn binui_node_count(app: *mut c_void) -> i32;
    fn binui_node_at(app: *mut c_void, index: i32, out: *mut RawNode) -> i32;
    fn binui_node_by_id(app: *mut c_void, id: i32, out: *mut RawNode) -> i32;
    fn binui_find_node(app: *mut c_void, name: *const c_char) -> i32;
    fn binui_node_text(app: *mut c_void, id: i32, buf: *mut c_char, cap: i32) -> i32;

    fn binui_set_text(app: *mut c_void, id: i32, text: *const c_char, len: i32) -> i32;
    fn binui_set_visible(app: *mut c_void, id: i32, visible: i32) -> i32;
    fn binui_set_checked(app: *mut c_void, id: i32, on: i32) -> i32;
    fn binui_invalidate_node(app: *mut c_void, id: i32) -> i32;
    fn binui_invalidate_all(app: *mut c_void);
    fn binui_hit_test(app: *mut c_void, x: f32, y: f32) -> i32;
    fn binui_app_size(app: *mut c_void, w: *mut i32, h: *mut i32) -> i32;

    fn binui_request_exit(app: *mut c_void);
    fn binui_minimize(app: *mut c_void);
    fn binui_maximize(app: *mut c_void);
    fn binui_begin_window_drag(app: *mut c_void);

    fn binui_set_image_raw(
        app: *mut c_void,
        index: i32,
        w: i32,
        h: i32,
        rgba: *const c_void,
    ) -> i32;
    fn binui_set_image(app: *mut c_void, index: i32, data: *const c_void, len: i32) -> i32;
    fn binui_mount_components(app: *mut c_void, layout_dir: *const c_char) -> i32;
}

fn last_error() -> Error {
    Error(unsafe { CStr::from_ptr(binui_last_error()) }.to_string_lossy().into_owned())
}

/// 库版本串
pub fn version() -> String {
    unsafe { CStr::from_ptr(binui_version()).to_string_lossy().into_owned() }
}

/// 库的 ABI 版本（应与 [`ABI`] 相等）
pub fn abi_version() -> i32 {
    unsafe { binui_abi_version() }
}

/// 把 `&str` 转成临时 CString（内部不发 NUL）
fn cstr(s: &str) -> CString {
    CString::new(s.replace('\0', "")).expect("不含 NUL")
}

/// UI 源码文件 -> 二进制布局文件
pub fn compile_file(src: &str, out: &str) -> Result<()> {
    let (cs, co) = (cstr(src), cstr(out));
    if unsafe { binui_compile_file(cs.as_ptr(), co.as_ptr()) } == 1 {
        Ok(())
    } else {
        Err(last_error())
    }
}

/// UI 源码字符串 -> 布局字节
pub fn compile_str(html: &str) -> Result<Vec<u8>> {
    let src = cstr(html);
    let mut data: *mut c_void = std::ptr::null_mut();
    let mut len: i32 = 0;
    if unsafe { binui_compile_memory(src.as_ptr(), html.len() as i32, &mut data, &mut len) } != 1 {
        return Err(last_error());
    }
    let bytes = unsafe { std::slice::from_raw_parts(data as *const u8, len as usize) }.to_vec();
    unsafe { binui_free(data) };
    Ok(bytes)
}

/// 一个 binUI 窗口。
///
/// 刻意不实现 `Send`/`Sync`：Win32 窗口必须在创建它的线程上 pump。
pub struct App {
    handle: *mut c_void,
}

impl App {
    /// 加载布局字节（还没建窗）。`asset_dir` 是图片/外挂组件的基准目录。
    pub fn new(layout: &[u8], asset_dir: Option<&str>) -> Result<App> {
        if layout.is_empty() {
            return Err(Error("布局为空".into()));
        }
        let dir = asset_dir.map(cstr);
        let handle = unsafe {
            binui_app_create(
                layout.as_ptr() as *const c_void,
                layout.len() as i32,
                dir.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            )
        };
        if handle.is_null() {
            Err(last_error())
        } else {
            Ok(App { handle })
        }
    }

    /// 建窗口（w/h <= 0 时用布局自带的窗口描述）
    pub fn open(&self, w: i32, h: i32, flags: u32, title: &str) -> Result<()> {
        let ct = cstr(title);
        if unsafe { binui_app_open(self.handle, w, h, flags, ct.as_ptr()) } == 1 {
            Ok(())
        } else {
            Err(last_error())
        }
    }

    /// 推进一帧；`false` 表示窗口已关闭
    pub fn pump(&self, timeout_ms: i32) -> bool {
        unsafe { binui_pump(self.handle, timeout_ms) == 0 }
    }

    /// 以子窗口形式挂进宿主窗口的客户区（`WS_CHILD`）。
    ///
    /// 与 [`App::open`] 的差别：不碰进程 DPI 策略、不抢焦点、**不自己收消息** ——
    /// 宿主 `DispatchMessage` 会把鼠标/键盘投递给本窗口，事件照旧从 [`App::poll`] 出来。
    /// 所以子窗口模式每帧调 [`App::render`] 而不是 [`App::pump`]。
    /// 宿主窗口记得带 `WS_CLIPCHILDREN`（否则父窗口的 `WM_PAINT` 会把子窗口刷掉）。
    pub fn open_child(&self, parent: *mut c_void, x: i32, y: i32, w: i32, h: i32) -> Result<()> {
        if parent.is_null() {
            return Err(Error("parent HWND 为空".into()));
        }
        if w <= 0 || h <= 0 {
            return Err(Error("尺寸非法".into()));
        }
        if unsafe { binui_app_open_child(self.handle, parent, x, y, w, h, 0) } == 1 {
            Ok(())
        } else {
            Err(last_error())
        }
    }

    /// 只出一帧（推进动画 + 绘制脏区），不碰宿主的消息队列。
    /// `false` 表示窗口已关闭（宿主销毁了父窗口）。
    pub fn render(&self) -> bool {
        unsafe { binui_render(self.handle) == 0 }
    }

    /// 宿主改了本区域尺寸时调用（等价于收到 `WM_SIZE`）
    pub fn resize(&self, w: i32, h: i32) {
        unsafe { binui_resize(self.handle, w, h) };
    }

    /// 本窗口句柄（子窗口模式下宿主布局要用它；未开窗返回 null）
    pub fn hwnd(&self) -> *mut c_void {
        unsafe { binui_app_hwnd(self.handle) }
    }

    /// 取一个事件（队列空返回 None）
    pub fn poll(&self) -> Option<Event> {
        let mut raw = RawEvent {
            kind: 0,
            node_id: -1,
            x: 0.0,
            y: 0.0,
            dy: 0.0,
            key: 0,
            text: std::ptr::null(),
            text_len: 0,
            width: 0,
            height: 0,
        };
        if unsafe { binui_poll_event(self.handle, &mut raw) } != 1 {
            return None;
        }
        let text = if raw.text.is_null() || raw.text_len <= 0 {
            String::new()
        } else {
            let slice = unsafe {
                std::slice::from_raw_parts(raw.text as *const u8, raw.text_len as usize)
            };
            String::from_utf8_lossy(slice).into_owned()
        };
        Some(Event {
            kind: EventKind::from_i32(raw.kind),
            node_id: raw.node_id,
            x: raw.x,
            y: raw.y,
            dy: raw.dy,
            key: raw.key,
            text,
            width: raw.width,
            height: raw.height,
        })
    }

    /// 标准主循环：pump 一帧，排空事件，交给闭包
    pub fn run<F: FnMut(&App, Event)>(&self, timeout_ms: i32, mut f: F) {
        while self.pump(timeout_ms) {
            while let Some(ev) = self.poll() {
                f(self, ev);
            }
        }
    }

    /// 全部节点快照
    pub fn nodes(&self) -> Vec<Node> {
        let mut out = Vec::new();
        for i in 0..unsafe { binui_node_count(self.handle) } {
            let mut raw = RawNode::default();
            if unsafe { binui_node_at(self.handle, i, &mut raw) } == 1 {
                out.push(Node(raw));
            }
        }
        out
    }

    /// 按 id 取节点
    pub fn node(&self, id: i32) -> Option<Node> {
        let mut raw = RawNode::default();
        if unsafe { binui_node_by_id(self.handle, id, &mut raw) } == 1 {
            Some(Node(raw))
        } else {
            None
        }
    }

    /// 按 HTML id / XML name 查节点 id；找不到返回 -1
    pub fn find(&self, name: &str) -> i32 {
        let cn = cstr(name);
        unsafe { binui_find_node(self.handle, cn.as_ptr()) }
    }

    /// 取节点文本（节点不存在返回空串）
    pub fn text(&self, id: i32) -> String {
        let need = unsafe { binui_node_text(self.handle, id, std::ptr::null_mut(), 0) };
        if need <= 0 {
            return String::new();
        }
        let mut buf = vec![0i8 as c_char; need as usize + 1];
        unsafe { binui_node_text(self.handle, id, buf.as_mut_ptr(), need + 1) };
        let c = unsafe { CStr::from_ptr(buf.as_ptr()) };
        c.to_string_lossy().into_owned()
    }

    /// 改文本（会自动重绘该节点）
    pub fn set_text(&self, id: i32, text: &str) -> Result<()> {
        let ct = cstr(text);
        if unsafe { binui_set_text(self.handle, id, ct.as_ptr(), text.len() as i32) } == 1 {
            Ok(())
        } else {
            Err(Error(format!("set_text 失败：节点 #{id} 不存在")))
        }
    }

    pub fn set_visible(&self, id: i32, visible: bool) {
        unsafe { binui_set_visible(self.handle, id, visible as i32) };
    }

    pub fn set_checked(&self, id: i32, checked: bool) {
        unsafe { binui_set_checked(self.handle, id, checked as i32) };
    }

    pub fn invalidate(&self, id: i32) {
        unsafe { binui_invalidate_node(self.handle, id) };
    }

    pub fn invalidate_all(&self) {
        unsafe { binui_invalidate_all(self.handle) };
    }

    pub fn hit_test(&self, x: f32, y: f32) -> i32 {
        unsafe { binui_hit_test(self.handle, x, y) }
    }

    pub fn size(&self) -> (i32, i32) {
        let (mut w, mut h) = (0, 0);
        unsafe { binui_app_size(self.handle, &mut w, &mut h) };
        (w, h)
    }

    pub fn request_exit(&self) {
        unsafe { binui_request_exit(self.handle) };
    }

    pub fn minimize(&self) {
        unsafe { binui_minimize(self.handle) };
    }

    pub fn maximize(&self) {
        unsafe { binui_maximize(self.handle) };
    }

    pub fn begin_window_drag(&self) {
        unsafe { binui_begin_window_drag(self.handle) };
    }

    /// 用 RGBA8 像素替换图片槽（程序化纹理/视频帧）
    pub fn set_image_rgba(&self, index: i32, w: i32, h: i32, rgba: &[u8]) -> Result<()> {
        if (rgba.len() as i64) != (w as i64) * (h as i64) * 4 {
            return Err(Error("rgba 长度必须是 w*h*4".into()));
        }
        if unsafe {
            binui_set_image_raw(self.handle, index, w, h, rgba.as_ptr() as *const c_void)
        } == 1
        {
            Ok(())
        } else {
            Err(last_error())
        }
    }

    /// 用编码数据（PNG/JPEG）替换图片槽
    pub fn set_image(&self, index: i32, data: &[u8]) -> Result<()> {
        if unsafe {
            binui_set_image(self.handle, index, data.as_ptr() as *const c_void, data.len() as i32)
        } == 1
        {
            Ok(())
        } else {
            Err(last_error())
        }
    }

    /// 挂载 `<use ... external>` 记录的外挂组件，返回挂载数量
    pub fn mount_components(&self, layout_dir: Option<&str>) -> Result<i32> {
        let d = layout_dir.map(cstr);
        let n = unsafe {
            binui_mount_components(
                self.handle,
                d.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            )
        };
        if n < 0 {
            Err(last_error())
        } else {
            Ok(n)
        }
    }
}

impl Drop for App {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { binui_app_destroy(self.handle) };
            self.handle = std::ptr::null_mut();
        }
    }
}

#[allow(dead_code)]
fn _assert_layout() {
    // ABI 布局自检：与 tests/test_capi.cpp 的期望一致
    assert_eq!(std::mem::size_of::<RawNode>(), 56);
    assert_eq!(std::mem::offset_of!(RawNode, color), 32);
    assert_eq!(std::mem::offset_of!(RawNode, z_index), 52);
    assert_eq!(std::mem::size_of::<RawEvent>(), 48);
    assert_eq!(std::mem::offset_of!(RawEvent, text), 24);
    let _ = c_int::MIN;
}
