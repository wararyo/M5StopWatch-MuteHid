#include "domain/ProbeState.h"
#include <cassert>
#include <iostream>
int main() {
    ProbeState s;
    assert(!s.ready() && s.hostMute == -1);
    s.connect(1);
    assert(!s.output(1, 1, 1, 1));
    s.encrypted = true;
    assert(!s.ready());
    s.subscribed = true;
    assert(s.ready());
    assert(!s.output(0, 1, 1, 1));
    assert(!s.output(1, 2, 1, 1));
    assert(!s.output(1, 1, 0, 1));
    assert(!s.output(1, 1, 2, 1));
    assert(s.output(1, 1, 1, 7) && s.hostMute == 1);
    s.sent(100);
    assert(!s.tick(1099));
    assert(s.tick(1100) && s.hostMute == -1);
    s.sent(0xFFFFFF00u);
    assert(!s.tick(0x10));
    assert(s.tick(0x300));
    s.sent(100);
    assert(s.output(1, 1, 1, 0) && !s.pending && s.hostMute == 0);
    s.disconnect();
    assert(!s.ready() && s.hostMute == -1);
    s.connect(2); s.encrypted = s.subscribed = true;
    assert(!s.output(1, 1, 1, 1) && s.hostMute == -1);
    s.suspended = true; assert(!s.ready());
    s.suspended = false; s.reportMode = false; assert(!s.ready());
    std::cout << "Probe state tests passed\n";
}
