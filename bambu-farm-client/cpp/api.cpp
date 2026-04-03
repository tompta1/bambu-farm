#include "api.hpp"
#include "callback_registry.hpp"
#include "cloud_compat.hpp"
#include "local_print_context.hpp"
#include "local_state.hpp"
#include "printer_metadata.hpp"
#include "print_flow.hpp"
#include "print_job.hpp"
#include "session_state.hpp"
#include "tunnel_bridge.hpp"
#include "stdio.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <execinfo.h>
#include <mutex>
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

static std::string config_dir_path;
static std::string ca_path;
static size_t user_login_log_counter = 0;
static size_t build_login_cmd_log_counter = 0;
static const char *kLanBackendUrl = "http://127.0.0.1:47403";
static std::atomic<bool> lan_user_logged_in{true};
static std::string preview_payload(const std::string &message, size_t limit = 1024);
static BambuPlugin::SessionState session_state;
static BambuPlugin::CallbackRegistry callback_registry;
static BambuPlugin::PrinterMetadataStore printer_metadata_store;

static BambuPlugin::LocalPrintContextStore local_print_context_store;

static void write_diag(const std::string &message);

static std::string synthetic_local_id(const char *prefix, const std::string &dev_id, const std::string &remote_filename)
{
    std::hash<std::string> hasher;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string(prefix) + "-" + std::to_string(hasher(dev_id + "|" + remote_filename + "|" + std::to_string(now)));
}

static void update_local_print_context(
    const BBL::PrintParams &params,
    const std::string &remote_filename,
    const std::string &plate_path)
{
    BambuPlugin::LocalPrintContext context;
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
        BambuPlugin::local_thumbnail_cache_dir(config_dir_path));
    context.thumbnail_url = std::string(thumbnail_url.c_str(), thumbnail_url.size());
    context.active = true;
    local_print_context_store.update(context, config_dir_path, write_diag);
}

static bool lookup_local_print_context_by_subtask_id(const std::string &subtask_id, BambuPlugin::LocalPrintContext &out)
{
    return local_print_context_store.lookup_subtask_id(subtask_id, out);
}

static std::string rewrite_local_print_status_message(const std::string &device_id, const std::string &message)
{
    return local_print_context_store.rewrite_status_message(device_id, message);
}

static void persist_selected_device()
{
    BambuPlugin::persist_selected_device(config_dir_path, session_state.selected_device(), write_diag);
}

static void load_selected_device()
{
    std::string loaded = BambuPlugin::load_selected_device(config_dir_path, write_diag);
    if (loaded.empty()) {
        return;
    }

    session_state.load_selected_device(loaded);
}

static bool printer_json_matches_selected_device(const std::string &json)
{
    return printer_metadata_store.json_matches_device(json, session_state.selected_device());
}

static void connect_selected_device_if_pending(const std::string &json)
{
    if (!printer_json_matches_selected_device(json)) {
        return;
    }

    const std::string target = session_state.claim_autoconnect_target(BambuPlugin::json_string_field(json, "dev_id"));
    if (target.empty()) {
        return;
    }
    write_diag("connect_selected_device_if_pending: autoconnect dev_id=" + target);
    callback_registry.dispatch_connected(target, write_diag);
    bambu_network_rs_connect(target, "");
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
    if (notify) {
        callback_registry.notify_user_login(logged_in);
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

void bambu_init() {
    LOG_CALL();
    write_diag("bambu_init");
}

int Bambu_Create(Bambu_Tunnel *tunnel, char const *path)
{
    return BambuPlugin::create_tunnel(tunnel, path, write_diag);
}

void Bambu_SetLogger(Bambu_Tunnel tunnel, Logger logger, void *context)
{
    BambuPlugin::set_tunnel_logger(tunnel, logger, context);
}

int Bambu_Open(Bambu_Tunnel tunnel)
{
    return BambuPlugin::open_tunnel(tunnel, write_diag);
}

int Bambu_StartStream(Bambu_Tunnel tunnel, bool video)
{
    return BambuPlugin::start_tunnel_stream(tunnel, video, write_diag);
}

int Bambu_StartStreamEx(Bambu_Tunnel tunnel, int type)
{
    return BambuPlugin::start_tunnel_stream_ex(tunnel, type);
}

int Bambu_GetStreamCount(Bambu_Tunnel tunnel)
{
    return BambuPlugin::tunnel_stream_count(tunnel);
}

int Bambu_GetStreamInfo(Bambu_Tunnel tunnel, int index, Bambu_StreamInfo *info)
{
    return BambuPlugin::tunnel_stream_info(tunnel, index, info);
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
    return BambuPlugin::send_tunnel_message(tunnel, ctrl, data, len, write_diag);
}

int Bambu_ReadSample(Bambu_Tunnel tunnel, Bambu_Sample *sample)
{
    return BambuPlugin::read_tunnel_sample(tunnel, sample, write_diag);
}

int Bambu_RecvMessage(Bambu_Tunnel tunnel, int *ctrl, char *data, int *len)
{
    return BambuPlugin::recv_tunnel_message(tunnel, ctrl, data, len);
}

void Bambu_Close(Bambu_Tunnel tunnel)
{
    BambuPlugin::close_tunnel(tunnel);
}

void Bambu_Destroy(Bambu_Tunnel tunnel)
{
    BambuPlugin::destroy_tunnel(tunnel);
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
    return BambuPlugin::last_tunnel_error_message();
}

void Bambu_FreeLogMsg(tchar const *msg)
{
    std::free(const_cast<tchar *>(msg));
}

void bambu_network_cb_printer_available(const std::string &json) {
    const std::string effective_json = printer_metadata_store.apply_name_override(json);
    const std::string dev_id = BambuPlugin::json_string_field(effective_json, "dev_id");
    write_diag(
        "bambu_network_cb_printer_available: dev_id=" + dev_id +
        " preview=\"" + preview_payload(effective_json, 512) + "\""
    );
    callback_registry.remember_printer_json(dev_id, effective_json);
    callback_registry.dispatch_printer_available(effective_json);
    connect_selected_device_if_pending(effective_json);
}
void bambu_network_cb_message_recv(const std::string &device_id, const std::string &message) {
    const std::string effective_message = rewrite_local_print_status_message(device_id, message);
    if (callback_registry.has_message_callbacks()) {
        write_diag(
            "bambu_network_cb_message_recv: dev_id=" + device_id +
            " msg_len=" + std::to_string(effective_message.size()) +
            " preview=\"" + preview_payload(effective_message) + "\""
        );
    }
    callback_registry.dispatch_message(device_id, effective_message, write_diag);
}
void bambu_network_cb_connected(const std::string &device_id) {
    write_diag("bambu_network_cb_connected: " + device_id);
    session_state.mark_connected(device_id);
    callback_registry.dispatch_connected(device_id, write_diag);
}

void bambu_network_cb_disconnected(const std::string &device_id, int status, const std::string &message) {
    write_diag(
        "bambu_network_cb_disconnected: dev_id=" + device_id +
        " status=" + std::to_string(status) +
        " message=" + message
    );
    callback_registry.dispatch_disconnected(
        status,
        device_id,
        message,
        write_diag,
        session_state.mark_disconnected(status == ConnectStatusFailed));
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
    printer_metadata_store.load(config_dir_path, write_diag);
    local_print_context_store.load(config_dir_path, write_diag);
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
    callback_registry.set_ssdp_callback(fn, write_diag, []() { bambu_network_rs_refresh_available_printers(); });
    return 0;
}
int bambu_network_set_on_user_login_fn(void *agent_ptr, OnUserLoginFn fn)
{
    LOG_CALL();
    callback_registry.set_user_login_callback(fn, lan_user_logged_in.load(), write_diag);
    return 0;
}
int bambu_network_set_on_printer_connected_fn(void *agent_ptr, OnPrinterConnectedFn fn)
{
    LOG_CALL();
    callback_registry.set_printer_connected_callback(fn, session_state.connected_selected_device(), write_diag);
    return 0;
}
int bambu_network_set_on_server_connected_fn(void *agent_ptr, OnServerConnectedFn fn)
{
    LOG_CALL();
    callback_registry.set_server_connected_callback(fn, write_diag);
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
    callback_registry.set_message_callback(fn);
    return 0;
}
int bambu_network_set_on_local_connect_fn(void *agent_ptr, OnLocalConnectedFn fn)
{
    LOG_CALL();
    callback_registry.set_local_connect_callback(fn, session_state.connected_selected_device(), write_diag);
    return 0;
}
int bambu_network_set_on_local_message_fn(void *agent_ptr, OnMessageFn fn)
{
    LOG_CALL();
    write_diag("bambu_network_set_on_local_message_fn");
    callback_registry.set_local_message_callback(fn);
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
    switch (session_state.begin_connect(dev_id)) {
    case BambuPlugin::ConnectBeginResult::AlreadyConnected:
        write_diag("bambu_network_connect_printer: already connected " + dev_id);
        return 0;
    case BambuPlugin::ConnectBeginResult::AlreadyInProgress:
        write_diag("bambu_network_connect_printer: connect already in progress " + dev_id);
        return 0;
    case BambuPlugin::ConnectBeginResult::Start:
        break;
    }
    write_diag("bambu_network_connect_printer: " + dev_id + "@" + dev_ip);
    persist_selected_device();
    bambu_network_rs_connect(dev_id, password);
    return 0;
}
int bambu_network_disconnect_printer(void *agent_ptr)
{
    LOG_CALL();
    if (session_state.should_ignore_disconnect()) {
        write_diag("bambu_network_disconnect_printer: ignoring disconnect during in-flight connect");
        return 0;
    }
    const std::string target = session_state.disconnect_target();
    if (!target.empty()) {
        write_diag("bambu_network_disconnect_printer: dev_id=" + target);
        bambu_network_rs_disconnect(target);
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
    const std::string target = session_state.disconnect_target();
    if (!target.empty()) {
        bambu_network_rs_disconnect(target);
        local_print_context_store.clear(target, config_dir_path, write_diag);
    }
    session_state.clear_after_logout();
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
    const std::string selected_device = session_state.selected_device();
    write_diag("bambu_network_get_user_selected_machine: " + selected_device);
    return selected_device;
}
int bambu_network_set_user_selected_machine(void *agent_ptr, std::string dev_id)
{
    write_diag("bambu_network_set_user_selected_machine: " + dev_id);
    session_state.set_user_selected_device(dev_id);
    persist_selected_device();
    return 0;
}
int bambu_network_start_print(void *agent_ptr, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    write_diag("bambu_network_start_print");
    BambuPlugin::PrintFlowDeps deps;
    deps.log_diag = write_diag;
    deps.log_print_params = log_print_params;
    deps.log_local_file_probe = log_local_file_probe;
    deps.upload_file = [](const std::string &dev_id, const std::string &filename, const std::string &remote_filename) {
        return bambu_network_rs_upload_file(dev_id, filename, remote_filename);
    };
    deps.send_message = [](const std::string &dev_id, const std::string &json) {
        return bambu_network_rs_send(dev_id, json);
    };
    deps.update_local_context = update_local_print_context;
    deps.sleep_before_send = []() { std::this_thread::sleep_for(std::chrono::seconds(1)); };
    return BambuPlugin::start_local_print(params, update_fn, cancel_fn, wait_fn, deps);
}
int bambu_network_start_local_print_with_record(void *agent_ptr, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    LOG_CALL();
    return bambu_network_start_print(agent_ptr, params, update_fn, cancel_fn, wait_fn);
}
int bambu_network_start_send_gcode_to_sdcard(void *agent_ptr, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    LOG_CALL_ARGS("%s %s %s %s", params.project_name.c_str(), params.filename.c_str(), params.ftp_file.c_str(), params.dst_file.c_str());
    BambuPlugin::PrintFlowDeps deps;
    deps.log_diag = write_diag;
    deps.log_print_params = log_print_params;
    deps.log_local_file_probe = log_local_file_probe;
    deps.upload_file = [](const std::string &dev_id, const std::string &filename, const std::string &remote_filename) {
        return bambu_network_rs_upload_file(dev_id, filename, remote_filename);
    };
    return BambuPlugin::start_send_gcode_to_sdcard(params, update_fn, deps);
}
int bambu_network_start_local_print(void *agent_ptr, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    LOG_CALL_ARGS("%s %s %s", params.project_name.c_str(), params.filename.c_str(), params.ams_mapping.c_str());
    return bambu_network_start_print(agent_ptr, params, update_fn, cancel_fn, nullptr);
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
    printer_metadata_store.set_name_override(dev_id, dev_name, config_dir_path, write_diag);

    std::string last_json;
    std::string updated_json;
    last_json = callback_registry.printer_json(dev_id);
    if (!last_json.empty()) {
        updated_json = printer_metadata_store.apply_name_override(last_json);
        callback_registry.remember_printer_json(dev_id, updated_json);
    }
    if (!updated_json.empty()) {
        callback_registry.dispatch_printer_available(updated_json);
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
    const std::string message = "Model publishing is not supported by the OSS LAN plugin";
    write_diag("bambu_network_start_publish: unsupported");
    if (out) {
        *out = BambuPlugin::unsupported_cloud_json("publish");
    }
    write_diag(
        "report_update_status: status=" + std::to_string(PrintingStageERROR) +
        " code=" + std::to_string(BAMBU_NETWORK_ERR_INVALID_RESULT) +
        " message=" + message +
        " callback=" + (update_fn ? "set" : "null"));
    if (update_fn) {
        update_fn(PrintingStageERROR, BAMBU_NETWORK_ERR_INVALID_RESULT, message);
    }
    return BAMBU_NETWORK_ERR_INVALID_RESULT;
}
int bambu_network_get_profile_3mf(void *agent_ptr, BBLProfile *profile)
{
    LOG_CALL();
    return 0;
}
int bambu_network_get_model_publish_url(void *agent_ptr, std::string *url)
{
    LOG_CALL();
    if (url) {
        *url = BambuPlugin::model_publish_notice_url(config_dir_path);
    }
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
    if (url) {
        *url = BambuPlugin::model_mall_home_notice_url(config_dir_path);
    }
    return 0;
}
int bambu_network_get_model_mall_detail_url(void *agent_ptr, std::string *url, std::string id)
{
    LOG_CALL();
    if (url) {
        *url = BambuPlugin::model_mall_detail_notice_url(config_dir_path, id);
    }
    return 0;
}
int bambu_network_get_my_profile(void *agent_ptr, std::string token, unsigned int *http_code, std::string *http_body)
{
    LOG_CALL();
    BambuPlugin::fill_unsupported_my_profile(http_code, http_body);
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
    callback_registry.set_subscribe_failure_callback(fn);
    return 0;
}

int bambu_network_set_on_user_message_fn(void *agent_ptr, OnMessageFn fn)
{
    LOG_CALL();
    write_diag("bambu_network_set_on_user_message_fn");
    callback_registry.set_user_message_callback(fn);
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
    BambuPlugin::LocalPrintContext context;
    if (!local_print_context_store.build_subtask_info(subtask_id, task_json, http_code, http_body) ||
        !lookup_local_print_context_by_subtask_id(subtask_id, context)) {
        write_diag("bambu_network_get_subtask_info: not found subtask_id=" + subtask_id);
        return -1;
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
        *http_body = BambuPlugin::unsupported_user_tasks_json();
    }
    return 0;
}

int bambu_network_start_sdcard_print(void *agent_ptr, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    LOG_CALL_ARGS("%s %s %s", params.project_name.c_str(), params.filename.c_str(), params.dst_file.c_str());
    BambuPlugin::PrintFlowDeps deps;
    deps.log_diag = write_diag;
    deps.log_print_params = log_print_params;
    deps.send_message = [](const std::string &dev_id, const std::string &json) {
        return bambu_network_rs_send(dev_id, json);
    };
    return BambuPlugin::start_sdcard_print(params, update_fn, cancel_fn, deps);
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
    callback_registry.set_server_error_callback(fn);
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
    detect.dev_id = session_state.selected_device();
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
