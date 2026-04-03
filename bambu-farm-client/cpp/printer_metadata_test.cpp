#include "printer_metadata.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

static void test_json_string_field_extracts_values()
{
    const std::string json = "{\"dev_id\": \"TESTDEVICE123456\", \"dev_name\": \"A1 mini\"}";
    assert(BambuPlugin::json_string_field(json, "dev_id") == "TESTDEVICE123456");
    assert(BambuPlugin::json_string_field(json, "dev_name") == "A1 mini");
    assert(BambuPlugin::json_string_field(json, "missing").empty());
}

static void test_apply_name_override_rewrites_json()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "bambu-farm-printer-metadata-test-rewrite";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    BambuPlugin::PrinterMetadataStore store;
    store.set_name_override("TESTDEVICE123456", "Renamed Printer", root.string(), {});

    const std::string input = "{\"dev_id\": \"TESTDEVICE123456\", \"dev_name\": \"A1 mini\"}";
    const std::string output = store.apply_name_override(input);
    assert(output.find("\"dev_name\": \"Renamed Printer\"") != std::string::npos);
}

static void test_name_override_roundtrip_and_remove()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "bambu-farm-printer-metadata-test-roundtrip";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    BambuPlugin::PrinterMetadataStore writer;
    writer.set_name_override("TESTDEVICE123456", "Renamed Printer", root.string(), {});

    BambuPlugin::PrinterMetadataStore reader;
    reader.load(root.string(), {});

    const std::string input = "{\"dev_id\": \"TESTDEVICE123456\", \"dev_name\": \"A1 mini\"}";
    const std::string overridden = reader.apply_name_override(input);
    assert(overridden.find("\"dev_name\": \"Renamed Printer\"") != std::string::npos);
    assert(reader.json_matches_device(overridden, "TESTDEVICE123456"));

    reader.set_name_override("TESTDEVICE123456", "", root.string(), {});

    BambuPlugin::PrinterMetadataStore cleared;
    cleared.load(root.string(), {});
    const std::string cleared_json = cleared.apply_name_override(input);
    assert(cleared_json == input);
}

int main()
{
    test_json_string_field_extracts_values();
    test_apply_name_override_rewrites_json();
    test_name_override_roundtrip_and_remove();
    std::cout << "printer_metadata_test: ok\n";
    return 0;
}
