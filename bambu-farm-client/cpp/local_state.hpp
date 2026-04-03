#ifndef LOCAL_STATE_H
#define LOCAL_STATE_H

#include <functional>
#include <string>
#include <unordered_map>

namespace BambuPlugin
{
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

    using DiagLogFn = std::function<void(const std::string &)>;

    std::string local_thumbnail_cache_dir(const std::string &config_dir);
    void persist_local_print_contexts(
        const std::string &config_dir,
        const std::unordered_map<std::string, LocalPrintContext> &contexts,
        const DiagLogFn &log_fn
    );
    std::unordered_map<std::string, LocalPrintContext> load_local_print_contexts(
        const std::string &config_dir,
        const DiagLogFn &log_fn
    );

    void persist_printer_name_overrides(
        const std::string &config_dir,
        const std::unordered_map<std::string, std::string> &printer_name_overrides,
        const DiagLogFn &log_fn
    );
    std::unordered_map<std::string, std::string> load_printer_name_overrides(
        const std::string &config_dir,
        const DiagLogFn &log_fn
    );

    void persist_selected_device(
        const std::string &config_dir,
        const std::string &selected_device,
        const DiagLogFn &log_fn
    );
    std::string load_selected_device(const std::string &config_dir, const DiagLogFn &log_fn);
}

#endif // LOCAL_STATE_H
