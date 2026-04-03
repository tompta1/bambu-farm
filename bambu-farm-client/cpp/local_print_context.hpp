#ifndef LOCAL_PRINT_CONTEXT_H
#define LOCAL_PRINT_CONTEXT_H

#include "local_state.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace BambuPlugin
{
    using DiagLogFn = std::function<void(const std::string &)>;

    class LocalPrintContextStore
    {
    public:
        void load(const std::string &config_dir, const DiagLogFn &log_fn);
        void clear(const std::string &dev_id, const std::string &config_dir, const DiagLogFn &log_fn);
        void update(const LocalPrintContext &context, const std::string &config_dir, const DiagLogFn &log_fn);
        bool lookup_subtask_id(const std::string &subtask_id, LocalPrintContext &out) const;
        std::string rewrite_status_message(const std::string &device_id, const std::string &message) const;

    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::string, LocalPrintContext> contexts_;
    };
}

#endif // LOCAL_PRINT_CONTEXT_H
