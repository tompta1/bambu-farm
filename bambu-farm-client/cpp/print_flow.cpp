#include "print_flow.hpp"
#include "print_job.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

namespace
{
    void report_update_status(const BambuPlugin::PrintFlowDeps &deps, OnUpdateStatusFn update_fn, int status, int code, const std::string &message)
    {
        if (deps.log_diag) {
            deps.log_diag(
                "report_update_status: status=" + std::to_string(status) +
                " code=" + std::to_string(code) +
                " message=" + message +
                " callback=" + (update_fn ? "set" : "null"));
        }
        if (update_fn) {
            update_fn(status, code, message);
        }
    }

    void log_local_file_probe_default(const BambuPlugin::PrintFlowDeps &deps, const char *context, const std::string &path)
    {
        errno = 0;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            const int open_errno = errno;
            if (deps.log_diag) {
                deps.log_diag(
                    std::string(context) +
                    " local_file_probe=open_failed path=" + path +
                    " errno=" + std::to_string(open_errno) +
                    " error=" + std::strerror(open_errno));
            }
            return;
        }

        const std::streamoff end_pos = file.tellg();
        if (deps.log_diag) {
            deps.log_diag(
                std::string(context) +
                " local_file_probe=open_ok path=" + path +
                " size=" + std::to_string(static_cast<long long>(end_pos)));
        }
    }

    void sleep_before_send_default()
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

namespace BambuPlugin
{
    int start_local_print(
        const BBL::PrintParams &params,
        OnUpdateStatusFn update_fn,
        WasCancelledFn cancel_fn,
        OnWaitFn wait_fn,
        const PrintFlowDeps &deps
    )
    {
        if (deps.log_print_params) {
            deps.log_print_params("bambu_network_start_local_print", params);
        }
        report_update_status(deps, update_fn, PrintingStageCreate, BAMBU_NETWORK_SUCCESS, "Preparing print");

        if (cancel_fn && cancel_fn()) {
            report_update_status(deps, update_fn, PrintingStageERROR, BAMBU_NETWORK_ERR_CANCELED, "Canceled");
            return BAMBU_NETWORK_ERR_CANCELED;
        }

        const std::string remote_filename = BambuPlugin::resolve_remote_filename(params);
        const std::string plate_path = BambuPlugin::resolve_plate_metadata_path(params);

        report_update_status(deps, update_fn, PrintingStageUpload, BAMBU_NETWORK_SUCCESS, "Uploading to printer");
        if (deps.log_local_file_probe) {
            deps.log_local_file_probe("bambu_network_start_local_print", params.filename);
        } else {
            log_local_file_probe_default(deps, "bambu_network_start_local_print", params.filename);
        }
        const int upload_result = deps.upload_file ? deps.upload_file(params.dev_id, params.filename, remote_filename) : -1;
        if (deps.log_diag) {
            deps.log_diag(
                "bambu_network_start_local_print upload_result=" +
                std::to_string(upload_result) +
                " remote_filename=" + remote_filename +
                " plate_path=" + plate_path);
        }
        if (upload_result != BAMBU_NETWORK_SUCCESS) {
            report_update_status(deps, update_fn, PrintingStageERROR, upload_result, "Upload failed");
            return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
        }

        if (deps.update_local_context) {
            deps.update_local_context(params, remote_filename, plate_path);
        }

        if (cancel_fn && cancel_fn()) {
            report_update_status(deps, update_fn, PrintingStageERROR, BAMBU_NETWORK_ERR_CANCELED, "Canceled");
            return BAMBU_NETWORK_ERR_CANCELED;
        }

        report_update_status(deps, update_fn, PrintingStageSending, BAMBU_NETWORK_SUCCESS, "Sending print command");
        if (deps.sleep_before_send) {
            deps.sleep_before_send();
        } else {
            sleep_before_send_default();
        }

        const std::string json = BambuPlugin::build_project_file_command(params, remote_filename, plate_path);
        if (deps.log_diag) {
            deps.log_diag("bambu_network_start_local_print command=" + json);
        }

        const int send_result = deps.send_message ? deps.send_message(params.dev_id, json) : -1;
        if (deps.log_diag) {
            deps.log_diag("bambu_network_start_local_print send_result=" + std::to_string(send_result));
        }
        if (send_result != BAMBU_NETWORK_SUCCESS) {
            report_update_status(deps, update_fn, PrintingStageERROR, send_result, "Send failed");
            return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
        }

        if (wait_fn) {
            report_update_status(deps, update_fn, PrintingStageWaitPrinter, BAMBU_NETWORK_SUCCESS, "Waiting for printer");
            const bool wait_ok = wait_fn(PrintingStageWaitPrinter, "{}");
            if (deps.log_diag) {
                deps.log_diag(std::string("bambu_network_start_local_print wait_result=") + (wait_ok ? "true" : "false"));
            }
            if (!wait_ok) {
                report_update_status(deps, update_fn, PrintingStageERROR, BAMBU_NETWORK_ERR_TIMEOUT, "Wait failed");
                return BAMBU_NETWORK_ERR_TIMEOUT;
            }
        }

        report_update_status(deps, update_fn, PrintingStageFinished, BAMBU_NETWORK_SUCCESS, "Print submitted");
        return BAMBU_NETWORK_SUCCESS;
    }

    int start_send_gcode_to_sdcard(
        const BBL::PrintParams &params,
        OnUpdateStatusFn update_fn,
        const PrintFlowDeps &deps
    )
    {
        if (deps.log_print_params) {
            deps.log_print_params("bambu_network_start_send_gcode_to_sdcard", params);
        }
        report_update_status(deps, update_fn, PrintingStageCreate, BAMBU_NETWORK_SUCCESS, "Preparing upload");

        const std::string remote_filename = BambuPlugin::resolve_remote_filename(params);
        report_update_status(deps, update_fn, PrintingStageUpload, BAMBU_NETWORK_SUCCESS, "Uploading to printer");
        if (deps.log_local_file_probe) {
            deps.log_local_file_probe("bambu_network_start_send_gcode_to_sdcard", params.filename);
        } else {
            log_local_file_probe_default(deps, "bambu_network_start_send_gcode_to_sdcard", params.filename);
        }
        const int upload_result = deps.upload_file ? deps.upload_file(params.dev_id, params.filename, remote_filename) : -1;
        if (deps.log_diag) {
            deps.log_diag(
                "bambu_network_start_send_gcode_to_sdcard upload_result=" +
                std::to_string(upload_result) +
                " remote_filename=" + remote_filename);
        }

        if (upload_result != BAMBU_NETWORK_SUCCESS) {
            report_update_status(deps, update_fn, PrintingStageERROR, upload_result, "Upload failed");
            return BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;
        }

        report_update_status(deps, update_fn, PrintingStageFinished, BAMBU_NETWORK_SUCCESS, "Upload finished");
        return BAMBU_NETWORK_SUCCESS;
    }

    int start_sdcard_print(
        const BBL::PrintParams &params,
        OnUpdateStatusFn update_fn,
        WasCancelledFn cancel_fn,
        const PrintFlowDeps &deps
    )
    {
        if (deps.log_print_params) {
            deps.log_print_params("bambu_network_start_sdcard_print", params);
        }

        const std::string plate_path = !params.dst_file.empty() ? params.dst_file : BambuPlugin::resolve_plate_metadata_path(params);
        std::string remote_filename = BambuPlugin::basename_or_empty(plate_path);
        if (remote_filename.empty()) {
            remote_filename = BambuPlugin::resolve_remote_filename(params);
        }

        report_update_status(deps, update_fn, PrintingStageCreate, BAMBU_NETWORK_SUCCESS, "Preparing print");
        if (cancel_fn && cancel_fn()) {
            report_update_status(deps, update_fn, PrintingStageERROR, BAMBU_NETWORK_ERR_CANCELED, "Canceled");
            return BAMBU_NETWORK_ERR_CANCELED;
        }

        report_update_status(deps, update_fn, PrintingStageSending, BAMBU_NETWORK_SUCCESS, "Sending print command");
        const std::string json = BambuPlugin::build_project_file_command(params, remote_filename, plate_path);
        if (deps.log_diag) {
            deps.log_diag("bambu_network_start_sdcard_print command=" + json);
        }

        const int send_result = deps.send_message ? deps.send_message(params.dev_id, json) : -1;
        if (deps.log_diag) {
            deps.log_diag("bambu_network_start_sdcard_print send_result=" + std::to_string(send_result));
        }
        if (send_result != BAMBU_NETWORK_SUCCESS) {
            report_update_status(deps, update_fn, PrintingStageERROR, send_result, "Send failed");
            return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
        }

        report_update_status(deps, update_fn, PrintingStageFinished, BAMBU_NETWORK_SUCCESS, "Print submitted");
        return BAMBU_NETWORK_SUCCESS;
    }
}
