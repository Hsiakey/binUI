// 告诉 rustc 去哪找 binui_c：优先环境变量 BINUI_C_LIB_DIR，
// 否则用仓库内的 build-mingw/bin（从仓库根 mingw32-make capi 产出）。
use std::path::PathBuf;

fn main() {
    let dir = std::env::var("BINUI_C_LIB_DIR").map(PathBuf::from).unwrap_or_else(|_| {
        let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
        manifest.join("../../build-mingw/bin")
    });
    let dir = dir.canonicalize().unwrap_or(dir);
    println!("cargo:rustc-link-search=native={}", dir.display());
    println!("cargo:rustc-link-lib=dylib=binui_c");
    println!("cargo:rerun-if-env-changed=BINUI_C_LIB_DIR");
}
