#include "api.hpp"
#include "print_job.hpp"
#include "stdio.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <execinfo.h>
#include <mutex>
#include <memory>
#include <fstream>
#include <filesystem>
#include <regex>
#include <thread>
#include <unordered_map>

#include "bambu-farm-client/src/api.rs.h"

#define LOG_CALL_ARGS(FORMAT, ...) {\
    char _log_macro_buf[1024];\
    snprintf(_log_macro_buf, 1024, "%s (Line %d): " FORMAT "\n", __FUNCTION__, __LINE__, __VA_ARGS__);\
    bambu_network_rs_log_debug(std::string(_log_macro_buf));\
    }
#define LOG_CALL() {\
    char _log_macro_buf[1024];\
    snprintf(_log_macro_buf, 1024, "%s (Line %d)\n", __FUNCTION__, __LINE__);\
    bambu_network_rs_log_debug(std::string(_log_macro_buf));\
    }

static OnMsgArrivedFn on_msg_arrived;
static OnLocalConnectedFn on_local_connect;
static OnMessageFn on_message;
static OnMessageFn on_mqtt_message;
static OnMessageFn on_user_message;
static OnPrinterConnectedFn on_printer_connected;
static OnServerConnectedFn on_server_connected;
static OnServerErrFn on_server_error;
static OnUserLoginFn on_user_login;
static GetSubscribeFailureFn on_subscribe_failure;
static std::string selected_device;
static std::string config_dir_path;
static std::string ca_path;
static size_t user_login_log_counter = 0;
static size_t build_login_cmd_log_counter = 0;
static const char *kLanBackendUrl = "http://127.0.0.1:47403";
static std::atomic<bool> lan_user_logged_in{true};
static std::atomic<bool> auto_connect_pending{false};
static std::atomic<bool> printer_connected{false};
static std::atomic<bool> connect_in_progress{false};
static std::mutex printer_name_overrides_mutex;
static std::unordered_map<std::string, std::string> printer_name_overrides;
static std::unordered_map<std::string, std::string> last_printer_json_by_id;
static std::mutex local_print_context_mutex;
static void persist_local_print_contexts();
static std::string last_tunnel_error;
static std::string preview_payload(const std::string &message, size_t limit = 1024);
static constexpr size_t kTunnelDownloadChunkSize = 4 * 1024 * 1024;

struct CompatTunnel
{
    std::string url;
    std::string device_id;
    Logger logger{nullptr};
    void *logger_context{nullptr};
    bool opened{false};
    bool control_stream_started{false};
    std::mutex mutex;
    std::deque<std::string> queued_responses;
    std::string active_response;
};

struct LocalPrintContext
{
    std::string dev_id;
    std::string project_id;
    std::string profile_id;
    std::string subtask_id;
    std::string task_id;
    std::string remote_filename;
    std::string plate_path;
    std::string thumbnail_url;
    int plate_index{-1};
    bool active{false};
};

static std::unordered_map<std::string, LocalPrintContext> local_print_contexts;

static void write_diag(const std::string &message);

static CompatTunnel *as_compat_tunnel(Bambu_Tunnel tunnel)
{
    return reinterpret_cast<CompatTunnel *>(tunnel);
}

static std::string bambu_query_param(const std::string &url, const std::string &key)
{
    const std::string needle = key + "=";
    const size_t query_pos = url.find('?');
    size_t pos = url.find(needle, query_pos == std::string::npos ? 0 : query_pos + 1);
    if (pos == std::string::npos) {
        return {};
    }
    pos += needle.size();
    const size_t end = url.find('&', pos);
    return url.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

static void compat_tunnel_log(CompatTunnel *tunnel, const std::string &message)
{
    write_diag(message);
    if (!tunnel || !tunnel->logger) {
        return;
    }
    char *copy = static_cast<char *>(std::malloc(message.size() + 1));
    if (!copy) {
        return;
    }
    std::memcpy(copy, message.c_str(), message.size());
    copy[message.size()] = '\0';
    tunnel->logger(tunnel->logger_context, 0, copy);
}

static std::string json_escape(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (ch < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                escaped += buf;
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return escaped;
}

static bool extract_json_integer(const std::string &json, const char *key, std::uint64_t &value)
{
    const std::regex pattern(std::string("\"") + key + "\":([0-9]+)");
    std::smatch match;
    if (!std::regex_search(json, match, pattern) || match.size() < 2) {
        return false;
    }
    value = std::strtoull(match[1].str().c_str(), nullptr, 10);
    return true;
}

static std::string extract_json_string(const std::string &json, const char *key)
{
    const std::regex pattern(std::string("\"") + key + "\":\"([^\"]*)\"");
    std::smatch match;
    if (!std::regex_search(json, match, pattern) || match.size() < 2) {
        return {};
    }
    return match[1].str();
}

static std::vector<std::string> split_file_download_response_frames(const std::string &response_bytes)
{
    const size_t separator = response_bytes.find("\n\n");
    if (separator == std::string::npos) {
        return {response_bytes};
    }

    const std::string header = response_bytes.substr(0, separator);
    const std::string payload = response_bytes.substr(separator + 2);
    if (payload.size() <= kTunnelDownloadChunkSize) {
        return {response_bytes};
    }

    std::uint64_t sequence = 0;
    std::uint64_t total = static_cast<std::uint64_t>(payload.size());
    if (!extract_json_integer(header, "sequence", sequence)) {
        return {response_bytes};
    }
    extract_json_integer(header, "total", total);

    const std::string file_md5 = extract_json_string(header, "file_md5");
    const std::string ftp_file_md5 = extract_json_string(header, "ftp_file_md5");
    const std::string path = extract_json_string(header, "path");

    std::vector<std::string> frames;
    frames.reserve((payload.size() + kTunnelDownloadChunkSize - 1) / kTunnelDownloadChunkSize);
    for (size_t offset = 0; offset < payload.size(); offset += kTunnelDownloadChunkSize) {
        const size_t chunk_size = std::min(kTunnelDownloadChunkSize, payload.size() - offset);
        const int result = (offset + chunk_size) < payload.size() ? 1 : 0;
        std::string frame_header =
            "{\"result\":" + std::to_string(result) +
            ",\"sequence\":" + std::to_string(sequence) +
            ",\"reply\":{\"size\":" + std::to_string(chunk_size) +
            ",\"offset\":" + std::to_string(offset) +
            ",\"total\":" + std::to_string(total) +
            ",\"file_md5\":\"" + json_escape(file_md5) +
            "\",\"ftp_file_md5\":\"" + json_escape(ftp_file_md5) +
            "\",\"path\":\"" + json_escape(path) + "\"}}";
        std::string frame;
        frame.reserve(frame_header.size() + 2 + chunk_size);
        frame += frame_header;
        frame += "\n\n";
        frame.append(payload.data() + offset, chunk_size);
        frames.emplace_back(std::move(frame));
    }
    return frames;
}

static std::string selected_device_state_path()
{
    if (!config_dir_path.empty()) {
        return config_dir_path + "/oss-last-device.txt";
    }

    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    if (xdg && *xdg) {
        return std::string(xdg) + "/BambuStudio/oss-last-device.txt";
    }
    if (home && *home) {
        return std::string(home) + "/.config/BambuStudio/oss-last-device.txt";
    }
    return "/tmp/bambu-oss-last-device.txt";
}

static std::string printer_names_state_path()
{
    if (!config_dir_path.empty()) {
        return config_dir_path + "/oss-printer-names.tsv";
    }

    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    if (xdg && *xdg) {
        return std::string(xdg) + "/BambuStudio/oss-printer-names.tsv";
    }
    if (home && *home) {
        return std::string(home) + "/.config/BambuStudio/oss-printer-names.tsv";
    }
    return "/tmp/bambu-oss-printer-names.tsv";
}

static std::string local_thumbnail_cache_dir()
{
    if (!config_dir_path.empty()) {
        return config_dir_path + "/oss-thumbnails";
    }

    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    if (xdg && *xdg) {
        return std::string(xdg) + "/BambuStudio/oss-thumbnails";
    }
    if (home && *home) {
        return std::string(home) + "/.config/BambuStudio/oss-thumbnails";
    }
    return "/tmp/bambu-oss-thumbnails";
}

static std::string local_print_contexts_state_path()
{
    if (!config_dir_path.empty()) {
        return config_dir_path + "/oss-local-print-contexts.tsv";
    }

    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    if (xdg && *xdg) {
        return std::string(xdg) + "/BambuStudio/oss-local-print-contexts.tsv";
    }
    if (home && *home) {
        return std::string(home) + "/.config/BambuStudio/oss-local-print-contexts.tsv";
    }
    return "/tmp/bambu-oss-local-print-contexts.tsv";
}

static std::string synthetic_local_id(const char *prefix, const std::string &dev_id, const std::string &remote_filename)
{
    std::hash<std::string> hasher;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string(prefix) + "-" + std::to_string(hasher(dev_id + "|" + remote_filename + "|" + std::to_string(now)));
}

static bool find_json_string_value_range(const std::string &json, const std::string &key, size_t &value_start, size_t &value_end)
{
    const std::string needle = "\"" + key + "\": \"";
    const size_t key_pos = json.find(needle);
    if (key_pos == std::string::npos) {
        return false;
    }

    value_start = key_pos + needle.size();
    value_end = value_start;
    bool escaped = false;
    while (value_end < json.size()) {
        const char c = json[value_end];
        if (!escaped && c == '"') {
            return true;
        }
        if (!escaped && c == '\\') {
            escaped = true;
        } else {
            escaped = false;
        }
        value_end++;
    }
    return false;
}

static std::string json_string_field(const std::string &json, const std::string &key)
{
    size_t value_start = 0;
    size_t value_end = 0;
    if (!find_json_string_value_range(json, key, value_start, value_end)) {
        return "";
    }
    return json.substr(value_start, value_end - value_start);
}

static void clear_local_print_context(const std::string &dev_id)
{
    if (dev_id.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(local_print_context_mutex);
        local_print_contexts.erase(dev_id);
    }
    persist_local_print_contexts();
}

static void persist_local_print_contexts()
{
    const std::string path = local_print_contexts_state_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        write_diag("persist_local_print_contexts: failed path=" + path);
        return;
    }

    std::lock_guard<std::mutex> lock(local_print_context_mutex);
    for (const auto &[dev_id, context] : local_print_contexts) {
        if (!context.active || dev_id.empty()) {
            continue;
        }
        out
            << context.dev_id << '\t'
            << context.project_id << '\t'
            << context.profile_id << '\t'
            << context.subtask_id << '\t'
            << context.task_id << '\t'
            << context.remote_filename << '\t'
            << context.plate_path << '\t'
            << context.thumbnail_url << '\t'
            << context.plate_index << '\n';
    }
    write_diag("persist_local_print_contexts: wrote path=" + path);
}

static void load_local_print_contexts()
{
    const std::string path = local_print_contexts_state_path();
    std::ifstream in(path);
    if (!in.is_open()) {
        write_diag("load_local_print_contexts: no state path=" + path);
        return;
    }

    std::unordered_map<std::string, LocalPrintContext> loaded;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> fields;
        size_t start = 0;
        while (true) {
            const size_t tab = line.find('\t', start);
            if (tab == std::string::npos) {
                fields.push_back(line.substr(start));
                break;
            }
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        if (fields.size() != 9 || fields[0].empty()) {
            continue;
        }

        LocalPrintContext context;
        context.dev_id = fields[0];
        context.project_id = fields[1];
        context.profile_id = fields[2];
        context.subtask_id = fields[3];
        context.task_id = fields[4];
        context.remote_filename = fields[5];
        context.plate_path = fields[6];
        context.thumbnail_url = fields[7];
        try {
            context.plate_index = std::stoi(fields[8]);
        } catch (...) {
            context.plate_index = 0;
        }
        context.active = true;
        loaded[context.dev_id] = context;
    }

    {
        std::lock_guard<std::mutex> lock(local_print_context_mutex);
        local_print_contexts = std::move(loaded);
    }
    write_diag("load_local_print_contexts: loaded path=" + path);
}

static void update_local_print_context(
    const BBL::PrintParams &params,
    const std::string &remote_filename,
    const std::string &plate_path)
{
    LocalPrintContext context;
    context.dev_id = params.dev_id;
    context.project_id = synthetic_local_id("local-project", params.dev_id, remote_filename);
    context.profile_id = synthetic_local_id("local-profile", params.dev_id, remote_filename);
    context.subtask_id = synthetic_local_id("local-subtask", params.dev_id, remote_filename);
    context.task_id = context.subtask_id;
    context.remote_filename = remote_filename;
    context.plate_path = plate_path;
    context.plate_index = params.plate_index > 0 ? params.plate_index - 1 : 0;
    rust::String thumbnail_url = bambu_network_rs_extract_3mf_thumbnail(
        params.filename,
        params.plate_index,
        local_thumbnail_cache_dir());
    context.thumbnail_url = std::string(thumbnail_url.c_str(), thumbnail_url.size());
    context.active = true;

    {
        std::lock_guard<std::mutex> lock(local_print_context_mutex);
        local_print_contexts[params.dev_id] = context;
    }
    persist_local_print_contexts();

    write_diag(
        "update_local_print_context: dev_id=" + context.dev_id +
        " project_id=" + context.project_id +
        " profile_id=" + context.profile_id +
        " subtask_id=" + context.subtask_id +
        " plate_index=" + std::to_string(context.plate_index) +
        " thumbnail_url=" + context.thumbnail_url
    );
}

static bool lookup_local_print_context_by_subtask_id(const std::string &subtask_id, LocalPrintContext &out)
{
    std::lock_guard<std::mutex> lock(local_print_context_mutex);
    for (const auto &[dev_id, context] : local_print_contexts) {
        if (context.active && context.subtask_id == subtask_id) {
            out = context;
            return true;
        }
    }
    return false;
}

static std::string rewrite_local_print_status_message(const std::string &device_id, const std::string &message)
{
    const auto inject_file_capability = [](const std::string &input) -> std::string {
        const size_t print_key_pos = input.find("\"print\"");
        if (print_key_pos == std::string::npos) {
            return input;
        }
        const size_t print_object_start = input.find('{', print_key_pos);
        if (print_object_start == std::string::npos) {
            return input;
        }

        const size_t ipcam_key_pos = input.find("\"ipcam\"", print_object_start);
        if (ipcam_key_pos != std::string::npos) {
            const size_t ipcam_object_start = input.find('{', ipcam_key_pos);
            if (ipcam_object_start != std::string::npos) {
                const size_t ipcam_object_end = input.find('}', ipcam_object_start);
                if (ipcam_object_end != std::string::npos) {
                    const std::string ipcam_body =
                        input.substr(ipcam_object_start, ipcam_object_end - ipcam_object_start);
                    if (ipcam_body.find("\"file\"") == std::string::npos) {
                        std::string rewritten = input;
                        rewritten.insert(
                            ipcam_object_start + 1,
                            "\"file\":{\"local\":\"local\",\"remote\":\"none\",\"model_download\":\"disabled\"},");
                        return rewritten;
                    }
                }
            }
            return input;
        }

        std::string rewritten = input;
        rewritten.insert(
            print_object_start + 1,
            "\"ipcam\":{\"file\":{\"local\":\"local\",\"remote\":\"none\",\"model_download\":\"disabled\"}},");
        return rewritten;
    };

    const std::string capability_message = inject_file_capability(message);
    LocalPrintContext context;
    {
        std::lock_guard<std::mutex> lock(local_print_context_mutex);
        const auto it = local_print_contexts.find(device_id);
        if (it == local_print_contexts.end() || !it->second.active) {
            return capability_message;
        }
        context = it->second;
    }

    const size_t print_key_pos = capability_message.find("\"print\"");
    if (print_key_pos == std::string::npos) {
        return capability_message;
    }
    const size_t object_start = capability_message.find('{', print_key_pos);
    if (object_start == std::string::npos) {
        return capability_message;
    }

    const bool has_project_id = capability_message.find("\"project_id\"", print_key_pos) != std::string::npos;
    const bool has_profile_id = capability_message.find("\"profile_id\"", print_key_pos) != std::string::npos;
    const bool has_subtask_id = capability_message.find("\"subtask_id\"", print_key_pos) != std::string::npos;
    const bool has_task_id = capability_message.find("\"task_id\"", print_key_pos) != std::string::npos;
    if (has_project_id && has_profile_id && has_subtask_id && has_task_id) {
        return capability_message;
    }

    std::string injected;
    if (!has_project_id) {
        injected += "\"project_id\":\"" + BambuPlugin::json_escape(context.project_id) + "\",";
    }
    if (!has_profile_id) {
        injected += "\"profile_id\":\"" + BambuPlugin::json_escape(context.profile_id) + "\",";
    }
    if (!has_subtask_id) {
        injected += "\"subtask_id\":\"" + BambuPlugin::json_escape(context.subtask_id) + "\",";
    }
    if (!has_task_id) {
        injected += "\"task_id\":\"" + BambuPlugin::json_escape(context.task_id) + "\",";
    }
    if (injected.empty()) {
        return message;
    }

    std::string rewritten = capability_message;
    rewritten.insert(object_start + 1, injected);
    return rewritten;
}

static std::string apply_printer_name_override(const std::string &json)
{
    const std::string dev_id = json_string_field(json, "dev_id");
    if (dev_id.empty()) {
        return json;
    }

    std::string override_name;
    {
        std::lock_guard<std::mutex> lock(printer_name_overrides_mutex);
        const auto it = printer_name_overrides.find(dev_id);
        if (it == printer_name_overrides.end() || it->second.empty()) {
            return json;
        }
        override_name = it->second;
    }

    size_t value_start = 0;
    size_t value_end = 0;
    if (!find_json_string_value_range(json, "dev_name", value_start, value_end)) {
        return json;
    }

    std::string rewritten = json;
    rewritten.replace(value_start, value_end - value_start, BambuPlugin::json_escape(override_name));
    return rewritten;
}

static void persist_printer_name_overrides()
{
    const std::string path = printer_names_state_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        write_diag("persist_printer_name_overrides: failed path=" + path);
        return;
    }

    std::lock_guard<std::mutex> lock(printer_name_overrides_mutex);
    for (const auto &[dev_id, name] : printer_name_overrides) {
        if (!dev_id.empty() && !name.empty()) {
            out << dev_id << '\t' << name << '\n';
        }
    }
    write_diag("persist_printer_name_overrides: wrote path=" + path);
}

static void load_printer_name_overrides()
{
    const std::string path = printer_names_state_path();
    std::ifstream in(path);
    if (!in.is_open()) {
        write_diag("load_printer_name_overrides: no state path=" + path);
        return;
    }

    std::unordered_map<std::string, std::string> loaded;
    std::string line;
    while (std::getline(in, line)) {
        const size_t tab_pos = line.find('\t');
        if (tab_pos == std::string::npos) {
            continue;
        }
        const std::string dev_id = line.substr(0, tab_pos);
        const std::string name = line.substr(tab_pos + 1);
        if (!dev_id.empty() && !name.empty()) {
            loaded[dev_id] = name;
        }
    }

    {
        std::lock_guard<std::mutex> lock(printer_name_overrides_mutex);
        printer_name_overrides = std::move(loaded);
    }
    write_diag("load_printer_name_overrides: loaded path=" + path);
}

static void persist_selected_device()
{
    const std::string path = selected_device_state_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    if (selected_device.empty()) {
        std::filesystem::remove(path, ec);
        write_diag("persist_selected_device: cleared path=" + path);
        return;
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        write_diag("persist_selected_device: failed path=" + path);
        return;
    }
    out << selected_device;
    write_diag("persist_selected_device: wrote dev_id=" + selected_device + " path=" + path);
}

static void load_selected_device()
{
    const std::string path = selected_device_state_path();
    std::ifstream in(path);
    if (!in.is_open()) {
        write_diag("load_selected_device: no state path=" + path);
        return;
    }

    std::string loaded;
    std::getline(in, loaded);
    if (loaded.empty()) {
        write_diag("load_selected_device: empty state path=" + path);
        return;
    }

    selected_device = loaded;
    auto_connect_pending.store(true);
    write_diag("load_selected_device: dev_id=" + selected_device + " path=" + path);
}

static bool printer_json_matches_selected_device(const std::string &json)
{
    if (selected_device.empty()) {
        return false;
    }
    const std::string needle = "\"dev_id\": \"" + selected_device + "\"";
    return json.find(needle) != std::string::npos;
}

static void connect_selected_device_if_pending(const std::string &json)
{
    if (!auto_connect_pending.load() || !lan_user_logged_in.load()) {
        return;
    }
    if (!printer_json_matches_selected_device(json)) {
        return;
    }

    auto_connect_pending.store(false);
    write_diag("connect_selected_device_if_pending: autoconnect dev_id=" + selected_device);
    if (on_printer_connected) {
        on_printer_connected(selected_device);
    }
    bambu_network_rs_connect(selected_device);
}

static BambuPlugin::LoginState lan_login_state()
{
    BambuPlugin::LoginState state;
    state.logged_in = lan_user_logged_in.load();
    if (state.logged_in) {
        state.user_id = "00000000000000001";
        state.user_name = "LAN User";
        state.nickname = "LAN User";
        state.avatar = "";
    }
    return state;
}

static void set_lan_login_state(bool logged_in, bool notify)
{
    lan_user_logged_in.store(logged_in);
    write_diag(std::string("set_lan_login_state: logged_in=") + (logged_in ? "true" : "false"));
    if (notify && on_user_login) {
        on_user_login(0, logged_in);
    }
}

static void write_diag(const std::string &message) {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    std::string log_path;

    if (xdg && *xdg) {
        log_path = std::string(xdg) + "/BambuStudio/oss-plugin-debug.log";
    } else if (home && *home) {
        log_path = std::string(home) + "/.config/BambuStudio/oss-plugin-debug.log";
    } else {
        log_path = "/tmp/bambu-oss-plugin-debug.log";
    }

    std::ofstream log(log_path, std::ios::app);
    if (log.is_open()) {
        log << message << "\n";
    }
}

static void write_periodic_diag(const char *label, size_t &counter, size_t period, const std::string &message)
{
    counter++;
    if (counter <= 5 || (period > 0 && (counter % period) == 0)) {
        write_diag(message + " count=" + std::to_string(counter));
    }
}

static void write_backtrace_once(const char *label)
{
    static bool wrote_check_debug = false;
    static bool wrote_get_version = false;

    bool *slot = nullptr;
    if (std::string(label) == "check_debug_consistent") {
        slot = &wrote_check_debug;
    } else if (std::string(label) == "get_version") {
        slot = &wrote_get_version;
    }

    if (slot && *slot) {
        return;
    }
    if (slot) {
        *slot = true;
    }

    void *frames[16];
    int count = backtrace(frames, 16);
    char **symbols = backtrace_symbols(frames, count);
    if (!symbols) {
        write_diag(std::string("backtrace unavailable for ") + label);
        return;
    }

    write_diag(std::string("backtrace begin: ") + label);
    for (int i = 0; i < count; ++i) {
        write_diag(std::string("  ") + symbols[i]);
    }
    write_diag(std::string("backtrace end: ") + label);
    free(symbols);
}

static std::string preview_payload(const std::string &message, size_t limit)
{
    const size_t preview_len = std::min(message.size(), limit);
    std::string preview = BambuPlugin::json_escape(message.substr(0, preview_len));
    if (preview_len < message.size()) {
        preview += "...";
    }
    return preview;
}

static void log_print_params(const char *fn, const PrintParams &params)
{
    write_diag(
        std::string(fn) +
        ": dev_id=" + params.dev_id +
        " task_name=" + params.task_name +
        " project_name=" + params.project_name +
        " filename=" + params.filename +
        " ftp_file=" + params.ftp_file +
        " dst_file=" + params.dst_file +
        " plate_index=" + std::to_string(params.plate_index) +
        " ams_mapping=" + params.ams_mapping +
        " ams_mapping2=" + params.ams_mapping2 +
        " ams_mapping_info=" + params.ams_mapping_info +
        " use_ams=" + (params.task_use_ams ? "true" : "false") +
        " bed_leveling=" + (params.task_bed_leveling ? "true" : "false") +
        " flow_cali=" + (params.task_flow_cali ? "true" : "false") +
        " vibration_cali=" + (params.task_vibration_cali ? "true" : "false") +
        " layer_inspect=" + (params.task_layer_inspect ? "true" : "false") +
        " timelapse=" + (params.task_record_timelapse ? "true" : "false")
    );
}

static void report_update_status(OnUpdateStatusFn update_fn, int status, int code, const std::string &message)
{
    write_diag(
        "report_update_status: status=" + std::to_string(status) +
        " code=" + std::to_string(code) +
        " message=" + message +
        " callback=" + (update_fn ? "set" : "null")
    );
    if (!update_fn) {
        return;
    }
    update_fn(status, code, message);
}

static void log_local_file_probe(const char *context, const std::string &path)
{
    errno = 0;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        const int open_errno = errno;
        write_diag(
            std::string(context) +
            " local_file_probe=open_failed path=" + path +
            " errno=" + std::to_string(open_errno) +
            " error=" + std::strerror(open_errno)
        );
        return;
    }

    const std::streamoff end_pos = file.tellg();
    write_diag(
        std::string(context) +
        " local_file_probe=open_ok path=" + path +
        " size=" + std::to_string(static_cast<long long>(end_pos))
    );
}

static int start_local_print_impl(const PrintParams &params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    log_print_params("bambu_network_start_local_print", params);
    report_update_status(update_fn, PrintingStageCreate, BAMBU_NETWORK_SUCCESS, "Preparing print");

    if (cancel_fn && cancel_fn()) {
        report_update_status(update_fn, PrintingStageERROR, BAMBU_NETWORK_ERR_CANCELED, "Canceled");
        return BAMBU_NETWORK_ERR_CANCELED;
    }

    const std::string remote_filename = BambuPlugin::resolve_remote_filename(params);
    const std::string plate_path = BambuPlugin::resolve_plate_metadata_path(params);

    report_update_status(update_fn, PrintingStageUpload, BAMBU_NETWORK_SUCCESS, "Uploading to printer");
    log_local_file_probe("bambu_network_start_local_print", params.filename);
    const int upload_result = bambu_network_rs_upload_file(params.dev_id, params.filename, remote_filename);
    write_diag(
        "bambu_network_start_local_print upload_result=" +
        std::to_string(upload_result) +
        " remote_filename=" + remote_filename +
        " plate_path=" + plate_path
    );
    if (upload_result != BAMBU_NETWORK_SUCCESS) {
        report_update_status(update_fn, PrintingStageERROR, upload_result, "Upload failed");
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }

    update_local_print_context(params, remote_filename, plate_path);

    if (cancel_fn && cancel_fn()) {
        report_update_status(update_fn, PrintingStageERROR, BAMBU_NETWORK_ERR_CANCELED, "Canceled");
        return BAMBU_NETWORK_ERR_CANCELED;
    }

    report_update_status(update_fn, PrintingStageSending, BAMBU_NETWORK_SUCCESS, "Sending print command");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::string json = BambuPlugin::build_project_file_command(params, remote_filename, plate_path);
    write_diag("bambu_network_start_local_print command=" + json);

    const int send_result = bambu_network_rs_send(params.dev_id, json);
    write_diag("bambu_network_start_local_print send_result=" + std::to_string(send_result));
    if (send_result != BAMBU_NETWORK_SUCCESS) {
        report_update_status(update_fn, PrintingStageERROR, send_result, "Send failed");
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }

    if (wait_fn) {
        report_update_status(update_fn, PrintingStageWaitPrinter, BAMBU_NETWORK_SUCCESS, "Waiting for printer");
        const bool wait_ok = wait_fn(PrintingStageWaitPrinter, "{}");
        write_diag(std::string("bambu_network_start_local_print wait_result=") + (wait_ok ? "true" : "false"));
        if (!wait_ok) {
            report_update_status(update_fn, PrintingStageERROR, BAMBU_NETWORK_ERR_TIMEOUT, "Wait failed");
            return BAMBU_NETWORK_ERR_TIMEOUT;
        }
    }

    report_update_status(update_fn, PrintingStageFinished, BAMBU_NETWORK_SUCCESS, "Print submitted");
    return BAMBU_NETWORK_SUCCESS;
}

void bambu_init() {
    LOG_CALL();
    write_diag("bambu_init");
}

int Bambu_Create(Bambu_Tunnel *tunnel, char const *path)
{
    if (!tunnel || !path) {
        last_tunnel_error = "Bambu_Create: invalid arguments";
        return -1;
    }

    auto session = std::make_unique<CompatTunnel>();
    session->url = path;
    session->device_id = bambu_query_param(session->url, "device");
    if (session->device_id.empty()) {
        last_tunnel_error = "Bambu_Create: missing device in URL";
        return -1;
    }

    *tunnel = reinterpret_cast<Bambu_Tunnel>(session.release());
    write_diag("Bambu_Create: url=" + std::string(path) + " device=" + as_compat_tunnel(*tunnel)->device_id);
    return Bambu_success;
}

void Bambu_SetLogger(Bambu_Tunnel tunnel, Logger logger, void *context)
{
    if (auto *session = as_compat_tunnel(tunnel)) {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->logger = logger;
        session->logger_context = context;
    }
}

int Bambu_Open(Bambu_Tunnel tunnel)
{
    auto *session = as_compat_tunnel(tunnel);
    if (!session) {
        last_tunnel_error = "Bambu_Open: invalid tunnel";
        return -1;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    session->opened = true;
    compat_tunnel_log(session, "Bambu_Open: device=" + session->device_id);
    return Bambu_success;
}

int Bambu_StartStream(Bambu_Tunnel tunnel, bool video)
{
    auto *session = as_compat_tunnel(tunnel);
    if (!session) {
        last_tunnel_error = "Bambu_StartStream: invalid tunnel";
        return -1;
    }
    if (video) {
        last_tunnel_error = "Bambu_StartStream: video stream unsupported";
        return -1;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    session->control_stream_started = true;
    return Bambu_success;
}

int Bambu_StartStreamEx(Bambu_Tunnel tunnel, int type)
{
    auto *session = as_compat_tunnel(tunnel);
    if (!session) {
        last_tunnel_error = "Bambu_StartStreamEx: invalid tunnel";
        return -1;
    }
    if (type != 0x3001) {
        last_tunnel_error = "Bambu_StartStreamEx: unsupported stream type";
        return -1;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    session->control_stream_started = true;
    return Bambu_success;
}

int Bambu_GetStreamCount(Bambu_Tunnel)
{
    return 0;
}

int Bambu_GetStreamInfo(Bambu_Tunnel, int, Bambu_StreamInfo *)
{
    return -1;
}

unsigned long Bambu_GetDuration(Bambu_Tunnel)
{
    return 0;
}

int Bambu_Seek(Bambu_Tunnel, unsigned long)
{
    return -1;
}

int Bambu_SendMessage(Bambu_Tunnel tunnel, int ctrl, char const *data, int len)
{
    auto *session = as_compat_tunnel(tunnel);
    if (!session || !data || len < 0) {
        last_tunnel_error = "Bambu_SendMessage: invalid arguments";
        return -1;
    }
    if (ctrl != 0x3001) {
        last_tunnel_error = "Bambu_SendMessage: unsupported control type";
        return -1;
    }

    const std::string request(data, static_cast<size_t>(len));
    const bool is_file_download = request.find("\"cmdtype\":4") != std::string::npos;
    compat_tunnel_log(session, "Bambu_SendMessage: device=" + session->device_id + " request=" + preview_payload(request));
    rust::Vec<std::uint8_t> response = bambu_network_rs_tunnel_request(session->url, session->device_id, request);
    if (response.empty()) {
        last_tunnel_error = "Bambu_SendMessage: empty control response";
        return -1;
    }

    std::string response_bytes;
    response_bytes.reserve(response.size());
    for (std::uint8_t byte : response) {
        response_bytes.push_back(static_cast<char>(byte));
    }
    compat_tunnel_log(session, "Bambu_SendMessage: response_bytes=" + std::to_string(response_bytes.size()));

    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (is_file_download) {
            auto frames = split_file_download_response_frames(response_bytes);
            compat_tunnel_log(
                session,
                "Bambu_SendMessage: queued_frames=" + std::to_string(frames.size()) +
                    " chunked_file_download=" + std::string(frames.size() > 1 ? "true" : "false"));
            for (auto &frame : frames) {
                session->queued_responses.emplace_back(std::move(frame));
            }
        } else {
            session->queued_responses.emplace_back(std::move(response_bytes));
        }
    }
    return Bambu_success;
}

int Bambu_ReadSample(Bambu_Tunnel tunnel, Bambu_Sample *sample)
{
    auto *session = as_compat_tunnel(tunnel);
    if (!session || !sample) {
        last_tunnel_error = "Bambu_ReadSample: invalid arguments";
        return -1;
    }

    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->queued_responses.empty()) {
        return Bambu_would_block;
    }

    session->active_response = std::move(session->queued_responses.front());
    session->queued_responses.pop_front();
    compat_tunnel_log(session, "Bambu_ReadSample: response_bytes=" + std::to_string(session->active_response.size()));
    sample->itrack = 0;
    sample->size = static_cast<int>(session->active_response.size());
    sample->flags = 1;
    sample->buffer = reinterpret_cast<unsigned char const *>(session->active_response.data());
    sample->decode_time = 0;
    return Bambu_success;
}

int Bambu_RecvMessage(Bambu_Tunnel tunnel, int *ctrl, char *data, int *len)
{
    auto *session = as_compat_tunnel(tunnel);
    if (!session || !len) {
        last_tunnel_error = "Bambu_RecvMessage: invalid arguments";
        return -1;
    }

    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->queued_responses.empty()) {
        return Bambu_would_block;
    }

    const std::string &response = session->queued_responses.front();
    if (!data || *len < static_cast<int>(response.size())) {
        *len = static_cast<int>(response.size());
        return Bambu_buffer_limit;
    }

    std::memcpy(data, response.data(), response.size());
    *len = static_cast<int>(response.size());
    if (ctrl) {
        *ctrl = 0x3001;
    }
    session->queued_responses.pop_front();
    return Bambu_success;
}

void Bambu_Close(Bambu_Tunnel tunnel)
{
    if (auto *session = as_compat_tunnel(tunnel)) {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->opened = false;
        session->control_stream_started = false;
        session->queued_responses.clear();
        session->active_response.clear();
    }
}

void Bambu_Destroy(Bambu_Tunnel tunnel)
{
    delete as_compat_tunnel(tunnel);
}

int Bambu_Init()
{
    return Bambu_success;
}

void Bambu_Deinit()
{
}

char const *Bambu_GetLastErrorMsg()
{
    return last_tunnel_error.c_str();
}

void Bambu_FreeLogMsg(tchar const *msg)
{
    std::free(const_cast<tchar *>(msg));
}

void bambu_network_cb_printer_available(const std::string &json) {
    const std::string effective_json = apply_printer_name_override(json);
    const std::string dev_id = json_string_field(effective_json, "dev_id");
    if (!dev_id.empty()) {
        std::lock_guard<std::mutex> lock(printer_name_overrides_mutex);
        last_printer_json_by_id[dev_id] = effective_json;
    }
    if (on_msg_arrived) {
        on_msg_arrived(effective_json);
    }
    connect_selected_device_if_pending(effective_json);
}
void bambu_network_cb_message_recv(const std::string &device_id, const std::string &message) {
    const std::string effective_message = rewrite_local_print_status_message(device_id, message);
    if (on_message || on_mqtt_message) {
        write_diag(
            "bambu_network_cb_message_recv: dev_id=" + device_id +
            " msg_len=" + std::to_string(effective_message.size()) +
            " preview=\"" + preview_payload(effective_message) + "\""
        );
    }

    if (on_mqtt_message) {
        write_diag("bambu_network_cb_message_recv: entering local callback");
        on_mqtt_message(device_id, effective_message);
        write_diag("bambu_network_cb_message_recv: local callback returned");
    }

    if (on_message) {
        write_diag("bambu_network_cb_message_recv: entering message callback");
        on_message(device_id, effective_message);
        write_diag("bambu_network_cb_message_recv: message callback returned");
    }
}
void bambu_network_cb_connected(const std::string &device_id) {
    write_diag("bambu_network_cb_connected: " + device_id);
    connect_in_progress.store(false);
    printer_connected.store(true);
    if (on_printer_connected) {
        write_diag("bambu_network_cb_connected: signaling printer connected " + device_id);
        on_printer_connected(device_id);
    }
    if (on_local_connect) {
        write_diag("bambu_network_cb_connected: calling on_local_connect");
        on_local_connect(0, device_id, "Connected");
        write_diag("bambu_network_cb_connected: on_local_connect returned");
    }
}

void bambu_network_cb_disconnected(const std::string &device_id, int status, const std::string &message) {
    write_diag(
        "bambu_network_cb_disconnected: dev_id=" + device_id +
        " status=" + std::to_string(status) +
        " message=" + message
    );
    connect_in_progress.store(false);
    const bool was_connected = printer_connected.exchange(false);
    if (on_local_connect && (was_connected || status == ConnectStatusFailed)) {
        write_diag("bambu_network_cb_disconnected: calling on_local_connect");
        on_local_connect(status, device_id, message);
        write_diag("bambu_network_cb_disconnected: on_local_connect returned");
    }
}

bool bambu_network_check_debug_consistent(bool is_debug)
{
    LOG_CALL();
    write_backtrace_once("check_debug_consistent");
    const char *override = std::getenv("BAMBU_NETWORK_DEBUG_CONSISTENT_OVERRIDE");
    bool result = true;
    if (override && *override) {
        result = std::string(override) == "1" || std::string(override) == "true" || std::string(override) == "TRUE";
    }
    write_diag(
        std::string("bambu_network_check_debug_consistent: input=") +
        (is_debug ? "true" : "false") +
        " result=" +
        (result ? "true" : "false")
    );
    return result;
}
std::string bambu_network_get_version(void)
{
    LOG_CALL();
    write_backtrace_once("get_version");
    const char *override = std::getenv("BAMBU_NETWORK_AGENT_VERSION_OVERRIDE");
    const std::string version = (override && *override) ? override : BAMBU_NETWORK_AGENT_VERSION;
    write_diag(std::string("bambu_network_get_version: ") + version);
    return version;
}
void *bambu_network_create_agent(std::string log_dir)
{
    LOG_CALL();
    write_diag("bambu_network_create_agent: log_dir=" + log_dir);
    bambu_network_rs_init();
    return (void*)0x10000000;
}
int bambu_network_destroy_agent(void *agent_ptr)
{
    LOG_CALL();
    return 0;
}
int bambu_network_init_log(void *agent_ptr)
{
    LOG_CALL();
    return 0;
}
int bambu_network_set_config_dir(void *agent_ptr, std::string config_dir)
{
    LOG_CALL_ARGS("%s", config_dir.c_str());
    write_diag("bambu_network_set_config_dir: " + config_dir);
    config_dir_path = config_dir;
    load_selected_device();
    load_printer_name_overrides();
    load_local_print_contexts();
    return 0;
}
int bambu_network_set_cert_file(void *agent_ptr, std::string folder, std::string filename)
{
    LOG_CALL();
    ca_path = folder + "/" + filename;
    return 0;
}
int bambu_network_set_country_code(void *agent_ptr, std::string country_code)
{
    LOG_CALL();
    // No region-locked features.
    return 0;
}
int bambu_network_start(void *agent_ptr)
{
    LOG_CALL();
    return 0;
}
int bambu_network_set_on_ssdp_msg_fn(void *agent_ptr, OnMsgArrivedFn fn)
{
    LOG_CALL();
    on_msg_arrived = fn;
    return 0;
}
int bambu_network_set_on_user_login_fn(void *agent_ptr, OnUserLoginFn fn)
{
    LOG_CALL();
    on_user_login = fn;
    if (on_user_login) {
        write_diag("bambu_network_set_on_user_login_fn: signaling current LAN login state");
        on_user_login(0, lan_user_logged_in.load());
    }
    return 0;
}
int bambu_network_set_on_printer_connected_fn(void *agent_ptr, OnPrinterConnectedFn fn)
{
    LOG_CALL();
    on_printer_connected = fn;
    if (on_printer_connected && !selected_device.empty() && printer_connected.load()) {
        write_diag("bambu_network_set_on_printer_connected_fn: signaling existing printer " + selected_device);
        on_printer_connected(selected_device);
    }
    return 0;
}
int bambu_network_set_on_server_connected_fn(void *agent_ptr, OnServerConnectedFn fn)
{
    LOG_CALL();
    on_server_connected = fn;
    if (on_server_connected) {
        write_diag("bambu_network_set_on_server_connected_fn: signaling LAN server connected");
        on_server_connected(ConnectStatusOk, 0);
    }
    return 0;
}
int bambu_network_set_on_http_error_fn(void *agent_ptr, OnHttpErrorFn fn)
{
    LOG_CALL();
    return 0;
}
int bambu_network_set_get_country_code_fn(void *agent_ptr, GetCountryCodeFn fn)
{
    LOG_CALL();
    // No region-locked features.
    return 0;
}
int bambu_network_set_on_message_fn(void *agent_ptr, OnMessageFn fn)
{
    LOG_CALL();
    write_diag("bambu_network_set_on_message_fn");
    on_message = fn;
    return 0;
}
int bambu_network_set_on_local_connect_fn(void *agent_ptr, OnLocalConnectedFn fn)
{
    LOG_CALL();
    on_local_connect = fn;
    return 0;
}
int bambu_network_set_on_local_message_fn(void *agent_ptr, OnMessageFn fn)
{
    LOG_CALL();
    write_diag("bambu_network_set_on_local_message_fn");
    on_mqtt_message = fn;
    return 0;
}
int bambu_network_set_queue_on_main_fn(void *agent_ptr, QueueOnMainFn fn)
{
    LOG_CALL();
    return 0;
}
int bambu_network_connect_server(void *agent_ptr)
{
    LOG_CALL();
    // No-op.
    return 0; // Maybe BAMBU_NETWORK_ERR_CONNECT_FAILED is more appropriate?
}
bool bambu_network_is_server_connected(void *agent_ptr)
{
    write_diag("bambu_network_is_server_connected: returning true (LAN mode)");
    return true;
}
int bambu_network_refresh_connection(void *agent_ptr)
{
    LOG_CALL();
    return 0; // Maybe BAMBU_NETWORK_ERR_CONNECT_FAILED is more appropriate?
}
int bambu_network_start_subscribe(void *agent_ptr, std::string module)
{
    LOG_CALL();
    return 0; // Maybe BAMBU_NETWORK_ERR_CONNECT_FAILED is more appropriate?
}
int bambu_network_stop_subscribe(void *agent_ptr, std::string module)
{
    LOG_CALL();
    return 0;
}
int bambu_network_send_message(void *agent_ptr, std::string dev_id, std::string json_str, int qos, int flag)
{
    LOG_CALL();
    // TODO: Maybe send message here, but this seems cloud-only.
    return 0;
}
int bambu_network_connect_printer(void *agent_ptr, std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    LOG_CALL_ARGS("%s %s %s %s", dev_id.c_str(), dev_ip.c_str(), username.c_str(), password.c_str());
    if (selected_device == dev_id && printer_connected.load()) {
        write_diag("bambu_network_connect_printer: already connected " + dev_id);
        return 0;
    }
    if (selected_device == dev_id && connect_in_progress.load()) {
        write_diag("bambu_network_connect_printer: connect already in progress " + dev_id);
        return 0;
    }
    write_diag("bambu_network_connect_printer: " + dev_id + "@" + dev_ip);
    selected_device = dev_id;
    auto_connect_pending.store(false);
    connect_in_progress.store(true);
    printer_connected.store(false);
    persist_selected_device();
    bambu_network_rs_connect(dev_id);
    return 0;
}
int bambu_network_disconnect_printer(void *agent_ptr)
{
    LOG_CALL();
    if (connect_in_progress.load() && !printer_connected.load()) {
        write_diag("bambu_network_disconnect_printer: ignoring disconnect during in-flight connect");
        return 0;
    }
    if (!selected_device.empty()) {
        write_diag("bambu_network_disconnect_printer: dev_id=" + selected_device);
        bambu_network_rs_disconnect(selected_device);
    }
    return 0;
}
int bambu_network_send_message_to_printer(void *agent_ptr, std::string dev_id, std::string json_str, int qos, int flag)
{
    write_diag("bambu_network_send_message_to_printer: dev_id=" + dev_id);
    bambu_network_rs_send(dev_id, json_str);
    write_diag("bambu_network_send_message_to_printer: returned");
    return 0;
}
bool bambu_network_start_discovery(void *agent_ptr, bool start, bool sending)
{
    LOG_CALL();
    return 0;
}
int bambu_network_change_user(void *agent_ptr, std::string user_info)
{
    write_diag("bambu_network_change_user: user_info_len=" + std::to_string(user_info.size()));
    set_lan_login_state(true, true);
    return BAMBU_NETWORK_SUCCESS;
}
bool bambu_network_is_user_login(void *agent_ptr)
{
    write_periodic_diag(
        "bambu_network_is_user_login",
        user_login_log_counter,
        50,
        std::string("bambu_network_is_user_login: returning ") +
        (lan_user_logged_in.load() ? "true" : "false") +
        " (LAN mode)"
    );
    return lan_user_logged_in.load();
}
int bambu_network_user_logout(void *agent_ptr, bool request)
{
    write_diag(std::string("bambu_network_user_logout request=") + (request ? "true" : "false"));
    if (!selected_device.empty()) {
        bambu_network_rs_disconnect(selected_device);
        clear_local_print_context(selected_device);
    }
    printer_connected.store(false);
    selected_device.clear();
    auto_connect_pending.store(false);
    persist_selected_device();
    set_lan_login_state(false, true);
    return 0;
}
std::string bambu_network_get_user_id(void *agent_ptr)
{
    write_diag("bambu_network_get_user_id");
    return lan_login_state().user_id;
}
std::string bambu_network_get_user_name(void *agent_ptr)
{
    write_diag("bambu_network_get_user_name");
    return lan_login_state().user_name;
}
std::string bambu_network_get_user_avatar(void *agent_ptr)
{
    write_diag("bambu_network_get_user_avatar");
    return lan_login_state().avatar;
}
std::string bambu_network_get_user_nickanme(void *agent_ptr)
{
    write_diag("bambu_network_get_user_nickanme");
    return lan_login_state().nickname;
}
std::string bambu_network_build_login_cmd(void *agent_ptr)
{
    LOG_CALL();
    const std::string json = BambuPlugin::build_logged_in_login_cmd(lan_login_state());
    write_periodic_diag(
        "bambu_network_build_login_cmd",
        build_login_cmd_log_counter,
        20,
        "bambu_network_build_login_cmd: returning " + json
    );
    return json;
}
std::string bambu_network_build_logout_cmd(void *agent_ptr)
{
    LOG_CALL();
    return BambuPlugin::build_logout_cmd("lan");
}
std::string bambu_network_build_login_info(void *agent_ptr)
{
    LOG_CALL();
    return BambuPlugin::build_login_info(lan_login_state(), kLanBackendUrl, kLanBackendUrl);
}
int bambu_network_bind(void *agent_ptr, std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    LOG_CALL();
    return 0;
}
int bambu_network_unbind(void *agent_ptr, std::string dev_id)
{
    LOG_CALL();
    return 0;
}
std::string bambu_network_get_bambulab_host(void *agent_ptr)
{
    LOG_CALL();
    return "bambulab.com";
}
std::string bambu_network_get_user_selected_machine(void *agent_ptr)
{
    write_diag("bambu_network_get_user_selected_machine: " + selected_device);
    return selected_device;
}
int bambu_network_set_user_selected_machine(void *agent_ptr, std::string dev_id)
{
    write_diag("bambu_network_set_user_selected_machine: " + dev_id);
    selected_device = dev_id;
    auto_connect_pending.store(!dev_id.empty());
    persist_selected_device();
    return 0;
}
int bambu_network_start_print(void *agent_ptr, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    write_diag("bambu_network_start_print");
    return start_local_print_impl(params, update_fn, cancel_fn, wait_fn);
}
int bambu_network_start_local_print_with_record(void *agent_ptr, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    LOG_CALL();
    return start_local_print_impl(params, update_fn, cancel_fn, wait_fn);
}
int bambu_network_start_send_gcode_to_sdcard(void *agent_ptr, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    LOG_CALL_ARGS("%s %s %s %s", params.project_name.c_str(), params.filename.c_str(), params.ftp_file.c_str(), params.dst_file.c_str());
    log_print_params("bambu_network_start_send_gcode_to_sdcard", params);
    report_update_status(update_fn, PrintingStageCreate, BAMBU_NETWORK_SUCCESS, "Preparing upload");

    const std::string remote_filename = BambuPlugin::resolve_remote_filename(params);
    report_update_status(update_fn, PrintingStageUpload, BAMBU_NETWORK_SUCCESS, "Uploading to printer");
    log_local_file_probe("bambu_network_start_send_gcode_to_sdcard", params.filename);
    const int upload_result = bambu_network_rs_upload_file(params.dev_id, params.filename, remote_filename);
    write_diag(
        "bambu_network_start_send_gcode_to_sdcard upload_result=" +
        std::to_string(upload_result) +
        " remote_filename=" + remote_filename
    );

    if (upload_result != BAMBU_NETWORK_SUCCESS) {
        report_update_status(update_fn, PrintingStageERROR, upload_result, "Upload failed");
        return BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;
    }

    report_update_status(update_fn, PrintingStageFinished, BAMBU_NETWORK_SUCCESS, "Upload finished");
    return BAMBU_NETWORK_SUCCESS;
}
int bambu_network_start_local_print(void *agent_ptr, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    LOG_CALL_ARGS("%s %s %s", params.project_name.c_str(), params.filename.c_str(), params.ams_mapping.c_str());
    return start_local_print_impl(params, update_fn, cancel_fn, nullptr);
}
int bambu_network_get_user_presets(void *agent_ptr, std::map<std::string, std::map<std::string, std::string>> *user_presets)
{
    write_diag("bambu_network_get_user_presets");
    return 0;
}
std::string bambu_network_request_setting_id(void *agent_ptr, std::string name, std::map<std::string, std::string> *values_map, unsigned int *http_code)
{
    write_diag("bambu_network_request_setting_id: " + name);
    if (http_code) {
        *http_code = 200;
    }
    return "";
}
int bambu_network_put_setting(void *agent_ptr, std::string setting_id, std::string name, std::map<std::string, std::string> *values_map, unsigned int *http_code)
{
    LOG_CALL();
    return 0;
}
int bambu_network_get_setting_list(void *agent_ptr, std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    write_diag("bambu_network_get_setting_list: bundle_version=" + bundle_version);
    return 0;
}
int bambu_network_delete_setting(void *agent_ptr, std::string setting_id)
{
    LOG_CALL();
    return 0;
}
std::string bambu_network_get_studio_info_url(void *agent_ptr)
{
    LOG_CALL();
    return "https://studio.bambulab.com";
}
int bambu_network_set_extra_http_header(void *agent_ptr, std::map<std::string, std::string> extra_headers)
{
    LOG_CALL();
    return 0;
}
int bambu_network_get_my_message(void *agent_ptr, int type, int after, int limit, unsigned int *http_code, std::string *http_body)
{
    LOG_CALL();
    return 0;
}
int bambu_network_check_user_task_report(void *agent_ptr, int *task_id, bool *printable)
{
    LOG_CALL();
    return 0;
}
int bambu_network_get_user_print_info(void *agent_ptr, unsigned int *http_code, std::string *http_body)
{
    LOG_CALL();
    return 0;
}

int bambu_network_get_printer_firmware(void *agent_ptr, std::string dev_id, unsigned *http_code, std::string *http_body)
{
    LOG_CALL();
    *http_code = 500;
    *http_body = "";
    return 0;
}
int bambu_network_get_task_plate_index(void *agent_ptr, std::string task_id, int *plate_index)
{
    LOG_CALL();
    return 0;
}
int bambu_network_get_user_info(void *agent_ptr, int *identifier)
{
    LOG_CALL();
    return 0;
}
int bambu_network_request_bind_ticket(void *agent_ptr, std::string *ticket)
{
    LOG_CALL();
    return 0;
}
int bambu_network_get_slice_info(void *agent_ptr, std::string project_id, std::string profile_id, int plate_index, std::string *slice_json)
{
    LOG_CALL();
    return 0;
}
int bambu_network_query_bind_status(void *agent_ptr, std::vector<std::string> query_list, unsigned int *http_code, std::string *http_body)
{
    LOG_CALL();
    return 0;
}
int bambu_network_modify_printer_name(void *agent_ptr, std::string dev_id, std::string dev_name)
{
    write_diag(
        "bambu_network_modify_printer_name: dev_id=" + dev_id +
        " dev_name=" + dev_name
    );
    {
        std::lock_guard<std::mutex> lock(printer_name_overrides_mutex);
        if (dev_name.empty()) {
            printer_name_overrides.erase(dev_id);
        } else {
            printer_name_overrides[dev_id] = dev_name;
        }
    }
    persist_printer_name_overrides();

    std::string last_json;
    std::string updated_json;
    {
        std::lock_guard<std::mutex> lock(printer_name_overrides_mutex);
        const auto it = last_printer_json_by_id.find(dev_id);
        if (it != last_printer_json_by_id.end()) {
            last_json = it->second;
        }
    }
    if (!last_json.empty()) {
        updated_json = apply_printer_name_override(last_json);
        std::lock_guard<std::mutex> lock(printer_name_overrides_mutex);
        last_printer_json_by_id[dev_id] = updated_json;
    }
    if (!updated_json.empty() && on_msg_arrived) {
        on_msg_arrived(updated_json);
    }
    return 0;
}
int bambu_network_get_camera_url(void *agent_ptr, std::string dev_id, std::function<void(std::string)> callback)
{
    LOG_CALL();
    return 0;
}
int bambu_network_get_design_staffpick(void *agent_ptr, int offset, int limit, std::function<void(std::string)> callback)
{
    LOG_CALL();
    return 0;
}
int bambu_network_start_publish(void *agent_ptr, PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string *out)
{
    LOG_CALL();
    return 0;
}
int bambu_network_get_profile_3mf(void *agent_ptr, BBLProfile *profile)
{
    LOG_CALL();
    return 0;
}
int bambu_network_get_model_publish_url(void *agent_ptr, std::string *url)
{
    LOG_CALL();
    if (url) *url = "https://makerworld.com/upload";
    return 0;
}
int bambu_network_get_subtask(void *agent_ptr, BBLModelTask *task, OnGetSubTaskFn getsub_fn)
{
    LOG_CALL();
    if (getsub_fn && task) {
        getsub_fn(task);
    }
    return 0;
}
int bambu_network_get_model_mall_home_url(void *agent_ptr, std::string *url)
{
    LOG_CALL();
    if (url) *url = "https://makerworld.com";
    return 0;
}
int bambu_network_get_model_mall_detail_url(void *agent_ptr, std::string *url, std::string id)
{
    LOG_CALL();
    if (url) *url = "https://makerworld.com/models/" + id;
    return 0;
}
int bambu_network_get_my_profile(void *agent_ptr, std::string token, unsigned int *http_code, std::string *http_body)
{
    LOG_CALL();
    return 0;
}
int bambu_network_track_enable(void *agent_ptr, bool enable)
{
    LOG_CALL();
    return 0;
}
int bambu_network_track_event(void *agent_ptr, std::string evt_key, std::string content)
{
    LOG_CALL();
    return 0;
}
int bambu_network_track_header(void *agent_ptr, std::string header)
{
    LOG_CALL();
    return 0;
}
int bambu_network_track_update_property(void *agent_ptr, std::string name, std::string value, std::string type)
{
    LOG_CALL();
    return 0;
}
int bambu_network_track_get_property(void *agent_ptr, std::string name, std::string &value, std::string type)
{
    LOG_CALL();
    value = "";
    return 0;
}

#define BAMBU_COMPAT_STUB(name) \
int name(...) \
{ \
    LOG_CALL(); \
    write_diag(#name); \
    return 0; \
}

int bambu_network_set_on_subscribe_failure_fn(void *agent_ptr, GetSubscribeFailureFn fn)
{
    LOG_CALL();
    write_diag("bambu_network_set_on_subscribe_failure_fn");
    on_subscribe_failure = fn;
    return 0;
}

int bambu_network_set_on_user_message_fn(void *agent_ptr, OnMessageFn fn)
{
    LOG_CALL();
    write_diag("bambu_network_set_on_user_message_fn");
    on_user_message = fn;
    return 0;
}

void bambu_network_install_device_cert(void *agent_ptr, std::string dev_id, bool lan_only)
{
    LOG_CALL();
    write_diag(
        "bambu_network_install_device_cert: dev_id=" + dev_id +
        " lan_only=" + (lan_only ? "true" : "false")
    );
}

void bambu_network_enable_multi_machine(void *agent_ptr, bool enable)
{
    LOG_CALL();
    write_diag(std::string("bambu_network_enable_multi_machine: enable=") + (enable ? "true" : "false"));
}

int bambu_network_add_subscribe(void *agent_ptr, std::vector<std::string> dev_list)
{
    LOG_CALL();
    write_diag("bambu_network_add_subscribe count=" + std::to_string(dev_list.size()));
    return 0;
}

int bambu_network_del_subscribe(void *agent_ptr, std::vector<std::string> dev_list)
{
    LOG_CALL();
    write_diag("bambu_network_del_subscribe count=" + std::to_string(dev_list.size()));
    return 0;
}

BAMBU_COMPAT_STUB(bambu_network_report_consent)
BAMBU_COMPAT_STUB(bambu_network_get_oss_config)
BAMBU_COMPAT_STUB(bambu_network_get_my_token)
BAMBU_COMPAT_STUB(bambu_network_track_remove_files)
BAMBU_COMPAT_STUB(bambu_network_check_user_report)
BAMBU_COMPAT_STUB(bambu_network_get_camera_url_for_golive)
BAMBU_COMPAT_STUB(bambu_network_get_hms_snapshot)
BAMBU_COMPAT_STUB(bambu_network_get_model_rating_id)
BAMBU_COMPAT_STUB(bambu_network_put_model_mall_rating)
BAMBU_COMPAT_STUB(bambu_network_get_model_instance_id)
BAMBU_COMPAT_STUB(bambu_network_get_model_mall_rating)
BAMBU_COMPAT_STUB(bambu_network_put_rating_picture_oss)
BAMBU_COMPAT_STUB(bambu_network_del_rating_picture_oss)

int bambu_network_get_subtask_info(void *agent_ptr, std::string subtask_id, std::string *task_json, unsigned int *http_code, std::string *http_body)
{
    LOG_CALL();
    LocalPrintContext context;
    if (!lookup_local_print_context_by_subtask_id(subtask_id, context)) {
        write_diag("bambu_network_get_subtask_info: not found subtask_id=" + subtask_id);
        if (http_code) {
            *http_code = 404;
        }
        if (http_body) {
            *http_body = "{\"error\":\"not_found\"}";
        }
        if (task_json) {
            task_json->clear();
        }
        return -1;
    }

    if (http_code) {
        *http_code = 200;
    }
    if (http_body) {
        *http_body = "{}";
    }
    if (task_json) {
        *task_json =
            "{"
            "\"content\":\"{\\\"info\\\":{\\\"plate_idx\\\":" + std::to_string(context.plate_index) + "}}\","
            "\"context\":{"
            "\"plates\":[{"
            "\"index\":" + std::to_string(context.plate_index) + ","
            "\"thumbnail\":{\"url\":\"" + BambuPlugin::json_escape(context.thumbnail_url) + "\"},"
            "\"filaments\":[]"
            "}]"
            "}"
            "}";
    }

    write_diag(
        "bambu_network_get_subtask_info: subtask_id=" + subtask_id +
        " plate_index=" + std::to_string(context.plate_index) +
        " thumbnail_url=" + context.thumbnail_url
    );
    return 0;
}

int bambu_network_get_user_tasks(void *agent_ptr, TaskQueryParams params, std::string *http_body)
{
    LOG_CALL();
    write_diag(
        "bambu_network_get_user_tasks: dev_id=" + params.dev_id +
        " status=" + std::to_string(params.status) +
        " offset=" + std::to_string(params.offset) +
        " limit=" + std::to_string(params.limit)
    );
    if (http_body) {
        *http_body = "{\"total\":0,\"hits\":[]}";
    }
    return 0;
}

int bambu_network_start_sdcard_print(void *agent_ptr, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    LOG_CALL_ARGS("%s %s %s", params.project_name.c_str(), params.filename.c_str(), params.dst_file.c_str());
    log_print_params("bambu_network_start_sdcard_print", params);

    const std::string plate_path = !params.dst_file.empty() ? params.dst_file : BambuPlugin::resolve_plate_metadata_path(params);
    std::string remote_filename = BambuPlugin::basename_or_empty(plate_path);
    if (remote_filename.empty()) {
        remote_filename = BambuPlugin::resolve_remote_filename(params);
    }

    report_update_status(update_fn, PrintingStageCreate, BAMBU_NETWORK_SUCCESS, "Preparing print");
    if (cancel_fn && cancel_fn()) {
        report_update_status(update_fn, PrintingStageERROR, BAMBU_NETWORK_ERR_CANCELED, "Canceled");
        return BAMBU_NETWORK_ERR_CANCELED;
    }

    report_update_status(update_fn, PrintingStageSending, BAMBU_NETWORK_SUCCESS, "Sending print command");
    std::string json = BambuPlugin::build_project_file_command(params, remote_filename, plate_path);
    write_diag("bambu_network_start_sdcard_print command=" + json);

    const int send_result = bambu_network_rs_send(params.dev_id, json);
    write_diag("bambu_network_start_sdcard_print send_result=" + std::to_string(send_result));
    if (send_result != BAMBU_NETWORK_SUCCESS) {
        report_update_status(update_fn, PrintingStageERROR, send_result, "Send failed");
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }

    report_update_status(update_fn, PrintingStageFinished, BAMBU_NETWORK_SUCCESS, "Print submitted");
    return BAMBU_NETWORK_SUCCESS;
}

int bambu_network_update_cert(void *agent_ptr)
{
    LOG_CALL();
    write_diag("bambu_network_update_cert");
    return 0;
}

int bambu_network_set_server_callback(void *agent_ptr, OnServerErrFn fn)
{
    LOG_CALL();
    write_diag("bambu_network_set_server_callback");
    on_server_error = fn;
    return 0;
}

int bambu_network_ping_bind(void *agent_ptr, std::string ping_code)
{
    LOG_CALL();
    write_diag("bambu_network_ping_bind: code=" + ping_code);
    return 0;
}

int bambu_network_bind_detect(void *agent_ptr, std::string dev_ip, std::string sec_link, detectResult &detect)
{
    LOG_CALL();
    write_diag("bambu_network_bind_detect: dev_ip=" + dev_ip);
    detect.result_msg = "success";
    detect.command = "bind_detect";
    detect.dev_id = selected_device;
    detect.connect_type = "lan";
    return 0;
}

int bambu_network_get_setting_list2(void *agent_ptr, std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    write_diag("bambu_network_get_setting_list2: bundle_version=" + bundle_version);
    if (cancel_fn && cancel_fn()) {
        return BAMBU_NETWORK_ERR_CANCELED;
    }
    if (pro_fn) {
        pro_fn(100);
    }
    return 0;
}

int bambu_network_get_mw_user_preference(void *agent_ptr, std::function<void(std::string)> callback)
{
    LOG_CALL();
    write_diag(
        std::string("bambu_network_get_mw_user_preference callback=") +
        (callback ? "set" : "null")
    );
    return 0;
}

int bambu_network_get_mw_user_4ulist(void *agent_ptr, int seed, int limit, std::function<void(std::string)> callback)
{
    LOG_CALL();
    write_diag(
        "bambu_network_get_mw_user_4ulist: seed=" + std::to_string(seed) +
        " limit=" + std::to_string(limit)
    );
    return 0;
}
