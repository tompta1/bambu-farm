#include "callback_registry.hpp"

namespace BambuPlugin
{
    void CallbackRegistry::set_ssdp_callback(const BBL::OnMsgArrivedFn &fn, const DiagLogFn &log_fn, const std::function<void()> &refresh_fn)
    {
        std::unordered_map<std::string, std::string> cached;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            on_msg_arrived_ = fn;
            cached = last_printer_json_by_id_;
        }
        if (log_fn) {
            log_fn(std::string("bambu_network_set_on_ssdp_msg_fn: callback=") + (fn ? "set" : "null"));
        }
        if (!fn) {
            return;
        }
        for (const auto &[dev_id, json] : cached) {
            if (log_fn) {
                log_fn("bambu_network_set_on_ssdp_msg_fn: replay dev_id=" + dev_id);
            }
            fn(json);
        }
        if (log_fn) {
            log_fn("bambu_network_set_on_ssdp_msg_fn: requesting Rust discovery refresh");
        }
        if (refresh_fn) {
            refresh_fn();
        }
    }

    void CallbackRegistry::set_user_login_callback(const BBL::OnUserLoginFn &fn, bool logged_in, const DiagLogFn &log_fn)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            on_user_login_ = fn;
        }
        if (fn && log_fn) {
            log_fn("bambu_network_set_on_user_login_fn: signaling current LAN login state");
        }
        if (fn) {
            fn(0, logged_in);
        }
    }

    void CallbackRegistry::notify_user_login(bool logged_in)
    {
        BBL::OnUserLoginFn fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fn = on_user_login_;
        }
        if (fn) {
            fn(0, logged_in);
        }
    }

    void CallbackRegistry::set_printer_connected_callback(const BBL::OnPrinterConnectedFn &fn, const std::string &connected_device, const DiagLogFn &log_fn)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            on_printer_connected_ = fn;
        }
        if (fn && !connected_device.empty()) {
            if (log_fn) {
                log_fn("bambu_network_set_on_printer_connected_fn: signaling existing printer " + connected_device);
            }
            fn(connected_device);
        }
    }

    void CallbackRegistry::set_server_connected_callback(const BBL::OnServerConnectedFn &fn, const DiagLogFn &log_fn)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            on_server_connected_ = fn;
        }
        if (fn) {
            if (log_fn) {
                log_fn("bambu_network_set_on_server_connected_fn: signaling LAN server connected");
            }
            fn(BBL::ConnectStatusOk, 0);
        }
    }

    void CallbackRegistry::set_message_callback(const BBL::OnMessageFn &fn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        on_message_ = fn;
    }

    void CallbackRegistry::set_local_connect_callback(const BBL::OnLocalConnectedFn &fn, const std::string &connected_device, const DiagLogFn &log_fn)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            on_local_connect_ = fn;
        }
        if (fn && !connected_device.empty()) {
            if (log_fn) {
                log_fn("bambu_network_set_on_local_connect_fn: signaling existing printer " + connected_device);
            }
            fn(BBL::ConnectStatusOk, connected_device, "Connected");
        }
    }

    void CallbackRegistry::set_local_message_callback(const BBL::OnMessageFn &fn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        on_mqtt_message_ = fn;
    }

    void CallbackRegistry::set_subscribe_failure_callback(const BBL::GetSubscribeFailureFn &fn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        on_subscribe_failure_ = fn;
    }

    void CallbackRegistry::set_user_message_callback(const BBL::OnMessageFn &fn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        on_user_message_ = fn;
    }

    void CallbackRegistry::set_server_error_callback(const BBL::OnServerErrFn &fn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        on_server_error_ = fn;
    }

    void CallbackRegistry::remember_printer_json(const std::string &dev_id, const std::string &json)
    {
        if (dev_id.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        last_printer_json_by_id_[dev_id] = json;
    }

    std::string CallbackRegistry::printer_json(const std::string &dev_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = last_printer_json_by_id_.find(dev_id);
        if (it == last_printer_json_by_id_.end()) {
            return {};
        }
        return it->second;
    }

    void CallbackRegistry::dispatch_printer_available(const std::string &json)
    {
        BBL::OnMsgArrivedFn callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = on_msg_arrived_;
        }
        if (callback) {
            callback(json);
        }
    }

    bool CallbackRegistry::has_message_callbacks() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<bool>(on_message_) || static_cast<bool>(on_mqtt_message_);
    }

    void CallbackRegistry::dispatch_message(const std::string &device_id, const std::string &message, const DiagLogFn &log_fn)
    {
        BBL::OnMessageFn message_fn;
        BBL::OnMessageFn local_message_fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            message_fn = on_message_;
            local_message_fn = on_mqtt_message_;
        }

        if (local_message_fn) {
            if (log_fn) {
                log_fn("bambu_network_cb_message_recv: entering local callback");
            }
            local_message_fn(device_id, message);
            if (log_fn) {
                log_fn("bambu_network_cb_message_recv: local callback returned");
            }
        }

        if (message_fn) {
            if (log_fn) {
                log_fn("bambu_network_cb_message_recv: entering message callback");
            }
            message_fn(device_id, message);
            if (log_fn) {
                log_fn("bambu_network_cb_message_recv: message callback returned");
            }
        }
    }

    void CallbackRegistry::dispatch_connected(const std::string &device_id, const DiagLogFn &log_fn)
    {
        BBL::OnPrinterConnectedFn printer_connected_fn;
        BBL::OnLocalConnectedFn local_connect_fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            printer_connected_fn = on_printer_connected_;
            local_connect_fn = on_local_connect_;
        }
        if (printer_connected_fn) {
            if (log_fn) {
                log_fn("bambu_network_cb_connected: signaling printer connected " + device_id);
            }
            printer_connected_fn(device_id);
        }
        if (local_connect_fn) {
            if (log_fn) {
                log_fn("bambu_network_cb_connected: calling on_local_connect");
            }
            local_connect_fn(0, device_id, "Connected");
            if (log_fn) {
                log_fn("bambu_network_cb_connected: on_local_connect returned");
            }
        }
    }

    void CallbackRegistry::dispatch_disconnected(int status, const std::string &device_id, const std::string &message, const DiagLogFn &log_fn, bool should_emit)
    {
        BBL::OnLocalConnectedFn local_connect_fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            local_connect_fn = on_local_connect_;
        }
        if (local_connect_fn && should_emit) {
            if (log_fn) {
                log_fn("bambu_network_cb_disconnected: calling on_local_connect");
            }
            local_connect_fn(status, device_id, message);
            if (log_fn) {
                log_fn("bambu_network_cb_disconnected: on_local_connect returned");
            }
        }
    }
}
