#ifndef PLUGIN_API_H
#define PLUGIN_API_H

#include "bambu_networking.hpp"
#include "ProjectTask.hpp"

#include <string>
#include <map>
#include <cstddef>

using namespace BBL;
using namespace Slic3r;

#define EXPORT extern "C" __attribute__((visibility("default")))

EXPORT __attribute__((constructor)) void bambu_init();

using tchar = char;
using Bambu_Tunnel = void *;
using Logger = void (*)(void *context, int level, tchar const *msg);

struct Bambu_StreamInfo
{
    int type;
    int sub_type;
    int format_type;
    int format_size;
    int max_frame_size;
    unsigned char const *format_buffer;
};

struct Bambu_Sample
{
    int itrack;
    int size;
    int flags;
    unsigned char const *buffer;
    unsigned long long decode_time;
};

enum Bambu_Error
{
    Bambu_success = 0,
    Bambu_stream_end = 1,
    Bambu_would_block = 2,
    Bambu_buffer_limit = 3,
};

__attribute__((visibility("default"))) void bambu_network_cb_printer_available(const std::string &device_id);
__attribute__((visibility("default"))) void bambu_network_cb_message_recv(const std::string &device_id, const std::string &message);
__attribute__((visibility("default"))) void bambu_network_cb_connected(const std::string &device_id);
__attribute__((visibility("default"))) void bambu_network_cb_disconnected(const std::string &device_id, int status, const std::string &message);

EXPORT bool bambu_network_check_debug_consistent(bool is_debug);
EXPORT std::string bambu_network_get_version(void);
EXPORT void *bambu_network_create_agent(std::string log_dir);
EXPORT int bambu_network_destroy_agent(void *agent);
EXPORT int bambu_network_init_log(void *agent);
EXPORT int bambu_network_set_config_dir(void *agent, std::string config_dir);
EXPORT int bambu_network_set_cert_file(void *agent, std::string folder, std::string filename);
EXPORT int bambu_network_set_country_code(void *agent, std::string country_code);
EXPORT int bambu_network_start(void *agent);
EXPORT int bambu_network_set_on_ssdp_msg_fn(void *agent, OnMsgArrivedFn fn);
EXPORT int bambu_network_set_on_user_login_fn(void *agent, OnUserLoginFn fn);
EXPORT int bambu_network_set_on_printer_connected_fn(void *agent, OnPrinterConnectedFn fn);
EXPORT int bambu_network_set_on_server_connected_fn(void *agent, OnServerConnectedFn fn);
EXPORT int bambu_network_set_on_http_error_fn(void *agent, OnHttpErrorFn fn);
EXPORT int bambu_network_set_get_country_code_fn(void *agent, GetCountryCodeFn fn);
EXPORT int bambu_network_set_on_message_fn(void *agent, OnMessageFn fn);
EXPORT int bambu_network_set_on_local_connect_fn(void *agent, OnLocalConnectedFn fn);
EXPORT int bambu_network_set_on_local_message_fn(void *agent, OnMessageFn fn);
EXPORT int bambu_network_set_queue_on_main_fn(void *agent, QueueOnMainFn fn);
EXPORT int bambu_network_connect_server(void *agent);
EXPORT bool bambu_network_is_server_connected(void *agent);
EXPORT int bambu_network_refresh_connection(void *agent);
EXPORT int bambu_network_start_subscribe(void *agent, std::string module);
EXPORT int bambu_network_stop_subscribe(void *agent, std::string module);
EXPORT int bambu_network_send_message(void *agent, std::string dev_id, std::string json_str, int qos, int flag);
EXPORT int bambu_network_connect_printer(void *agent, std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl);
EXPORT int bambu_network_disconnect_printer(void *agent);
EXPORT int bambu_network_send_message_to_printer(void *agent, std::string dev_id, std::string json_str, int qos, int flag);
EXPORT bool bambu_network_start_discovery(void *agent, bool start, bool sending);
EXPORT int bambu_network_change_user(void *agent, std::string user_info);
EXPORT bool bambu_network_is_user_login(void *agent);
EXPORT int bambu_network_user_logout(void *agent, bool request);
EXPORT std::string bambu_network_get_user_id(void *agent);
EXPORT std::string bambu_network_get_user_name(void *agent);
EXPORT std::string bambu_network_get_user_avatar(void *agent);
EXPORT std::string bambu_network_get_user_nickanme(void *agent);
EXPORT std::string bambu_network_build_login_cmd(void *agent);
EXPORT std::string bambu_network_build_logout_cmd(void *agent);
EXPORT std::string bambu_network_build_login_info(void *agent);
EXPORT int bambu_network_bind(void *agent, std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn);
EXPORT int bambu_network_unbind(void *agent, std::string dev_id);
EXPORT std::string bambu_network_get_bambulab_host(void *agent);
EXPORT std::string bambu_network_get_user_selected_machine(void *agent);
EXPORT int bambu_network_set_user_selected_machine(void *agent, std::string dev_id);
EXPORT int bambu_network_start_print(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
EXPORT int bambu_network_start_local_print_with_record(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
EXPORT int bambu_network_start_send_gcode_to_sdcard(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
EXPORT int bambu_network_start_local_print(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
EXPORT int bambu_network_get_user_presets(void *agent, std::map<std::string, std::map<std::string, std::string>> *user_presets);
EXPORT std::string bambu_network_request_setting_id(void *agent, std::string name, std::map<std::string, std::string> *values_map, unsigned int *http_code);
EXPORT int bambu_network_put_setting(void *agent, std::string setting_id, std::string name, std::map<std::string, std::string> *values_map, unsigned int *http_code);
EXPORT int bambu_network_get_setting_list(void *agent, std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn);
EXPORT int bambu_network_delete_setting(void *agent, std::string setting_id);
EXPORT std::string bambu_network_get_studio_info_url(void *agent);
EXPORT int bambu_network_set_extra_http_header(void *agent, std::map<std::string, std::string> extra_headers);
EXPORT int bambu_network_get_my_message(void *agent, int type, int after, int limit, unsigned int *http_code, std::string *http_body);
EXPORT int bambu_network_check_user_task_report(void *agent, int *task_id, bool *printable);
EXPORT int bambu_network_get_user_print_info(void *agent, unsigned int *http_code, std::string *http_body);
EXPORT int bambu_network_get_printer_firmware(void *agent, std::string dev_id, unsigned *http_code, std::string *http_body);
EXPORT int bambu_network_get_task_plate_index(void *agent, std::string task_id, int *plate_index);
EXPORT int bambu_network_get_user_info(void *agent, int *identifier);
EXPORT int bambu_network_request_bind_ticket(void *agent, std::string *ticket);
EXPORT int bambu_network_get_slice_info(void *agent, std::string project_id, std::string profile_id, int plate_index, std::string *slice_json);
EXPORT int bambu_network_query_bind_status(void *agent, std::vector<std::string> query_list, unsigned int *http_code, std::string *http_body);
EXPORT int bambu_network_modify_printer_name(void *agent, std::string dev_id, std::string dev_name);
EXPORT int bambu_network_get_camera_url(void *agent, std::string dev_id, std::function<void(std::string)> callback);
EXPORT int bambu_network_get_design_staffpick(void *agent, int offset, int limit, std::function<void(std::string)> callback);
EXPORT int bambu_network_start_publish(void *agent, PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string *out);
EXPORT int bambu_network_get_profile_3mf(void *agent, BBLProfile *profile);
EXPORT int bambu_network_get_model_publish_url(void *agent, std::string *url);
EXPORT int bambu_network_get_subtask(void *agent, BBLModelTask *task, OnGetSubTaskFn getsub_fn);
EXPORT int bambu_network_get_model_mall_home_url(void *agent, std::string *url);
EXPORT int bambu_network_get_model_mall_detail_url(void *agent, std::string *url, std::string id);
EXPORT int bambu_network_get_my_profile(void *agent, std::string token, unsigned int *http_code, std::string *http_body);
EXPORT int bambu_network_track_enable(void *agent, bool enable);
EXPORT int bambu_network_track_event(void *agent, std::string evt_key, std::string content);
EXPORT int bambu_network_track_header(void *agent, std::string header);
EXPORT int bambu_network_track_update_property(void *agent, std::string name, std::string value, std::string type);
EXPORT int bambu_network_track_get_property(void *agent, std::string name, std::string &value, std::string type);

// Compatibility exports found in newer proprietary plugins. These are stubs for
// loader/ABI compatibility until the underlying features are implemented.
EXPORT int bambu_network_set_on_subscribe_failure_fn(void *agent, GetSubscribeFailureFn fn);
EXPORT int bambu_network_set_on_user_message_fn(void *agent, OnMessageFn fn);
EXPORT void bambu_network_install_device_cert(void *agent, std::string dev_id, bool lan_only);
EXPORT void bambu_network_enable_multi_machine(void *agent, bool enable);
EXPORT int bambu_network_add_subscribe(void *agent, std::vector<std::string> dev_list);
EXPORT int bambu_network_del_subscribe(void *agent, std::vector<std::string> dev_list);
EXPORT int bambu_network_report_consent(...);
EXPORT int bambu_network_get_user_tasks(void *agent, TaskQueryParams params, std::string *http_body);
EXPORT int bambu_network_get_oss_config(...);
EXPORT int bambu_network_get_my_token(...);
EXPORT int bambu_network_start_sdcard_print(void *agent, PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
EXPORT int bambu_network_track_remove_files(...);
EXPORT int bambu_network_update_cert(void *agent);
EXPORT int bambu_network_set_server_callback(void *agent, OnServerErrFn fn);
EXPORT int bambu_network_ping_bind(void *agent, std::string ping_code);
EXPORT int bambu_network_bind_detect(void *agent, std::string dev_ip, std::string sec_link, detectResult &detect);
EXPORT int bambu_network_check_user_report(...);
EXPORT int bambu_network_get_subtask_info(void *agent, std::string subtask_id, std::string *task_json, unsigned int *http_code, std::string *http_body);
EXPORT int bambu_network_get_camera_url_for_golive(...);
EXPORT int bambu_network_get_mw_user_preference(void *agent, std::function<void(std::string)> callback);
EXPORT int bambu_network_get_mw_user_4ulist(void *agent, int seed, int limit, std::function<void(std::string)> callback);
EXPORT int bambu_network_get_hms_snapshot(...);
EXPORT int bambu_network_get_model_rating_id(...);
EXPORT int bambu_network_put_model_mall_rating(...);
EXPORT int bambu_network_get_model_instance_id(...);
EXPORT int bambu_network_get_model_mall_rating(...);
EXPORT int bambu_network_put_rating_picture_oss(...);
EXPORT int bambu_network_del_rating_picture_oss(...);
EXPORT int bambu_network_get_setting_list2(void *agent, std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn);

EXPORT int Bambu_Create(Bambu_Tunnel *tunnel, char const *path);
EXPORT void Bambu_SetLogger(Bambu_Tunnel tunnel, Logger logger, void *context);
EXPORT int Bambu_Open(Bambu_Tunnel tunnel);
EXPORT int Bambu_StartStream(Bambu_Tunnel tunnel, bool video);
EXPORT int Bambu_StartStreamEx(Bambu_Tunnel tunnel, int type);
EXPORT int Bambu_GetStreamCount(Bambu_Tunnel tunnel);
EXPORT int Bambu_GetStreamInfo(Bambu_Tunnel tunnel, int index, Bambu_StreamInfo *info);
EXPORT unsigned long Bambu_GetDuration(Bambu_Tunnel tunnel);
EXPORT int Bambu_Seek(Bambu_Tunnel tunnel, unsigned long time);
EXPORT int Bambu_ReadSample(Bambu_Tunnel tunnel, Bambu_Sample *sample);
EXPORT int Bambu_SendMessage(Bambu_Tunnel tunnel, int ctrl, char const *data, int len);
EXPORT int Bambu_RecvMessage(Bambu_Tunnel tunnel, int *ctrl, char *data, int *len);
EXPORT void Bambu_Close(Bambu_Tunnel tunnel);
EXPORT void Bambu_Destroy(Bambu_Tunnel tunnel);
EXPORT int Bambu_Init();
EXPORT void Bambu_Deinit();
EXPORT char const *Bambu_GetLastErrorMsg();
EXPORT void Bambu_FreeLogMsg(tchar const *msg);

#endif // PLUGIN_API_H
