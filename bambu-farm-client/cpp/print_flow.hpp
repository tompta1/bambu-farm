#ifndef PRINT_FLOW_H
#define PRINT_FLOW_H

#include "api.hpp"

#include <functional>
#include <string>

namespace BambuPlugin
{
    struct PrintFlowDeps
    {
        std::function<void(const std::string &)> log_diag;
        std::function<void(const char *, const BBL::PrintParams &)> log_print_params;
        std::function<void(const char *, const std::string &)> log_local_file_probe;
        std::function<int(const std::string &, const std::string &, const std::string &)> upload_file;
        std::function<int(const std::string &, const std::string &)> send_message;
        std::function<void(const BBL::PrintParams &, const std::string &, const std::string &)> update_local_context;
        std::function<void()> sleep_before_send;
    };

    int start_local_print(
        const BBL::PrintParams &params,
        OnUpdateStatusFn update_fn,
        WasCancelledFn cancel_fn,
        OnWaitFn wait_fn,
        const PrintFlowDeps &deps
    );

    int start_send_gcode_to_sdcard(
        const BBL::PrintParams &params,
        OnUpdateStatusFn update_fn,
        const PrintFlowDeps &deps
    );

    int start_sdcard_print(
        const BBL::PrintParams &params,
        OnUpdateStatusFn update_fn,
        WasCancelledFn cancel_fn,
        const PrintFlowDeps &deps
    );
}

#endif // PRINT_FLOW_H
