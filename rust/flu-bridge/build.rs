use cxx_qt_build::CxxQtBuilder;

fn main() {
    let builder = CxxQtBuilder::new().files(["src/recovery.rs", "src/ui_state.rs"]);
    // Only add the adapter header location; CXX-Qt owns its Qt/compiler flags.
    let builder = unsafe {
        builder.cc_builder(|compiler| {
            compiler.include("../../src");
            compiler.flag_if_supported("-Wno-sfinae-incomplete");
        })
    };
    builder.build();
    println!("cargo:rerun-if-changed=../../src/signingkeycontext.h");
    println!("cargo:rerun-if-changed=../../src/signingkeyrecovery.h");
}
