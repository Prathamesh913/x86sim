# simhid: touch, buttons and keyboard

`host/simhid/simhid.m` builds `SimulatorHID.framework`, a stand-in for CoreSimulator's SimulatorHID. backboardd loads
it from `SIMULATOR_HID_SYSTEM_MANAGER` (SimulatorClient's `IndigoHIDSystemSpawnLoopback`): the bundle's principal
class `SimHIDSystem` is created with backboardd's `IOHIDEventSystemRef` and adds virtual HID services to it
(`_IOHIDServiceCreateVirtual`, `_IOHIDEventSystemAddService`, `_IOHIDServiceDispatchEvent`): a built-in
touchscreen, the main-screen buttons (home, lock, volume) and a keyboard. Apple's version receives "Indigo" messages
from Simulator.app over a mach port; ours reads a Unix socket.

Clients: `cli/libexec/simview.py` (mouse, keyboard; Ctrl/Shift+drag = two fingers; F1/Ctrl+H home, F2 lock, F3/F4
volume) and `cli/libexec/simtouch.py` (`x86sim tap|swipe|pinch|pan2|key|type|home|lock|raw`).

## Socket protocol

```
Socket: $SIMHID_SOCKET (default <SIMULATOR_SHARED_RESOURCES_DIRECTORY>/simhid.sock), stream, any number of
clients, one text command per line:
  touch down|move|up <x> <y>     x, y normalized to the display (0..1, origin top left)
  touch2 down|move|up <x1> <y1> <x2> <y2>   two fingers (pinch, two-finger pan): one hand event with two finger
                                 children, finger indexes 1 and 2 with their own identities
  key down|up <usage>            keyboard usage (page 7), e.g. 4 = 'a', 0x28 = return
  button home|lock|volup|voldown down|up
```
