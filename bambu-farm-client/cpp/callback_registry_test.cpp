#include "callback_registry.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

static void test_ssdp_replay_and_refresh()
{
    BambuPlugin::CallbackRegistry registry;
    registry.remember_printer_json("printer-a", "{\"dev_id\":\"printer-a\"}");

    std::vector<std::string> seen;
    int refresh_count = 0;
    registry.set_ssdp_callback(
        [&](std::string json) { seen.push_back(std::move(json)); },
        {},
        [&]() { refresh_count++; });

    assert(seen.size() == 1);
    assert(seen.front().find("printer-a") != std::string::npos);
    assert(refresh_count == 1);
}

static void test_connected_and_disconnected_callbacks()
{
    BambuPlugin::CallbackRegistry registry;

    std::string printer_connected;
    std::string local_message;
    int local_status = -1;

    registry.set_printer_connected_callback(
        [&](std::string dev_id) { printer_connected = std::move(dev_id); },
        "printer-a",
        {});
    registry.set_local_connect_callback(
        [&](int status, std::string dev_id, std::string msg) {
            local_status = status;
            local_message = dev_id + ":" + msg;
        },
        "printer-a",
        {});

    assert(printer_connected == "printer-a");
    assert(local_status == BBL::ConnectStatusOk);
    assert(local_message == "printer-a:Connected");

    registry.dispatch_disconnected(BBL::ConnectStatusFailed, "printer-a", "Disconnected", {}, true);
    assert(local_status == BBL::ConnectStatusFailed);
    assert(local_message == "printer-a:Disconnected");
}

static void test_message_dispatch()
{
    BambuPlugin::CallbackRegistry registry;
    std::string message;
    std::string local_message;
    registry.set_message_callback([&](std::string dev_id, std::string msg) {
        message = dev_id + ":" + msg;
    });
    registry.set_local_message_callback([&](std::string dev_id, std::string msg) {
        local_message = dev_id + ":" + msg;
    });
    assert(registry.has_message_callbacks());
    registry.dispatch_message("printer-a", "{\"print\":{}}", {});
    assert(message == "printer-a:{\"print\":{}}");
    assert(local_message == "printer-a:{\"print\":{}}");
}

int main()
{
    test_ssdp_replay_and_refresh();
    test_connected_and_disconnected_callbacks();
    test_message_dispatch();
    std::cout << "callback_registry_test: ok\n";
    return 0;
}
