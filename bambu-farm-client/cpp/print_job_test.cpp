#include "print_job.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using BBL::PrintParams;

static std::filesystem::path write_test_file(const std::string &name, const std::string &contents)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bambu-farm-print-job-test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out.is_open());
    out << contents;
    out.close();
    return path;
}

static void test_resolve_remote_filename_for_print_package()
{
    PrintParams params;
    params.filename = write_test_file(".2.0.3mf", "swift-falcon-package-a").string();
    params.project_name = "Swift_Falcon";

    const std::string remote = BambuPlugin::resolve_remote_filename(params);
    assert(remote.find("Swift_Falcon-") == 0);
    assert(remote.size() > std::string("Swift_Falcon-.gcode.3mf").size());
    assert(remote.rfind(".gcode.3mf") == remote.size() - std::string(".gcode.3mf").size());
}

static void test_resolve_remote_filename_keeps_probe_name()
{
    PrintParams params;
    params.filename = "/app/share/BambuStudio/check_access_code.txt";
    params.project_name = "verify_job";

    const std::string remote = BambuPlugin::resolve_remote_filename(params);
    assert(remote == "check_access_code.txt");
}

static void test_resolve_remote_filename_sanitizes_hidden_temp_name()
{
    PrintParams params;
    params.filename = write_test_file(".2.0.3mf", "hidden-temp-package").string();

    const std::string remote = BambuPlugin::resolve_remote_filename(params);
    assert(remote.find("2.0-") == 0);
    assert(remote.rfind(".gcode.3mf") == remote.size() - std::string(".gcode.3mf").size());
}

static void test_resolve_remote_filename_matches_for_same_package_contents()
{
    PrintParams params_a;
    params_a.filename = write_test_file("session-a.3mf", "same-package-contents").string();
    params_a.project_name = "Swift_Falcon";

    PrintParams params_b;
    params_b.filename = write_test_file("session-b.3mf", "same-package-contents").string();
    params_b.project_name = "Swift_Falcon";

    const std::string remote_a = BambuPlugin::resolve_remote_filename(params_a);
    const std::string remote_b = BambuPlugin::resolve_remote_filename(params_b);

    assert(remote_a == remote_b);
}

static void test_resolve_remote_filename_differs_for_different_package_contents_same_path()
{
    const std::filesystem::path path = write_test_file("rewrite.3mf", "package-v1");

    PrintParams params;
    params.filename = path.string();
    params.project_name = "Swift_Falcon";
    const std::string remote_v1 = BambuPlugin::resolve_remote_filename(params);

    write_test_file("rewrite.3mf", "package-v2-different-contents");
    const std::string remote_v2 = BambuPlugin::resolve_remote_filename(params);

    assert(remote_v1 != remote_v2);
}

static void test_resolve_plate_metadata_path_is_one_based()
{
    PrintParams params;
    params.plate_index = 1;
    assert(BambuPlugin::resolve_plate_metadata_path(params) == "Metadata/plate_1.gcode");

    params.plate_index = 2;
    assert(BambuPlugin::resolve_plate_metadata_path(params) == "Metadata/plate_2.gcode");
}

static void test_resolve_plate_metadata_path_prefers_explicit_paths()
{
    PrintParams params;
    params.dst_file = "cards/print.gcode";
    assert(BambuPlugin::resolve_plate_metadata_path(params) == "cards/print.gcode");

    params.dst_file.clear();
    params.ftp_file = "Metadata/plate_7.gcode";
    assert(BambuPlugin::resolve_plate_metadata_path(params) == "Metadata/plate_7.gcode");
}

static void test_build_project_file_command()
{
    PrintParams params;
    params.plate_index = 1;
    params.project_name = "Swift Falcon";
    params.task_record_timelapse = false;
    params.task_bed_leveling = true;
    params.task_flow_cali = true;
    params.task_vibration_cali = false;
    params.task_layer_inspect = true;
    params.task_use_ams = true;
    params.ams_mapping = "[0,-1]";
    params.ams_mapping_info = "{\"source\":\"ui\"}";

    const std::string json = BambuPlugin::build_project_file_command(
        params,
        "Swift_Falcon.gcode.3mf",
        "Metadata/plate_1.gcode"
    );

    assert(json.find("\"command\":\"project_file\"") != std::string::npos);
    assert(json.find("\"param\":\"Metadata/plate_1.gcode\"") != std::string::npos);
    assert(json.find("\"subtask_name\":\"Swift Falcon\"") != std::string::npos);
    assert(json.find("\"url\":\"ftp://Swift_Falcon.gcode.3mf\"") != std::string::npos);
    assert(json.find("\"plate_idx\":0") != std::string::npos);
    assert(json.find("\"bed_leveling\":true") != std::string::npos);
    assert(json.find("\"use_ams\":true") != std::string::npos);
    assert(json.find("\"ams_mapping\":[0,-1]") != std::string::npos);
    assert(json.find("\"ams_mapping_info\":{\"source\":\"ui\"}") != std::string::npos);
}

static void test_resolve_display_subtask_name_prefers_project_name()
{
    PrintParams params;
    params.project_name = "SFERA Organic Geometric Planters & Pots";

    const std::string display = BambuPlugin::resolve_display_subtask_name(
        params,
        "SFERA_Organic_Geometric_Planters_&_Pots-deadbeef.gcode.3mf");

    assert(display == "SFERA Organic Geometric Planters & Pots");
}

static void test_resolve_display_subtask_name_strips_filename_suffix()
{
    PrintParams params;
    params.filename = "/tmp/session/example.3mf";

    const std::string display = BambuPlugin::resolve_display_subtask_name(
        params,
        "example-deadbeef.gcode.3mf");

    assert(display == "example");
}

static void test_build_logged_in_login_cmd()
{
    BambuPlugin::LoginState state;
    state.user_name = "LAN User";
    state.nickname = "LAN User";

    const std::string json = BambuPlugin::build_logged_in_login_cmd(state);
    assert(json.find("\"command\":\"studio_userlogin\"") != std::string::npos);
    assert(json.find("\"name\":\"LAN User\"") != std::string::npos);
    assert(json.find("\"avatar\":\"\"") != std::string::npos);
}

static void test_build_logout_cmd()
{
    const std::string json = BambuPlugin::build_logout_cmd("lan");
    assert(json == "{\"action\":\"logout\",\"provider\":\"lan\"}");
}

static void test_build_login_info()
{
    BambuPlugin::LoginState state;
    state.user_id = "00000000000000001";
    state.user_name = "LAN User";
    state.nickname = "LAN User";
    state.logged_in = true;

    const std::string json = BambuPlugin::build_login_info(
        state,
        "http://127.0.0.1:47403",
        "http://127.0.0.1:47403"
    );

    assert(json.find("\"user_id\":\"00000000000000001\"") != std::string::npos);
    assert(json.find("\"logged_in\":true") != std::string::npos);
    assert(json.find("\"access_token\":\"\"") != std::string::npos);
    assert(json.find("\"backend_url\":\"http://127.0.0.1:47403\"") != std::string::npos);
}

int main()
{
    test_resolve_remote_filename_for_print_package();
    test_resolve_remote_filename_keeps_probe_name();
    test_resolve_remote_filename_sanitizes_hidden_temp_name();
    test_resolve_remote_filename_matches_for_same_package_contents();
    test_resolve_remote_filename_differs_for_different_package_contents_same_path();
    test_resolve_plate_metadata_path_is_one_based();
    test_resolve_plate_metadata_path_prefers_explicit_paths();
    test_resolve_display_subtask_name_prefers_project_name();
    test_resolve_display_subtask_name_strips_filename_suffix();
    test_build_project_file_command();
    test_build_logged_in_login_cmd();
    test_build_logout_cmd();
    test_build_login_info();
    std::cout << "print_job_test: ok\n";
    return 0;
}
