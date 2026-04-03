#ifndef CLOUD_COMPAT_H
#define CLOUD_COMPAT_H

#include <string>

namespace BambuPlugin
{
    std::string unsupported_cloud_json(const std::string &feature);
    std::string ensure_cloud_notice_file(
        const std::string &config_dir,
        const std::string &relative_path,
        const std::string &title,
        const std::string &message
    );
}

#endif // CLOUD_COMPAT_H
