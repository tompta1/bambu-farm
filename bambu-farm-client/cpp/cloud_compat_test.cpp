#include "cloud_compat.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

static void test_unsupported_cloud_json()
{
    const std::string json = BambuPlugin::unsupported_cloud_json("publish");
    assert(json.find("\"error\":\"unsupported\"") != std::string::npos);
    assert(json.find("\"feature\":\"publish\"") != std::string::npos);
}

static void test_ensure_cloud_notice_file_writes_html()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "bambu-farm-cloud-compat-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const std::string url = BambuPlugin::ensure_cloud_notice_file(
        root.string(),
        "makerworld-home.html",
        "MakerWorld Not Supported",
        "MakerWorld browsing is currently disabled in the OSS LAN plugin."
    );

    assert(url.rfind("file://", 0) == 0);
    const std::filesystem::path path = root / "oss-cloud-notices" / "makerworld-home.html";
    assert(std::filesystem::exists(path));

    std::ifstream in(path);
    assert(in.is_open());
    const std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(html.find("MakerWorld Not Supported") != std::string::npos);
    assert(html.find("LAN discovery, connect/disconnect") != std::string::npos);
}

int main()
{
    test_unsupported_cloud_json();
    test_ensure_cloud_notice_file_writes_html();
    std::cout << "cloud_compat_test: ok\n";
    return 0;
}
