# Remote-control input contribution

Jelly5 previously opened only the standard `scePad` port (0). The proposed
input owner additionally opens the current user's remote-control port (16),
reads its button state, and merges recognised buttons into the same shell and
player navigation stream. Port 16 is documented by the public PS5 input
[ABI reference](https://github.com/blackbearreloaded/ps5-native-gamepad-input-research/blob/main/include/ps5_pad.hpp).

The remote is optional: an open/read failure leaves DualSense navigation
available. Each input source suppresses its own buttons held at entry; remote
OK therefore cannot suppress a fresh DualSense X. Directional repeats and
single-press confirmation use the existing policy after merging. Only the
standard handle receives vibration, light-bar and trigger effects. Both owned
handles are closed at the existing shell/player handoff.

This is an **unverified HDMI-CEC integration candidate**. The user reports that
their TV remote operates the PS5 system menu but does nothing in released
Jelly5. Opening port 16 is supported by the public ABI; it does not establish
that every firmware/TV routes CEC input there. No system processes, HDMI
settings or controller privileges are patched. Dedicated media key encodings
are not guessed: only recognised pad button bits are merged.

## Validation

Run the actual input-owner regression on Linux or macOS with an ASan/UBSan
compiler:

```sh
CC=clang bash app/tests/test-input.sh
```

It checks remote arrows/OK/back, repeat timing, nonrepeating OK, simultaneous
sources holding one button, entry suppression in both directions, unknown
flags, failed reads, unavailable devices, injected taps, idempotent close and
controller-only output effects. It needs no server, credentials or console.

For this candidate, the ASan/UBSan regression passed, the production input
module compiled with `-Wall -Wextra -Werror` for the PS5 target, and all 85 app
objects compiled using the existing Linux SDK/PacBrew toolchain. C++ compilation
used `-frtti` for the upstream `dynamic_cast`. These compilation results do not
establish a working HDMI-CEC route on hardware.

Before claiming HDMI-CEC support, test the built app on PS5:

1. Enable HDMI Device Link and the TV's CEC setting; confirm navigation works
   in the PS5 system menu with the same remote.
2. In Jelly5, test arrows, OK and Back; hold an arrow and hold OK. OK must
   activate once. Test with the DualSense off and with both devices active.
3. Start a video, navigate player controls and return to the shell. Repeat
   play/stop and confirm both devices work after each handoff.
4. Test any dedicated Play/Pause/seek keys separately; report their behaviour
   rather than assuming their CEC commands map to pad buttons.
5. Confirm normal DualSense controls still work when the remote port is
   unavailable. No supported controller feature should depend on CEC.

The proposed change has not been installed on the user's PS5. A host mock or
cross-compilation cannot replace this hardware acceptance.
