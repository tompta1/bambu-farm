#ifndef TUNNEL_PROTOCOL_H
#define TUNNEL_PROTOCOL_H

#include <string>
#include <vector>

namespace BambuPlugin
{
    std::vector<std::string> split_file_download_response_frames(const std::string &response_bytes);
}

#endif // TUNNEL_PROTOCOL_H
