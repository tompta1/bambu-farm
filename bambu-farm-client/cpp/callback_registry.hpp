#ifndef CALLBACK_REGISTRY_H
#define CALLBACK_REGISTRY_H

#include "bambu_networking.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace BambuPlugin
{
    using DiagLogFn = std::function<void(const std::string &)>;

    class CallbackRegistry
    {
    public:
        void set_ssdp_callback(const BBL::OnMsgArrivedFn &fn, const DiagLogFn &log_fn, const std::function<void()> &refresh_fn);
        void set_user_login_callback(const BBL::OnUserLoginFn &fn, bool logged_in, const DiagLogFn &log_fn);
        void notify_user_login(bool logged_in);
        void set_printer_connected_callback(const BBL::OnPrinterConnectedFn &fn, const std::string &connected_device, const DiagLogFn &log_fn);
        void set_server_connected_callback(const BBL::OnServerConnectedFn &fn, const DiagLogFn &log_fn);
        void set_message_callback(const BBL::OnMessageFn &fn);
        void set_local_connect_callback(const BBL::OnLocalConnectedFn &fn, const std::string &connected_device, const DiagLogFn &log_fn);
        void set_local_message_callback(const BBL::OnMessageFn &fn);
        void set_subscribe_failure_callback(const BBL::GetSubscribeFailureFn &fn);
        void set_user_message_callback(const BBL::OnMessageFn &fn);
        void set_server_error_callback(const BBL::OnServerErrFn &fn);

        void remember_printer_json(const std::string &dev_id, const std::string &json);
        std::string printer_json(const std::string &dev_id) const;
        void dispatch_printer_available(const std::string &json);
        bool has_message_callbacks() const;
        void dispatch_message(const std::string &device_id, const std::string &message, const DiagLogFn &log_fn);
        void dispatch_connected(const std::string &device_id, const DiagLogFn &log_fn);
        void dispatch_disconnected(int status, const std::string &device_id, const std::string &message, const DiagLogFn &log_fn, bool should_emit);

    private:
        mutable std::mutex mutex_;
        BBL::OnMsgArrivedFn on_msg_arrived_;
        BBL::OnLocalConnectedFn on_local_connect_;
        BBL::OnMessageFn on_message_;
        BBL::OnMessageFn on_mqtt_message_;
        BBL::OnMessageFn on_user_message_;
        BBL::OnPrinterConnectedFn on_printer_connected_;
        BBL::OnServerConnectedFn on_server_connected_;
        BBL::OnServerErrFn on_server_error_;
        BBL::OnUserLoginFn on_user_login_;
        BBL::GetSubscribeFailureFn on_subscribe_failure_;
        std::unordered_map<std::string, std::string> last_printer_json_by_id_;
    };
}

#endif // CALLBACK_REGISTRY_H
