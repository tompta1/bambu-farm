#include "local_state.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

static void test_selected_device_roundtrip()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "bambu-farm-local-state-test-selected";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    BambuPlugin::persist_selected_device(root.string(), "TESTDEVICE123456", {});
    const std::string loaded = BambuPlugin::load_selected_device(root.string(), {});
    assert(loaded == "TESTDEVICE123456");
}

static void test_printer_name_overrides_roundtrip()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "bambu-farm-local-state-test-names";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::unordered_map<std::string, std::string> overrides{
        {"TESTDEVICE123456", "A1 mini"},
        {"TESTDEVICE123457", "X1C"},
    };

    BambuPlugin::persist_printer_name_overrides(root.string(), overrides, {});
    const auto loaded = BambuPlugin::load_printer_name_overrides(root.string(), {});
    assert(loaded == overrides);
}

static void test_local_print_context_roundtrip()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "bambu-farm-local-state-test-contexts";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    BambuPlugin::LocalPrintContext context;
    context.dev_id = "TESTDEVICE123456";
    context.project_id = "project-1";
    context.profile_id = "profile-1";
    context.subtask_id = "subtask-1";
    context.task_id = "task-1";
    context.remote_filename = "job.gcode.3mf";
    context.plate_path = "Metadata/plate_1.gcode";
    context.thumbnail_url = "file:///tmp/thumb.png";
    context.plate_index = 0;
    context.active = true;

    std::unordered_map<std::string, BambuPlugin::LocalPrintContext> contexts{
        {context.dev_id, context},
    };

    BambuPlugin::persist_local_print_contexts(root.string(), contexts, {});
    const auto loaded = BambuPlugin::load_local_print_contexts(root.string(), {});
    assert(loaded.size() == 1);
    const auto &roundtrip = loaded.at(context.dev_id);
    assert(roundtrip.project_id == context.project_id);
    assert(roundtrip.subtask_id == context.subtask_id);
    assert(roundtrip.thumbnail_url == context.thumbnail_url);
    assert(roundtrip.plate_index == context.plate_index);
    assert(roundtrip.active);
}

int main()
{
    test_selected_device_roundtrip();
    test_printer_name_overrides_roundtrip();
    test_local_print_context_roundtrip();
    std::cout << "local_state_test: ok\n";
    return 0;
}
