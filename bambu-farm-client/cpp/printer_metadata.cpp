#include "printer_metadata.hpp"
#include "print_job.hpp"

namespace
{
    bool find_json_string_value_range(const std::string &json, const std::string &key, size_t &value_start, size_t &value_end)
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
}

namespace BambuPlugin
{
    std::string json_string_field(const std::string &json, const std::string &key)
    {
        size_t value_start = 0;
        size_t value_end = 0;
        if (!find_json_string_value_range(json, key, value_start, value_end)) {
            return "";
        }
        return json.substr(value_start, value_end - value_start);
    }

    void PrinterMetadataStore::load(const std::string &config_dir, const DiagLogFn &log_fn)
    {
        auto loaded = BambuPlugin::load_printer_name_overrides(config_dir, log_fn);
        std::lock_guard<std::mutex> lock(mutex_);
        printer_name_overrides_ = std::move(loaded);
    }

    void PrinterMetadataStore::set_name_override(
        const std::string &dev_id,
        const std::string &dev_name,
        const std::string &config_dir,
        const DiagLogFn &log_fn
    )
    {
        std::unordered_map<std::string, std::string> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (dev_name.empty()) {
                printer_name_overrides_.erase(dev_id);
            } else {
                printer_name_overrides_[dev_id] = dev_name;
            }
            snapshot = printer_name_overrides_;
        }
        BambuPlugin::persist_printer_name_overrides(config_dir, snapshot, log_fn);
    }

    std::string PrinterMetadataStore::apply_name_override(const std::string &json) const
    {
        const std::string dev_id = BambuPlugin::json_string_field(json, "dev_id");
        if (dev_id.empty()) {
            return json;
        }

        std::string override_name;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = printer_name_overrides_.find(dev_id);
            if (it == printer_name_overrides_.end() || it->second.empty()) {
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

    bool PrinterMetadataStore::json_matches_device(const std::string &json, const std::string &dev_id) const
    {
        return !dev_id.empty() && BambuPlugin::json_string_field(json, "dev_id") == dev_id;
    }
}
