#include "print_job.hpp"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace
{
    std::string to_lower_ascii(std::string value)
    {
        for (char &c : value) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return value;
    }

    bool ends_with_case_insensitive(const std::string &value, const std::string &suffix)
    {
        if (value.size() < suffix.size()) {
            return false;
        }
        return to_lower_ascii(value.substr(value.size() - suffix.size())) == to_lower_ascii(suffix);
    }

    std::string trim_ascii(std::string value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        return value;
    }

    bool looks_like_json_value(const std::string &value)
    {
        const std::string trimmed = trim_ascii(value);
        if (trimmed.empty()) {
            return false;
        }

        const char first = trimmed.front();
        if (first == '[' || first == '{' || first == '"' || first == '-' || (first >= '0' && first <= '9')) {
            return true;
        }
        return trimmed == "true" || trimmed == "false" || trimmed == "null";
    }

    void append_json_field(std::string &json, const std::string &key, const std::string &value, bool raw_json)
    {
        json += ",\"" + key + "\":";
        if (raw_json) {
            json += value;
        } else {
            json += "\"" + BambuPlugin::json_escape(value) + "\"";
        }
    }

    std::string append_package_suffix(const std::string &base_name, const std::string &suffix)
    {
        if (suffix.empty()) {
            return base_name;
        }

        if (ends_with_case_insensitive(base_name, ".gcode.3mf")) {
            const std::string stem = base_name.substr(0, base_name.size() - std::string(".gcode.3mf").size());
            return stem + "-" + suffix + ".gcode.3mf";
        }

        if (ends_with_case_insensitive(base_name, ".3mf")) {
            const std::string stem = base_name.substr(0, base_name.size() - std::string(".3mf").size());
            return stem + "-" + suffix + ".3mf";
        }

        return base_name + "-" + suffix;
    }

    std::string short_path_suffix(const std::string &path)
    {
        if (path.empty()) {
            return "";
        }

        uint32_t hash = 2166136261u;
        for (unsigned char ch : path) {
            hash ^= static_cast<uint32_t>(ch);
            hash *= 16777619u;
        }

        std::ostringstream out;
        out << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << hash;
        return out.str();
    }

    std::string short_content_suffix(const std::string &path)
    {
        if (path.empty()) {
            return "";
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return "";
        }

        uint32_t hash = 2166136261u;
        char buffer[8192];
        while (file.good()) {
            file.read(buffer, sizeof(buffer));
            const std::streamsize bytes_read = file.gcount();
            for (std::streamsize i = 0; i < bytes_read; ++i) {
                hash ^= static_cast<unsigned char>(buffer[i]);
                hash *= 16777619u;
            }
        }

        std::ostringstream out;
        out << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << hash;
        return out.str();
    }
}

namespace BambuPlugin
{
    std::string json_escape(const std::string &value)
    {
        std::string out;
        out.reserve(value.size());
        for (char c : value) {
            switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
            }
        }
        return out;
    }

    std::string basename_or_empty(const std::string &path)
    {
        if (path.empty()) {
            return "";
        }
        return std::filesystem::path(path).filename().string();
    }

    std::string sanitize_remote_filename(std::string value)
    {
        for (char &c : value) {
            switch (c) {
            case '/':
            case '\\':
            case ':':
            case '*':
            case '?':
            case '"':
            case '<':
            case '>':
            case '|':
                c = '_';
                break;
            default:
                break;
            }
        }

        while (!value.empty() && (value.front() == '.' || value.front() == ' ')) {
            value.erase(value.begin());
        }
        while (!value.empty() && value.back() == ' ') {
            value.pop_back();
        }

        if (value.empty()) {
            return "print.gcode.3mf";
        }
        return value;
    }

    std::string normalize_print_package_name(std::string value)
    {
        value = sanitize_remote_filename(std::move(value));
        if (ends_with_case_insensitive(value, ".gcode.3mf")) {
            return value;
        }
        if (ends_with_case_insensitive(value, ".3mf")) {
            value.resize(value.size() - 4);
        }
        return value + ".gcode.3mf";
    }

    std::string resolve_remote_filename(const BBL::PrintParams &params)
    {
        const std::string source_name = basename_or_empty(params.filename);
        const bool source_is_3mf =
            ends_with_case_insensitive(source_name, ".3mf") ||
            ends_with_case_insensitive(params.filename, ".3mf");
        const std::string package_suffix = [&params]() {
            const std::string content_suffix = short_content_suffix(params.filename);
            if (!content_suffix.empty()) {
                return content_suffix;
            }
            return short_path_suffix(params.filename);
        }();

        if (source_is_3mf) {
            if (!params.project_name.empty()) {
                return append_package_suffix(normalize_print_package_name(params.project_name), package_suffix);
            }
            if (!params.task_name.empty()) {
                return append_package_suffix(normalize_print_package_name(params.task_name), package_suffix);
            }
        }
        if (!params.ftp_file.empty()) {
            return sanitize_remote_filename(basename_or_empty(params.ftp_file));
        }
        if (!params.dst_file.empty()) {
            return sanitize_remote_filename(basename_or_empty(params.dst_file));
        }
        if (!params.task_name.empty()) {
            return sanitize_remote_filename(params.task_name);
        }
        if (!params.filename.empty()) {
            if (source_is_3mf) {
                return append_package_suffix(normalize_print_package_name(source_name), package_suffix);
            }
            return sanitize_remote_filename(source_name);
        }
        return "print.gcode.3mf";
    }

    std::string resolve_plate_metadata_path(const BBL::PrintParams &params)
    {
        if (!params.dst_file.empty()) {
            return params.dst_file;
        }
        if (!params.ftp_file.empty()) {
            return params.ftp_file;
        }
        const int plate_number = params.plate_index > 0 ? params.plate_index : 1;
        return "Metadata/plate_" + std::to_string(plate_number) + ".gcode";
    }

    std::string build_project_file_command(
        const BBL::PrintParams &params,
        const std::string &remote_filename,
        const std::string &plate_path
    )
    {
        std::string json =
            "{\"print\":{\"sequence_id\":0,\"command\":\"project_file\",\"param\":\"" + json_escape(plate_path) +
            "\",\"subtask_name\":\"" + json_escape(remote_filename) +
            "\",\"url\":\"ftp://" + json_escape(remote_filename) +
            "\",\"plate_idx\":" + std::to_string(params.plate_index > 0 ? params.plate_index - 1 : 0) +
            ",\"timelapse\":" + (params.task_record_timelapse ? "true" : "false") +
            ",\"bed_leveling\":" + (params.task_bed_leveling ? "true" : "false") +
            ",\"flow_cali\":" + (params.task_flow_cali ? "true" : "false") +
            ",\"vibration_cali\":" + (params.task_vibration_cali ? "true" : "false") +
            ",\"layer_inspect\":" + (params.task_layer_inspect ? "true" : "false") +
            ",\"use_ams\":" + (params.task_use_ams ? "true" : "false");

        if (!trim_ascii(params.ams_mapping).empty()) {
            append_json_field(json, "ams_mapping", trim_ascii(params.ams_mapping), looks_like_json_value(params.ams_mapping));
        }
        if (!trim_ascii(params.ams_mapping2).empty()) {
            append_json_field(json, "ams_mapping2", trim_ascii(params.ams_mapping2), looks_like_json_value(params.ams_mapping2));
        }
        if (!trim_ascii(params.ams_mapping_info).empty()) {
            append_json_field(json, "ams_mapping_info", trim_ascii(params.ams_mapping_info), looks_like_json_value(params.ams_mapping_info));
        }

        json += "}}";
        return json;
    }

    std::string build_logged_in_login_cmd(const LoginState &state)
    {
        const std::string display_name = !state.nickname.empty() ? state.nickname : state.user_name;
        return
            "{\"command\":\"studio_userlogin\",\"data\":{"
            "\"name\":\"" + json_escape(display_name) +
            "\",\"avatar\":\"" + json_escape(state.avatar) +
            "\"}}";
    }

    std::string build_logout_cmd(const std::string &provider)
    {
        return
            "{\"action\":\"logout\",\"provider\":\"" + json_escape(provider) + "\"}";
    }

    std::string build_login_info(const LoginState &state, const std::string &backend_url, const std::string &auth_url)
    {
        return
            "{"
            "\"user_id\":\"" + json_escape(state.user_id) +
            "\",\"user_name\":\"" + json_escape(state.user_name) +
            "\",\"nickname\":\"" + json_escape(state.nickname) +
            "\",\"avatar\":\"" + json_escape(state.avatar) +
            "\",\"logged_in\":" + (state.logged_in ? "true" : "false") +
            ",\"access_token\":\"\""
            ",\"refresh_token\":\"\""
            ",\"backend_url\":\"" + json_escape(backend_url) +
            "\",\"auth_url\":\"" + json_escape(auth_url) +
            "\"}";
    }
}
