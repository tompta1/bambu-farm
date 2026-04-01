#ifndef PRINT_JOB_H
#define PRINT_JOB_H

#include "bambu_networking.hpp"

#include <string>

namespace BambuPlugin
{
    struct LoginState
    {
        std::string user_id;
        std::string user_name;
        std::string nickname;
        std::string avatar;
        bool logged_in{true};
    };

    std::string json_escape(const std::string &value);
    std::string basename_or_empty(const std::string &path);
    std::string sanitize_remote_filename(std::string value);
    std::string normalize_print_package_name(std::string value);
    std::string resolve_display_subtask_name(const BBL::PrintParams &params, const std::string &remote_filename);
    std::string resolve_remote_filename(const BBL::PrintParams &params);
    std::string resolve_plate_metadata_path(const BBL::PrintParams &params);
    std::string build_project_file_command(
        const BBL::PrintParams &params,
        const std::string &remote_filename,
        const std::string &plate_path
    );
    std::string build_logged_in_login_cmd(const LoginState &state);
    std::string build_logout_cmd(const std::string &provider);
    std::string build_login_info(const LoginState &state, const std::string &backend_url, const std::string &auth_url);
}

#endif // PRINT_JOB_H
