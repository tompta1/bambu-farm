#include "local_print_context.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

static BambuPlugin::LocalPrintContext make_context()
{
    BambuPlugin::LocalPrintContext context;
    context.dev_id = "printer-a";
    context.project_id = "project-1";
    context.profile_id = "profile-1";
    context.subtask_id = "subtask-1";
    context.task_id = "task-1";
    context.remote_filename = "job.gcode.3mf";
    context.plate_path = "Metadata/plate_1.gcode";
    context.thumbnail_url = "file:///tmp/thumb.png";
    context.plate_index = 0;
    context.active = true;
    return context;
}

static void test_lookup_and_clear()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "bambu-farm-local-print-context-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    BambuPlugin::LocalPrintContextStore store;
    const auto context = make_context();
    store.update(context, root.string(), {});

    BambuPlugin::LocalPrintContext loaded;
    assert(store.lookup_subtask_id("subtask-1", loaded));
    assert(loaded.project_id == "project-1");

    store.clear("printer-a", root.string(), {});
    assert(!store.lookup_subtask_id("subtask-1", loaded));
}

static void test_rewrite_status_message()
{
    BambuPlugin::LocalPrintContextStore store;
    store.update(make_context(), std::filesystem::temp_directory_path().string(), {});

    const std::string input = "{\"print\":{\"gcode_state\":\"RUNNING\"}}";
    const std::string rewritten = store.rewrite_status_message("printer-a", input);
    assert(rewritten.find("\"project_id\":\"project-1\"") != std::string::npos);
    assert(rewritten.find("\"subtask_id\":\"subtask-1\"") != std::string::npos);
    assert(rewritten.find("\"ipcam\":{\"file\":{\"local\":\"local\"") != std::string::npos);
}

static void test_build_subtask_info()
{
    BambuPlugin::LocalPrintContextStore store;
    store.update(make_context(), std::filesystem::temp_directory_path().string(), {});

    std::string task_json;
    std::string http_body;
    unsigned int http_code = 0;
    assert(store.build_subtask_info("subtask-1", &task_json, &http_code, &http_body));
    assert(http_code == 200);
    assert(http_body == "{}");
    assert(task_json.find("\"thumbnail\":{\"url\":\"file:///tmp/thumb.png\"}") != std::string::npos);

    task_json.clear();
    http_body.clear();
    http_code = 0;
    assert(!store.build_subtask_info("missing", &task_json, &http_code, &http_body));
    assert(http_code == 404);
    assert(http_body == "{\"error\":\"not_found\"}");
    assert(task_json.empty());
}

int main()
{
    test_lookup_and_clear();
    test_rewrite_status_message();
    test_build_subtask_info();
    std::cout << "local_print_context_test: ok\n";
    return 0;
}
