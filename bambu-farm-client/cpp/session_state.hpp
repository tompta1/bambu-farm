#ifndef SESSION_STATE_H
#define SESSION_STATE_H

#include <mutex>
#include <string>

namespace BambuPlugin
{
    enum class ConnectBeginResult
    {
        Start,
        AlreadyConnected,
        AlreadyInProgress,
    };

    class SessionState
    {
    public:
        void load_selected_device(std::string dev_id);
        void set_user_selected_device(std::string dev_id);
        std::string selected_device() const;
        bool has_connected_selected_device() const;
        std::string connected_selected_device() const;
        ConnectBeginResult begin_connect(const std::string &dev_id);
        bool should_ignore_disconnect() const;
        std::string disconnect_target() const;
        void mark_connected(const std::string &dev_id);
        bool mark_disconnected(bool failed);
        std::string claim_autoconnect_target(const std::string &discovered_device_id);
        void clear_after_logout();

    private:
        mutable std::mutex mutex_;
        std::string selected_device_;
        bool auto_connect_pending_{false};
        bool printer_connected_{false};
        bool connect_in_progress_{false};
    };
}

#endif // SESSION_STATE_H
