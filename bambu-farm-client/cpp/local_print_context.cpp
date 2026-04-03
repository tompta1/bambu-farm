#include "local_print_context.hpp"
#include "print_job.hpp"

namespace BambuPlugin
{
    void LocalPrintContextStore::load(const std::string &config_dir, const DiagLogFn &log_fn)
    {
        auto loaded = BambuPlugin::load_local_print_contexts(config_dir, log_fn);
        std::lock_guard<std::mutex> lock(mutex_);
        contexts_ = std::move(loaded);
    }

    void LocalPrintContextStore::clear(const std::string &dev_id, const std::string &config_dir, const DiagLogFn &log_fn)
    {
        if (dev_id.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            contexts_.erase(dev_id);
        }
        BambuPlugin::persist_local_print_contexts(config_dir, contexts_, log_fn);
    }

    void LocalPrintContextStore::update(const LocalPrintContext &context, const std::string &config_dir, const DiagLogFn &log_fn)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            contexts_[context.dev_id] = context;
        }
        BambuPlugin::persist_local_print_contexts(config_dir, contexts_, log_fn);
        if (log_fn) {
            log_fn(
                "update_local_print_context: dev_id=" + context.dev_id +
                " project_id=" + context.project_id +
                " profile_id=" + context.profile_id +
                " subtask_id=" + context.subtask_id +
                " plate_index=" + std::to_string(context.plate_index) +
                " thumbnail_url=" + context.thumbnail_url
            );
        }
    }

    bool LocalPrintContextStore::lookup_subtask_id(const std::string &subtask_id, LocalPrintContext &out) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &[dev_id, context] : contexts_) {
            if (context.active && context.subtask_id == subtask_id) {
                out = context;
                return true;
            }
        }
        return false;
    }

    bool LocalPrintContextStore::build_subtask_info(
        const std::string &subtask_id,
        std::string *task_json,
        unsigned int *http_code,
        std::string *http_body
    ) const
    {
        LocalPrintContext context;
        if (!lookup_subtask_id(subtask_id, context)) {
            if (http_code) {
                *http_code = 404;
            }
            if (http_body) {
                *http_body = "{\"error\":\"not_found\"}";
            }
            if (task_json) {
                task_json->clear();
            }
            return false;
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
        return true;
    }

    std::string LocalPrintContextStore::rewrite_status_message(const std::string &device_id, const std::string &message) const
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
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = contexts_.find(device_id);
            if (it == contexts_.end() || !it->second.active) {
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
}
