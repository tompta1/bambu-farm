#ifndef TUNNEL_BRIDGE_H
#define TUNNEL_BRIDGE_H

#include "api.hpp"
#include "callback_registry.hpp"

#include <string>

namespace BambuPlugin
{
    int create_tunnel(Bambu_Tunnel *tunnel, char const *path, const DiagLogFn &log_fn);
    void set_tunnel_logger(Bambu_Tunnel tunnel, Logger logger, void *context);
    int open_tunnel(Bambu_Tunnel tunnel, const DiagLogFn &log_fn);
    int start_tunnel_stream(Bambu_Tunnel tunnel, bool video, const DiagLogFn &log_fn);
    int start_tunnel_stream_ex(Bambu_Tunnel tunnel, int type);
    int tunnel_stream_count(Bambu_Tunnel tunnel);
    int tunnel_stream_info(Bambu_Tunnel tunnel, int index, Bambu_StreamInfo *info);
    int send_tunnel_message(Bambu_Tunnel tunnel, int ctrl, char const *data, int len, const DiagLogFn &log_fn);
    int read_tunnel_sample(Bambu_Tunnel tunnel, Bambu_Sample *sample, const DiagLogFn &log_fn);
    int recv_tunnel_message(Bambu_Tunnel tunnel, int *ctrl, char *data, int *len);
    void close_tunnel(Bambu_Tunnel tunnel);
    void destroy_tunnel(Bambu_Tunnel tunnel);
    char const *last_tunnel_error_message();
}

#endif // TUNNEL_BRIDGE_H
