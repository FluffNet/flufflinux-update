use cxx_qt_build::CxxQtBuilder;
fn main() {
    println!("cargo:rerun-if-env-changed=CXX_QT_EXPORT_DIR");
    println!("cargo:rerun-if-env-changed=CXX_QT_EXPORT_CRATE_flu_test_bridge");
    let builder = CxxQtBuilder::new().files(["src/recovery.rs"]);
    let builder = unsafe {
        builder.cc_builder(|compiler| {
            compiler.include("../../tests/support");
            compiler.flag_if_supported("-Wno-sfinae-incomplete");
        })
    };
    builder.build();
    println!("cargo:rerun-if-changed=../../tests/support/signingkeycontext.h");
    println!("cargo:rerun-if-changed=../../tests/support/signingkeyrecovery.h");
}
