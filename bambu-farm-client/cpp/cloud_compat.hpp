#ifndef CLOUD_COMPAT_H
#define CLOUD_COMPAT_H

#include <string>

namespace BambuPlugin
{
    std::string unsupported_cloud_json(const std::string &feature);
    std::string unsupported_user_tasks_json();
    void fill_unsupported_my_profile(unsigned int *http_code, std::string *http_body);
    std::string ensure_cloud_notice_file(
        const std::string &config_dir,
        const std::string &relative_path,
        const std::string &title,
        const std::string &message
    );
    std::string model_publish_notice_url(const std::string &config_dir);
    std::string model_mall_home_notice_url(const std::string &config_dir);
    std::string model_mall_detail_notice_url(const std::string &config_dir, const std::string &id);
}

#endif // CLOUD_COMPAT_H
