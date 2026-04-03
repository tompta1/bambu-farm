#ifndef PRINTER_METADATA_H
#define PRINTER_METADATA_H

#include "local_state.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

namespace BambuPlugin
{
    std::string json_string_field(const std::string &json, const std::string &key);

    class PrinterMetadataStore
    {
    public:
        void load(const std::string &config_dir, const DiagLogFn &log_fn);
        void set_name_override(
            const std::string &dev_id,
            const std::string &dev_name,
            const std::string &config_dir,
            const DiagLogFn &log_fn
        );
        std::string apply_name_override(const std::string &json) const;
        bool json_matches_device(const std::string &json, const std::string &dev_id) const;

    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::string> printer_name_overrides_;
    };
}

#endif // PRINTER_METADATA_H
