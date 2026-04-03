#include "print_flow.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    struct Recorder
    {
        std::vector<std::tuple<int, int, std::string>> statuses;
        std::vector<std::string> sent;
        std::vector<std::string> uploads;
        int local_context_updates{0};
    };

    Recorder *g_recorder = nullptr;

    void status_callback(int status, int code, std::string message)
    {
        g_recorder->statuses.emplace_back(status, code, std::move(message));
    }

    bool never_cancelled()
    {
        return false;
    }

    bool wait_ok(int, std::string)
    {
        return true;
    }
}

static BBL::PrintParams make_params()
{
    BBL::PrintParams params;
    params.dev_id = "TESTDEVICE123456";
    params.project_name = "Test Job";
    params.filename = "/tmp/test.3mf";
    params.dst_file = "";
    params.plate_index = 1;
    return params;
}

static void test_start_local_print_happy_path()
{
    Recorder recorder;
    g_recorder = &recorder;
    const auto params = make_params();

    BambuPlugin::PrintFlowDeps deps;
    deps.upload_file = [&](const std::string &, const std::string &, const std::string &remote) {
        recorder.uploads.push_back(remote);
        return BAMBU_NETWORK_SUCCESS;
    };
    deps.send_message = [&](const std::string &, const std::string &json) {
        recorder.sent.push_back(json);
        return BAMBU_NETWORK_SUCCESS;
    };
    deps.update_local_context = [&](const BBL::PrintParams &, const std::string &, const std::string &) {
        recorder.local_context_updates++;
    };
    deps.sleep_before_send = []() {};

    const int result = BambuPlugin::start_local_print(params, status_callback, never_cancelled, wait_ok, deps);
    assert(result == BAMBU_NETWORK_SUCCESS);
    assert(recorder.uploads.size() == 1);
    assert(recorder.local_context_updates == 1);
    assert(recorder.sent.size() == 1);
    assert(std::get<0>(recorder.statuses.front()) == PrintingStageCreate);
    assert(std::get<0>(recorder.statuses.back()) == PrintingStageFinished);
}

static void test_start_send_gcode_to_sdcard_upload_error()
{
    Recorder recorder;
    g_recorder = &recorder;
    const auto params = make_params();

    BambuPlugin::PrintFlowDeps deps;
    deps.upload_file = [&](const std::string &, const std::string &, const std::string &) {
        return -4;
    };

    const int result = BambuPlugin::start_send_gcode_to_sdcard(params, status_callback, deps);
    assert(result == BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED);
    assert(std::get<0>(recorder.statuses.back()) == PrintingStageERROR);
}

static void test_start_sdcard_print_cancelled()
{
    Recorder recorder;
    g_recorder = &recorder;
    auto params = make_params();
    params.dst_file = "Metadata/plate_1.gcode";

    BambuPlugin::PrintFlowDeps deps;
    deps.send_message = [&](const std::string &, const std::string &) {
        recorder.sent.push_back("unexpected");
        return BAMBU_NETWORK_SUCCESS;
    };

    const int result = BambuPlugin::start_sdcard_print(
        params,
        status_callback,
        []() { return true; },
        deps);

    assert(result == BAMBU_NETWORK_ERR_CANCELED);
    assert(recorder.sent.empty());
    assert(std::get<0>(recorder.statuses.back()) == PrintingStageERROR);
}

int main()
{
    test_start_local_print_happy_path();
    test_start_send_gcode_to_sdcard_upload_error();
    test_start_sdcard_print_cancelled();
    std::cout << "print_flow_test: ok\n";
    return 0;
}
