// Host-side check of the state transitions in specification section 5.
// Build and run: g++ -std=c++17 -I src tools/test_mute.cpp -o test_mute && ./test_mute
#include "domain/MuteState.h"
#include <cassert>
#include <iostream>

using namespace domain;

int main() {
    MuteState s;
    assert(!s.ready() && s.host == Host::Unknown);
    assert(s.action() == Action::Blocked);

    s.connect(1);
    assert(s.output(1, 1, 1, 1) == Report::Rejected);  // Not encrypted yet.
    s.encrypted = true;
    assert(!s.ready());
    s.subscribed = true;
    assert(s.ready());

    // Rejected reports never move the state.
    assert(s.output(0, 1, 1, 1) == Report::Rejected);
    assert(s.output(1, 2, 1, 1) == Report::Rejected);
    assert(s.output(1, 1, 0, 1) == Report::Rejected);
    assert(s.output(1, 1, 2, 1) == Report::Rejected);
    assert(s.host == Host::Unknown);

    // Unknown cannot be inverted, so the only operation is the absolute mute.
    assert(s.action() == Action::Mute && s.valueFor(Action::Mute) == 1);
    s.sent(1, 100);
    assert(s.pending && s.action() == Action::Blocked);
    // No output: the host may already have been muted, so nothing is assumed.
    assert(s.tick(1099) == Expiry::None);
    assert(s.tick(1100) == Expiry::Unresolved && s.host == Host::Unknown);

    // A known state gives a toggle whose value is the opposite of the report.
    assert(s.output(1, 1, 1, 7) == Report::Changed && s.host == Host::Muted);
    assert(s.action() == Action::Toggle && s.valueFor(Action::Toggle) == 0);
    s.sent(0, 2000);
    assert(s.output(1, 1, 1, 0) == Report::Changed && !s.pending && s.host == Host::Unmuted);

    // Repeated identical reports are not treated as a change.
    assert(s.output(1, 1, 1, 0) == Report::Unchanged && s.host == Host::Unmuted);

    // Sending the value the host already reports stays silent; the known state
    // survives the reflection window instead of falling back to unknown.
    s.sent(0, 3000);
    assert(s.tick(4000) == Expiry::NoChange && s.host == Host::Unmuted && !s.pending);

    // A request that should have changed the host, with no report, is unknown.
    s.sent(1, 5000);
    assert(s.tick(6000) == Expiry::Unresolved && s.host == Host::Unknown);

    // The deadline comparison survives the 32-bit millisecond wrap.
    s.output(1, 1, 1, 1);
    s.sent(0, 0xFFFFFF00u);
    assert(s.tick(0x10) == Expiry::None);
    assert(s.tick(0x300) == Expiry::Unresolved);

    // Nothing carries across a reconnection.
    s.output(1, 1, 1, 1);
    s.disconnect();
    assert(!s.ready() && s.host == Host::Unknown && !s.pending);
    s.connect(2);
    s.encrypted = s.subscribed = true;
    assert(s.output(1, 1, 1, 1) == Report::Rejected);  // Stale generation.
    assert(s.host == Host::Unknown);

    // Suspend and boot protocol mode both stop reports from being sent.
    s.suspended = true;
    assert(!s.ready() && s.action() == Action::Blocked);
    s.suspended = false;
    s.reportMode = false;
    assert(!s.ready());
    s.reportMode = true;
    assert(s.ready() && s.action() == Action::Mute);

    std::cout << "Mute state tests passed\n";
}
