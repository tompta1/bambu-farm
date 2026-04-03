use std::fs::read_dir;

fn main() {
    // cc::Build::new()
    //     .file("cpp/api.cpp")
    //     .include("cpp")
    //     .link_lib_modifier("+whole-archive")
    //     .flag("-fvisibility=default")
    //     .flag("-export-all-symbols")
    //     .compile("bambu_networking_api");

    for entry in read_dir("cpp").unwrap() {
        println!(
            "cargo:rerun-if-changed=cpp/{}",
            entry.unwrap().file_name().to_str().unwrap()
        );
    }

    cxx_build::bridge("src/api.rs")
        .file("cpp/api.cpp")
        .file("cpp/callback_registry.cpp")
        .file("cpp/cloud_compat.cpp")
        .file("cpp/local_print_context.cpp")
        .file("cpp/local_state.cpp")
        .file("cpp/printer_metadata.cpp")
        .file("cpp/print_flow.cpp")
        .file("cpp/print_job.cpp")
        .file("cpp/session_state.cpp")
        .file("cpp/tunnel_bridge.cpp")
        .file("cpp/tunnel_protocol.cpp")
        .include("cpp")
        .flag_if_supported("-std=c++17")
        .flag_if_supported("-Wno-unused-parameter")
        // .link_lib_modifier("+whole-archive")
        .compile("bambu_networking_api");

    tonic_build::compile_protos("../bambu-farm-server/proto/service.proto").unwrap();
    println!("cargo:rerun-if-changed=../bambu-farm-server/proto/service.proto");
    println!("cargo:rerun-if-changed=build.rs");
}
