#include "session_state.hpp"

namespace BambuPlugin
{
    void SessionState::load_selected_device(std::string dev_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_device_ = std::move(dev_id);
        auto_connect_pending_ = !selected_device_.empty();
    }

    void SessionState::set_user_selected_device(std::string dev_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_device_ = std::move(dev_id);
        auto_connect_pending_ = !selected_device_.empty();
    }

    std::string SessionState::selected_device() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return selected_device_;
    }

    bool SessionState::has_connected_selected_device() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return !selected_device_.empty() && printer_connected_;
    }

    std::string SessionState::connected_selected_device() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (selected_device_.empty() || !printer_connected_) {
            return {};
        }
        return selected_device_;
    }

    ConnectBeginResult SessionState::begin_connect(const std::string &dev_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (selected_device_ == dev_id && printer_connected_) {
            return ConnectBeginResult::AlreadyConnected;
        }
        if (selected_device_ == dev_id && connect_in_progress_) {
            return ConnectBeginResult::AlreadyInProgress;
        }
        selected_device_ = dev_id;
        auto_connect_pending_ = false;
        connect_in_progress_ = true;
        printer_connected_ = false;
        return ConnectBeginResult::Start;
    }

    bool SessionState::should_ignore_disconnect() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return connect_in_progress_ && !printer_connected_;
    }

    std::string SessionState::disconnect_target() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return selected_device_;
    }

    void SessionState::mark_connected(const std::string &dev_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_device_ = dev_id;
        connect_in_progress_ = false;
        printer_connected_ = true;
    }

    bool SessionState::mark_disconnected(bool failed)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connect_in_progress_ = false;
        const bool was_connected = printer_connected_;
        printer_connected_ = false;
        return was_connected || failed;
    }

    std::string SessionState::claim_autoconnect_target(const std::string &discovered_device_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!auto_connect_pending_ || selected_device_ != discovered_device_id) {
            return {};
        }
        auto_connect_pending_ = false;
        return selected_device_;
    }

    void SessionState::clear_after_logout()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_device_.clear();
        auto_connect_pending_ = false;
        printer_connected_ = false;
        connect_in_progress_ = false;
    }
}
