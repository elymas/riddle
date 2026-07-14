fn main() {
    if std::env::var("CARGO_FEATURE_TAKEOVER").is_ok() {
        // Where libquill.so / libqsgepaper.so live at link time. Defaults match
        // the ferrari layout (sibling quill checkout); build-rm2.sh overrides
        // them for the reMarkable 2 shim.
        let quill = concat!(env!("CARGO_MANIFEST_DIR"), "/../quill");
        let quill_dir = std::env::var("QUILL_LIB_DIR").unwrap_or(format!("{quill}/build"));
        let vendor_dir = std::env::var("QSGEPAPER_LIB_DIR").unwrap_or(format!("{quill}/vendor"));
        println!("cargo:rustc-link-search=native={quill_dir}");
        println!("cargo:rustc-link-search=native={vendor_dir}");
        println!("cargo:rustc-link-lib=dylib=quill");
        println!("cargo:rustc-link-lib=dylib=qsgepaper");
        println!("cargo:rustc-link-arg=-Wl,-rpath,/home/root/quill:/usr/lib/plugins/scenegraph");
        // Resolve libquill's transitive Qt deps at link time from the SDK
        // sysroot. rpath-link only (NOT link-search: the SDK's libc/libm are
        // linker scripts with absolute paths that break outside --sysroot).
        let sysroot_lib = std::env::var("SDK_SYSROOT_LIB").ok().or_else(|| {
            std::env::var("HOME").ok().map(|home| {
                format!("{home}/rm-sdk-3.26/sysroots/cortexa53-crypto-remarkable-linux/usr/lib")
            })
        });
        if let Some(lib) = sysroot_lib {
            println!("cargo:rustc-link-arg=-Wl,-rpath-link,{lib}");
        }
    }
}
