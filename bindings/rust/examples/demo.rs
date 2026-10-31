//! Rust 绑定演示：整窗 UI 由 binUI 渲染，业务逻辑全在 Rust 里。
//!
//! 先在仓库根目录产出 C ABI（`mingw32-make SHELL=sh capi`），然后：
//!
//! ```text
//! cd bindings/rust
//! cargo run --example demo
//! ```
//!
//! 运行期要有 binui_c.dll：PATH 上或 exe 同目录。

use binui::{App, EventKind};

const UI: &str = include_str!("../ui.html");

fn main() {
    println!("binui_c {} abi {}", binui::version(), binui::abi_version());
    if binui::abi_version() != binui::ABI {
        eprintln!("警告：ABI 版本不一致");
    }

    let layout = binui::compile_str(UI).expect("编译失败");
    println!("ui.html -> {} 字节布局", layout.len());

    let app = App::new(&layout, None).expect("加载失败");
    app.open(0, 0, binui::OPEN_RESIZABLE, "binUI · Rust 绑定")
        .expect("建窗失败");

    let find = |name: &str| app.find(name);
    let (id_count, id_inc, id_reset, id_toggle, id_quit, id_echo) = (
        find("count"),
        find("inc"),
        find("reset"),
        find("toggle"),
        find("quit"),
        find("echo"),
    );

    let mut count = 0i32;
    let mut on = false;

    app.run(8, |app, ev| match ev.kind {
        EventKind::Click => {
            if ev.node_id == id_inc {
                count += 1;
                let _ = app.set_text(id_count, &count.to_string());
                println!("click: inc -> {count}");
            } else if ev.node_id == id_reset {
                count = 0;
                let _ = app.set_text(id_count, "0");
                println!("click: reset");
            } else if ev.node_id == id_toggle {
                on = !on;
                app.set_checked(id_toggle, on);
                let _ = app.set_text(id_toggle, if on { "已开" } else { "开关" });
                println!("click: toggle -> {on}");
            } else if ev.node_id == id_quit {
                println!("click: quit");
                app.request_exit();
            } else {
                println!("click: node {}", ev.node_id);
            }
        }
        EventKind::Input => {
            println!("input: {:?}", ev.text);
            let echo = if ev.text.is_empty() {
                "…".to_string()
            } else {
                format!("输入框内容：{}", ev.text)
            };
            let _ = app.set_text(id_echo, &echo);
        }
        EventKind::Close => println!("窗口关闭"),
        _ => {}
    });

    println!("退出");
}
