use cxx_qt_build::CxxQtBuilder;

fn main() {
    println!("cargo:rerun-if-env-changed=CXX_QT_EXPORT_DIR");
    println!("cargo:rerun-if-env-changed=CXX_QT_EXPORT_CRATE_flu_bridge");
    let builder = CxxQtBuilder::new().files(["src/backend.rs"]);
    // Only add the adapter header location; CXX-Qt owns its Qt/compiler flags.
    let builder = unsafe {
        builder.cc_builder(|compiler| {
            compiler.include("../../src");
            compiler.flag_if_supported("-Wno-sfinae-incomplete");
        })
    };
    builder.build();
    println!("cargo:rerun-if-changed=../../src/nativeqt.h");
}
