#include "cloud_compat.hpp"
#include "print_job.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    std::string cloud_notice_root_dir(const std::string &config_dir)
    {
        if (!config_dir.empty()) {
            return config_dir + "/oss-cloud-notices";
        }

        const char *xdg = std::getenv("XDG_CONFIG_HOME");
        const char *home = std::getenv("HOME");
        if (xdg && *xdg) {
            return std::string(xdg) + "/BambuStudio/oss-cloud-notices";
        }
        if (home && *home) {
            return std::string(home) + "/.config/BambuStudio/oss-cloud-notices";
        }
        return "/tmp/bambu-oss-cloud-notices";
    }

    std::string html_escape(std::string value)
    {
        size_t pos = 0;
        while ((pos = value.find('&', pos)) != std::string::npos) {
            value.replace(pos, 1, "&amp;");
            pos += 5;
        }
        pos = 0;
        while ((pos = value.find('<', pos)) != std::string::npos) {
            value.replace(pos, 1, "&lt;");
            pos += 4;
        }
        pos = 0;
        while ((pos = value.find('>', pos)) != std::string::npos) {
            value.replace(pos, 1, "&gt;");
            pos += 4;
        }
        return value;
    }
}

namespace BambuPlugin
{
    std::string unsupported_cloud_json(const std::string &feature)
    {
        return
            "{"
            "\"error\":\"unsupported\","
            "\"feature\":\"" + json_escape(feature) + "\","
            "\"message\":\"This OSS plugin currently supports LAN printer workflows only. Cloud and MakerWorld features are disabled.\""
            "}";
    }

    std::string ensure_cloud_notice_file(
        const std::string &config_dir,
        const std::string &relative_path,
        const std::string &title,
        const std::string &message
    )
    {
        const std::filesystem::path file_path = std::filesystem::path(cloud_notice_root_dir(config_dir)) / relative_path;
        std::error_code ec;
        std::filesystem::create_directories(file_path.parent_path(), ec);
        if (ec) {
            return {};
        }

        std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return {};
        }

        out
            << "<!doctype html>\n"
            << "<html><head><meta charset=\"utf-8\">"
            << "<title>" << html_escape(title) << "</title>"
            << "<style>"
            << "body{font-family:sans-serif;max-width:48rem;margin:3rem auto;padding:0 1rem;line-height:1.5;color:#111;}"
            << "h1{font-size:1.5rem;}p,li{font-size:1rem;}code{background:#f3f3f3;padding:.1rem .3rem;border-radius:4px;}"
            << "</style></head><body>"
            << "<h1>" << html_escape(title) << "</h1>"
            << "<p>" << html_escape(message) << "</p>"
            << "<p>Supported today: LAN discovery, connect/disconnect, upload, print submission, storage browsing, timelapse download, and live camera.</p>"
            << "<p>Not supported: MakerWorld browsing, cloud login, cloud print history sync, and model publishing.</p>"
            << "</body></html>\n";
        out.close();

        return "file://" + file_path.generic_string();
    }
}
