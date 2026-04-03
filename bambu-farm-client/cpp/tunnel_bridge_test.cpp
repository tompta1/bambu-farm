#include "tunnel_protocol.hpp"

#include <cassert>
#include <iostream>
#include <string>

static void test_split_file_download_response_frames_small_payload_stays_single()
{
    const std::string response = "{\"result\":0,\"sequence\":1,\"reply\":{\"size\":3,\"offset\":0,\"total\":3,\"file_md5\":\"a\",\"ftp_file_md5\":\"b\",\"path\":\"x\"}}\n\nabc";
    const auto frames = BambuPlugin::split_file_download_response_frames(response);
    assert(frames.size() == 1);
    assert(frames.front() == response);
}

static void test_split_file_download_response_frames_preserves_payload_and_metadata()
{
    const std::string payload((4 * 1024 * 1024) + 32, 'x');
    const std::string header =
        "{\"result\":0,\"sequence\":7,\"reply\":{\"size\":" + std::to_string(payload.size()) +
        ",\"offset\":0,\"total\":" + std::to_string(payload.size()) +
        ",\"file_md5\":\"abc\",\"ftp_file_md5\":\"def\",\"path\":\"timelapse/video.avi\"}}";
    const auto frames = BambuPlugin::split_file_download_response_frames(header + "\n\n" + payload);
    assert(frames.size() == 2);
    assert(frames.front().find("\"result\":1") != std::string::npos);
    assert(frames.front().find("\"sequence\":7") != std::string::npos);
    assert(frames.front().find("\"path\":\"timelapse/video.avi\"") != std::string::npos);
    assert(frames.back().find("\"result\":0") != std::string::npos);

    std::string reconstructed;
    for (const auto &frame : frames) {
        const size_t separator = frame.find("\n\n");
        assert(separator != std::string::npos);
        reconstructed += frame.substr(separator + 2);
    }
    assert(reconstructed == payload);
}

int main()
{
    test_split_file_download_response_frames_small_payload_stays_single();
    test_split_file_download_response_frames_preserves_payload_and_metadata();
    std::cout << "tunnel_bridge_test: ok\n";
    return 0;
}
