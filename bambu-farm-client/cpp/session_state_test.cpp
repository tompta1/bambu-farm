#include "session_state.hpp"

#include <cassert>
#include <iostream>

static void test_autoconnect_claim()
{
    BambuPlugin::SessionState state;
    state.load_selected_device("printer-a");
    assert(state.claim_autoconnect_target("printer-b").empty());
    assert(state.claim_autoconnect_target("printer-a") == "printer-a");
    assert(state.claim_autoconnect_target("printer-a").empty());
}

static void test_begin_connect_transitions()
{
    BambuPlugin::SessionState state;
    assert(state.begin_connect("printer-a") == BambuPlugin::ConnectBeginResult::Start);
    assert(state.begin_connect("printer-a") == BambuPlugin::ConnectBeginResult::AlreadyInProgress);
    state.mark_connected("printer-a");
    assert(state.begin_connect("printer-a") == BambuPlugin::ConnectBeginResult::AlreadyConnected);
}

static void test_disconnect_and_logout_transitions()
{
    BambuPlugin::SessionState state;
    state.begin_connect("printer-a");
    assert(state.should_ignore_disconnect());
    assert(state.mark_disconnected(true));
    assert(!state.should_ignore_disconnect());

    state.begin_connect("printer-a");
    state.mark_connected("printer-a");
    assert(state.has_connected_selected_device());
    assert(state.disconnect_target() == "printer-a");
    assert(state.mark_disconnected(false));
    state.clear_after_logout();
    assert(state.selected_device().empty());
}

int main()
{
    test_autoconnect_claim();
    test_begin_connect_transitions();
    test_disconnect_and_logout_transitions();
    std::cout << "session_state_test: ok\n";
    return 0;
}
