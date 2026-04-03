#include "local_state.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    std::string config_or_default_path(
        const std::string &config_dir,
        const std::string &suffix,
        const std::string &tmp_fallback
    )
    {
        if (!config_dir.empty()) {
            return config_dir + "/" + suffix;
        }

        const char *xdg = std::getenv("XDG_CONFIG_HOME");
        const char *home = std::getenv("HOME");
        if (xdg && *xdg) {
            return std::string(xdg) + "/BambuStudio/" + suffix;
        }
        if (home && *home) {
            return std::string(home) + "/.config/BambuStudio/" + suffix;
        }
        return tmp_fallback;
    }

    std::string selected_device_state_path(const std::string &config_dir)
    {
        return config_or_default_path(config_dir, "oss-last-device.txt", "/tmp/bambu-oss-last-device.txt");
    }

    std::string printer_names_state_path(const std::string &config_dir)
    {
        return config_or_default_path(config_dir, "oss-printer-names.tsv", "/tmp/bambu-oss-printer-names.tsv");
    }

    std::string local_print_contexts_state_path(const std::string &config_dir)
    {
        return config_or_default_path(config_dir, "oss-local-print-contexts.tsv", "/tmp/bambu-oss-local-print-contexts.tsv");
    }
}

namespace BambuPlugin
{
    std::string local_thumbnail_cache_dir(const std::string &config_dir)
    {
        return config_or_default_path(config_dir, "oss-thumbnails", "/tmp/bambu-oss-thumbnails");
    }

    void persist_local_print_contexts(
        const std::string &config_dir,
        const std::unordered_map<std::string, LocalPrintContext> &contexts,
        const DiagLogFn &log_fn
    )
    {
        const std::string path = local_print_contexts_state_path(config_dir);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open()) {
            if (log_fn) {
                log_fn("persist_local_print_contexts: failed path=" + path);
            }
            return;
        }

        for (const auto &[dev_id, context] : contexts) {
            if (!context.active || dev_id.empty()) {
                continue;
            }
            out
                << context.dev_id << '\t'
                << context.project_id << '\t'
                << context.profile_id << '\t'
                << context.subtask_id << '\t'
                << context.task_id << '\t'
                << context.remote_filename << '\t'
                << context.plate_path << '\t'
                << context.thumbnail_url << '\t'
                << context.plate_index << '\n';
        }
        if (log_fn) {
            log_fn("persist_local_print_contexts: wrote path=" + path);
        }
    }

    std::unordered_map<std::string, LocalPrintContext> load_local_print_contexts(
        const std::string &config_dir,
        const DiagLogFn &log_fn
    )
    {
        const std::string path = local_print_contexts_state_path(config_dir);
        std::ifstream in(path);
        if (!in.is_open()) {
            if (log_fn) {
                log_fn("load_local_print_contexts: no state path=" + path);
            }
            return {};
        }

        std::unordered_map<std::string, LocalPrintContext> loaded;
        std::string line;
        while (std::getline(in, line)) {
            std::vector<std::string> fields;
            size_t start = 0;
            while (true) {
                const size_t tab = line.find('\t', start);
                if (tab == std::string::npos) {
                    fields.push_back(line.substr(start));
                    break;
                }
                fields.push_back(line.substr(start, tab - start));
                start = tab + 1;
            }
            if (fields.size() != 9 || fields[0].empty()) {
                continue;
            }

            LocalPrintContext context;
            context.dev_id = fields[0];
            context.project_id = fields[1];
            context.profile_id = fields[2];
            context.subtask_id = fields[3];
            context.task_id = fields[4];
            context.remote_filename = fields[5];
            context.plate_path = fields[6];
            context.thumbnail_url = fields[7];
            try {
                context.plate_index = std::stoi(fields[8]);
            } catch (...) {
                context.plate_index = 0;
            }
            context.active = true;
            loaded[context.dev_id] = context;
        }

        if (log_fn) {
            log_fn("load_local_print_contexts: loaded path=" + path);
        }
        return loaded;
    }

    void persist_printer_name_overrides(
        const std::string &config_dir,
        const std::unordered_map<std::string, std::string> &printer_name_overrides,
        const DiagLogFn &log_fn
    )
    {
        const std::string path = printer_names_state_path(config_dir);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open()) {
            if (log_fn) {
                log_fn("persist_printer_name_overrides: failed path=" + path);
            }
            return;
        }

        for (const auto &[dev_id, name] : printer_name_overrides) {
            if (!dev_id.empty() && !name.empty()) {
                out << dev_id << '\t' << name << '\n';
            }
        }
        if (log_fn) {
            log_fn("persist_printer_name_overrides: wrote path=" + path);
        }
    }

    std::unordered_map<std::string, std::string> load_printer_name_overrides(
        const std::string &config_dir,
        const DiagLogFn &log_fn
    )
    {
        const std::string path = printer_names_state_path(config_dir);
        std::ifstream in(path);
        if (!in.is_open()) {
            if (log_fn) {
                log_fn("load_printer_name_overrides: no state path=" + path);
            }
            return {};
        }

        std::unordered_map<std::string, std::string> loaded;
        std::string line;
        while (std::getline(in, line)) {
            const size_t tab_pos = line.find('\t');
            if (tab_pos == std::string::npos) {
                continue;
            }
            const std::string dev_id = line.substr(0, tab_pos);
            const std::string name = line.substr(tab_pos + 1);
            if (!dev_id.empty() && !name.empty()) {
                loaded[dev_id] = name;
            }
        }

        if (log_fn) {
            log_fn("load_printer_name_overrides: loaded path=" + path);
        }
        return loaded;
    }

    void persist_selected_device(
        const std::string &config_dir,
        const std::string &selected_device,
        const DiagLogFn &log_fn
    )
    {
        const std::string path = selected_device_state_path(config_dir);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

        if (selected_device.empty()) {
            std::filesystem::remove(path, ec);
            if (log_fn) {
                log_fn("persist_selected_device: cleared path=" + path);
            }
            return;
        }

        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open()) {
            if (log_fn) {
                log_fn("persist_selected_device: failed path=" + path);
            }
            return;
        }
        out << selected_device;
        if (log_fn) {
            log_fn("persist_selected_device: wrote dev_id=" + selected_device + " path=" + path);
        }
    }

    std::string load_selected_device(const std::string &config_dir, const DiagLogFn &log_fn)
    {
        const std::string path = selected_device_state_path(config_dir);
        std::ifstream in(path);
        if (!in.is_open()) {
            if (log_fn) {
                log_fn("load_selected_device: no state path=" + path);
            }
            return {};
        }

        std::string loaded;
        std::getline(in, loaded);
        if (loaded.empty()) {
            if (log_fn) {
                log_fn("load_selected_device: empty state path=" + path);
            }
            return {};
        }

        if (log_fn) {
            log_fn("load_selected_device: dev_id=" + loaded + " path=" + path);
        }
        return loaded;
    }
}
