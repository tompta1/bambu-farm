#include "tunnel_bridge.hpp"
#include "print_job.hpp"
#include "tunnel_protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "bambu-farm-client/src/api.rs.h"

namespace
{
    std::atomic<std::uint64_t> next_video_stream_id{1};
    std::string last_tunnel_error;

    struct CompatTunnel
    {
        std::string url;
        std::string device_id;
        Logger logger{nullptr};
        void *logger_context{nullptr};
        bool opened{false};
        bool control_stream_started{false};
        bool video_stream_started{false};
        std::mutex mutex;
        std::deque<std::string> queued_responses;
        std::string active_response;
        std::uint64_t video_stream_id{0};
        unsigned long long active_decode_time{0};
    };

    CompatTunnel *as_compat_tunnel(Bambu_Tunnel tunnel)
    {
        return reinterpret_cast<CompatTunnel *>(tunnel);
    }

    std::string preview_payload(const std::string &message, size_t limit = 1024)
    {
        const size_t preview_len = std::min(limit, message.size());
        std::string preview = BambuPlugin::json_escape(message.substr(0, preview_len));
        if (preview_len < message.size()) {
            preview += "...";
        }
        return preview;
    }

    std::string bambu_query_param(const std::string &url, const std::string &key)
    {
        const std::string needle = key + "=";
        const size_t query_pos = url.find('?');
        size_t pos = url.find(needle, query_pos == std::string::npos ? 0 : query_pos + 1);
        if (pos == std::string::npos) {
            return {};
        }
        pos += needle.size();
        const size_t end = url.find('&', pos);
        return url.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    }

    void compat_tunnel_log(CompatTunnel *tunnel, const BambuPlugin::DiagLogFn &log_fn, const std::string &message)
    {
        if (log_fn) {
            log_fn(message);
        }
        if (!tunnel || !tunnel->logger) {
            return;
        }
        char *copy = static_cast<char *>(std::malloc(message.size() + 1));
        if (!copy) {
            return;
        }
        std::memcpy(copy, message.c_str(), message.size());
        copy[message.size()] = '\0';
        tunnel->logger(tunnel->logger_context, 0, copy);
    }
}

namespace BambuPlugin
{
    int create_tunnel(Bambu_Tunnel *tunnel, char const *path, const DiagLogFn &log_fn)
    {
        if (!tunnel || !path) {
            last_tunnel_error = "Bambu_Create: invalid arguments";
            return -1;
        }

        auto session = std::make_unique<CompatTunnel>();
        session->url = path;
        session->device_id = bambu_query_param(session->url, "device");
        if (session->device_id.empty()) {
            last_tunnel_error = "Bambu_Create: missing device in URL";
            return -1;
        }

        *tunnel = reinterpret_cast<Bambu_Tunnel>(session.release());
        if (log_fn) {
            log_fn("Bambu_Create: url=" + std::string(path) + " device=" + as_compat_tunnel(*tunnel)->device_id);
        }
        return Bambu_success;
    }

    void set_tunnel_logger(Bambu_Tunnel tunnel, Logger logger, void *context)
    {
        if (auto *session = as_compat_tunnel(tunnel)) {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->logger = logger;
            session->logger_context = context;
        }
    }

    int open_tunnel(Bambu_Tunnel tunnel, const DiagLogFn &log_fn)
    {
        auto *session = as_compat_tunnel(tunnel);
        if (!session) {
            last_tunnel_error = "Bambu_Open: invalid tunnel";
            return -1;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        session->opened = true;
        compat_tunnel_log(session, log_fn, "Bambu_Open: device=" + session->device_id);
        return Bambu_success;
    }

    int start_tunnel_stream(Bambu_Tunnel tunnel, bool video, const DiagLogFn &log_fn)
    {
        auto *session = as_compat_tunnel(tunnel);
        if (!session) {
            last_tunnel_error = "Bambu_StartStream: invalid tunnel";
            return -1;
        }
        if (video) {
            std::uint64_t stream_id = 0;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                if (session->video_stream_started) {
                    return Bambu_success;
                }
                stream_id = next_video_stream_id.fetch_add(1);
                session->video_stream_id = stream_id;
                session->video_stream_started = true;
                session->active_response.clear();
                session->active_decode_time = 0;
            }
            const int result = bambu_network_rs_start_camera_stream(stream_id, session->url, session->device_id);
            if (result != 0) {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->video_stream_started = false;
                session->video_stream_id = 0;
                last_tunnel_error = "Bambu_StartStream: failed to start camera stream";
                return -1;
            }
            compat_tunnel_log(
                session,
                log_fn,
                "Bambu_StartStream: started video stream stream_id=" + std::to_string(stream_id) +
                    " device=" + session->device_id);
            return Bambu_success;
        }

        std::lock_guard<std::mutex> lock(session->mutex);
        session->control_stream_started = true;
        return Bambu_success;
    }

    int start_tunnel_stream_ex(Bambu_Tunnel tunnel, int type)
    {
        auto *session = as_compat_tunnel(tunnel);
        if (!session) {
            last_tunnel_error = "Bambu_StartStreamEx: invalid tunnel";
            return -1;
        }
        if (type != 0x3001) {
            last_tunnel_error = "Bambu_StartStreamEx: unsupported stream type";
            return -1;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        session->control_stream_started = true;
        return Bambu_success;
    }

    int tunnel_stream_count(Bambu_Tunnel tunnel)
    {
        auto *session = as_compat_tunnel(tunnel);
        if (!session) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        return session->video_stream_started ? 1 : 0;
    }

    int tunnel_stream_info(Bambu_Tunnel tunnel, int index, Bambu_StreamInfo *info)
    {
        auto *session = as_compat_tunnel(tunnel);
        if (!session || !info || index != 0) {
            return -1;
        }
        std::lock_guard<std::mutex> lock(session->mutex);
        if (!session->video_stream_started) {
            return -1;
        }
        std::memset(info, 0, sizeof(*info));
        info->type = VIDE;
        info->sub_type = MJPG;
        info->format.video.width = 1920;
        info->format.video.height = 1080;
        info->format.video.frame_rate = 5;
        info->format_type = video_jpeg;
        info->format_size = 0;
        info->max_frame_size = 512 * 1024;
        info->format_buffer = nullptr;
        return Bambu_success;
    }

    int send_tunnel_message(Bambu_Tunnel tunnel, int ctrl, char const *data, int len, const DiagLogFn &log_fn)
    {
        auto *session = as_compat_tunnel(tunnel);
        if (!session || !data || len < 0) {
            last_tunnel_error = "Bambu_SendMessage: invalid arguments";
            return -1;
        }
        if (ctrl != 0x3001) {
            last_tunnel_error = "Bambu_SendMessage: unsupported control type";
            return -1;
        }

        const std::string request(data, static_cast<size_t>(len));
        const bool is_file_download = request.find("\"cmdtype\":4") != std::string::npos;
        compat_tunnel_log(session, log_fn, "Bambu_SendMessage: device=" + session->device_id + " request=" + preview_payload(request));
        rust::Vec<std::uint8_t> response = bambu_network_rs_tunnel_request(session->url, session->device_id, request);
        if (response.empty()) {
            last_tunnel_error = "Bambu_SendMessage: empty control response";
            return -1;
        }

        std::string response_bytes;
        response_bytes.reserve(response.size());
        for (std::uint8_t byte : response) {
            response_bytes.push_back(static_cast<char>(byte));
        }
        compat_tunnel_log(session, log_fn, "Bambu_SendMessage: response_bytes=" + std::to_string(response_bytes.size()));

        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (is_file_download) {
                auto frames = BambuPlugin::split_file_download_response_frames(response_bytes);
                compat_tunnel_log(
                    session,
                    log_fn,
                    "Bambu_SendMessage: queued_frames=" + std::to_string(frames.size()) +
                        " chunked_file_download=" + std::string(frames.size() > 1 ? "true" : "false"));
                for (auto &frame : frames) {
                    session->queued_responses.emplace_back(std::move(frame));
                }
            } else {
                session->queued_responses.emplace_back(std::move(response_bytes));
            }
        }
        return Bambu_success;
    }

    int read_tunnel_sample(Bambu_Tunnel tunnel, Bambu_Sample *sample, const DiagLogFn &log_fn)
    {
        auto *session = as_compat_tunnel(tunnel);
        if (!session || !sample) {
            last_tunnel_error = "Bambu_ReadSample: invalid arguments";
            return -1;
        }

        std::uint64_t video_stream_id = 0;
        std::lock_guard<std::mutex> lock(session->mutex);
        video_stream_id = session->video_stream_id;
        if (session->video_stream_started && video_stream_id != 0) {
            rust::Vec<std::uint8_t> frame = bambu_network_rs_read_camera_frame(video_stream_id);
            if (!frame.empty()) {
                session->active_response.clear();
                session->active_response.reserve(frame.size());
                for (std::uint8_t byte : frame) {
                    session->active_response.push_back(static_cast<char>(byte));
                }
                session->active_decode_time = 0;
                compat_tunnel_log(
                    session,
                    log_fn,
                    "Bambu_ReadSample: video frame bytes=" + std::to_string(session->active_response.size()));
                sample->itrack = 0;
                sample->size = static_cast<int>(session->active_response.size());
                sample->flags = 1;
                sample->buffer = reinterpret_cast<unsigned char const *>(session->active_response.data());
                sample->decode_time = session->active_decode_time;
                return Bambu_success;
            }

            const int state = bambu_network_rs_camera_stream_state(video_stream_id);
            if (state == 0) {
                return Bambu_would_block;
            }
            if (state == 1) {
                return Bambu_stream_end;
            }
            last_tunnel_error = "Bambu_ReadSample: camera stream failed";
            return -1;
        }

        if (session->queued_responses.empty()) {
            return Bambu_would_block;
        }

        session->active_response = std::move(session->queued_responses.front());
        session->queued_responses.pop_front();
        compat_tunnel_log(session, log_fn, "Bambu_ReadSample: response_bytes=" + std::to_string(session->active_response.size()));
        sample->itrack = 0;
        sample->size = static_cast<int>(session->active_response.size());
        sample->flags = 1;
        sample->buffer = reinterpret_cast<unsigned char const *>(session->active_response.data());
        sample->decode_time = 0;
        return Bambu_success;
    }

    int recv_tunnel_message(Bambu_Tunnel tunnel, int *ctrl, char *data, int *len)
    {
        auto *session = as_compat_tunnel(tunnel);
        if (!session || !len) {
            last_tunnel_error = "Bambu_RecvMessage: invalid arguments";
            return -1;
        }

        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->queued_responses.empty()) {
            return Bambu_would_block;
        }

        const std::string &response = session->queued_responses.front();
        if (!data || *len < static_cast<int>(response.size())) {
            *len = static_cast<int>(response.size());
            return Bambu_buffer_limit;
        }

        std::memcpy(data, response.data(), response.size());
        *len = static_cast<int>(response.size());
        if (ctrl) {
            *ctrl = 0x3001;
        }
        session->queued_responses.pop_front();
        return Bambu_success;
    }

    void close_tunnel(Bambu_Tunnel tunnel)
    {
        if (auto *session = as_compat_tunnel(tunnel)) {
            std::uint64_t video_stream_id = 0;
            std::lock_guard<std::mutex> lock(session->mutex);
            session->opened = false;
            session->control_stream_started = false;
            video_stream_id = session->video_stream_id;
            session->video_stream_started = false;
            session->video_stream_id = 0;
            session->queued_responses.clear();
            session->active_response.clear();
            session->active_decode_time = 0;
            if (video_stream_id != 0) {
                bambu_network_rs_stop_camera_stream(video_stream_id);
            }
        }
    }

    void destroy_tunnel(Bambu_Tunnel tunnel)
    {
        close_tunnel(tunnel);
        delete as_compat_tunnel(tunnel);
    }

    char const *last_tunnel_error_message()
    {
        return last_tunnel_error.c_str();
    }
}
