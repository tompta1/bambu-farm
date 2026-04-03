#include "tunnel_protocol.hpp"
#include "print_job.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <regex>
#include <string>

namespace
{
    constexpr size_t kTunnelDownloadChunkSize = 4 * 1024 * 1024;

    bool extract_json_integer(const std::string &json, const char *key, std::uint64_t &value)
    {
        const std::regex pattern(std::string("\"") + key + "\":([0-9]+)");
        std::smatch match;
        if (!std::regex_search(json, match, pattern) || match.size() < 2) {
            return false;
        }
        value = std::strtoull(match[1].str().c_str(), nullptr, 10);
        return true;
    }

    std::string extract_json_string(const std::string &json, const char *key)
    {
        const std::regex pattern(std::string("\"") + key + "\":\"([^\"]*)\"");
        std::smatch match;
        if (!std::regex_search(json, match, pattern) || match.size() < 2) {
            return {};
        }
        return match[1].str();
    }
}

namespace BambuPlugin
{
    std::vector<std::string> split_file_download_response_frames(const std::string &response_bytes)
    {
        const size_t separator = response_bytes.find("\n\n");
        if (separator == std::string::npos) {
            return {response_bytes};
        }

        const std::string header = response_bytes.substr(0, separator);
        const std::string payload = response_bytes.substr(separator + 2);
        if (payload.size() <= kTunnelDownloadChunkSize) {
            return {response_bytes};
        }

        std::uint64_t sequence = 0;
        std::uint64_t total = static_cast<std::uint64_t>(payload.size());
        if (!extract_json_integer(header, "sequence", sequence)) {
            return {response_bytes};
        }
        extract_json_integer(header, "total", total);

        const std::string file_md5 = extract_json_string(header, "file_md5");
        const std::string ftp_file_md5 = extract_json_string(header, "ftp_file_md5");
        const std::string path = extract_json_string(header, "path");

        std::vector<std::string> frames;
        frames.reserve((payload.size() + kTunnelDownloadChunkSize - 1) / kTunnelDownloadChunkSize);
        for (size_t offset = 0; offset < payload.size(); offset += kTunnelDownloadChunkSize) {
            const size_t chunk_size = std::min(kTunnelDownloadChunkSize, payload.size() - offset);
            const int result = (offset + chunk_size) < payload.size() ? 1 : 0;
            std::string frame_header =
                "{\"result\":" + std::to_string(result) +
                ",\"sequence\":" + std::to_string(sequence) +
                ",\"reply\":{\"size\":" + std::to_string(chunk_size) +
                ",\"offset\":" + std::to_string(offset) +
                ",\"total\":" + std::to_string(total) +
                ",\"file_md5\":\"" + BambuPlugin::json_escape(file_md5) +
                "\",\"ftp_file_md5\":\"" + BambuPlugin::json_escape(ftp_file_md5) +
                "\",\"path\":\"" + BambuPlugin::json_escape(path) + "\"}}";
            std::string frame;
            frame.reserve(frame_header.size() + 2 + chunk_size);
            frame += frame_header;
            frame += "\n\n";
            frame.append(payload.data() + offset, chunk_size);
            frames.emplace_back(std::move(frame));
        }
        return frames;
    }
}
