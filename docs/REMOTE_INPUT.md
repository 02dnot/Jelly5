# Remote-control input contribution

Jelly5's shared shell/player input owner opens the standard DualSense port (0)
and the optional remote-control port (16) for system user `0xff`. The public
PS5 input [ABI reference](https://github.com/blackbearreloaded/ps5-native-gamepad-input-research/blob/main/include/ps5_pad.hpp)
documents the remote port.

The TV remote reports arrows in the normal button mask. Its additional Sony
remote-key byte at offset `0x6c` carries OK and Back:

| Sony remote code | Jelly5 input |
| --- | --- |
| 0 | No additional key held |
| 13 / `0x0d` | Cross: select, play/pause |
| 15 / `0x0f` | Circle: back, leave the player |

These are the Sony pad codes, rather than the HDMI CEC wire values. The public
[PS5-SDL remote backend](https://github.com/ps5-payload-dev/SDL/blob/ee4c47dc0d617b3bc8f35108f9956baf228a1322/src/video/ps5/SDL_ps5remote.c#L163)
reads that same byte and uses the same OK/Back mapping. Like SDL, this decoder
does not require a nonzero unique-data length. It reads this field only from
the remote handle; ordinary DualSense device data is not interpreted as a
remote key. Unknown key codes and system-intercepted input are ignored.

A buffered remote read preserves taps that finish between frame snapshots.
Equal sample timestamps can carry real press/release transitions; a zero
timestamp does not roll back the watermark. Snapshot-only releases rearm a
button without treating a constantly empty snapshot as a new queue-only tap.
A held confirmation does not auto-repeat, and a controller or command-channel
hold prevents an additional remote tap from duplicating the same action.

The remote remains optional: an open/read failure leaves DualSense navigation
available. Each source suppresses its own buttons held at entry. Only the
standard handle receives vibration, light-bar and trigger effects. Both owned
handles close at the existing shell/player handoff. The change does not patch
system processes, HDMI settings, or controller privileges.

## Validation

Run the production input-owner regression with an ASan/UBSan compiler:

```sh
CC=clang bash app/tests/test-input.sh
```

It checks the actual Sony OK/Back bytes through snapshots and buffered reads,
held keys, launch suppression, simultaneous inputs, short taps, timestamp
edge cases, failures, unknown/intercepted input, and controller-only effects.
The suite needs no server, credentials or console.

Hardware capture on the test PS5 confirmed arrows and, in a labelled
Right -> OK -> Back sequence, Sony codes `0x0d` and `0x0f` with `0` releases.
Console acceptance passed for opening the selected tile, Back navigation, held
OK activating once, playback pause/resume and return to the shell, followed by
DualSense input. Host regressions and cross-compilation supplement that check.

Console checks for the completed candidate:

1. Enable HDMI Device Link and the TV's CEC setting; confirm the same remote
   operates the PS5 system menu.
2. In Jelly5, test arrows, OK, Back, a held arrow and held OK. OK must activate
   once. Check with DualSense off and with both devices active.
3. Start a video, pause/resume, seek with arrows and return to the shell.
   Confirm both devices work after repeated shell/player handoffs.
4. Check dedicated media keys separately. This change adds navigation keys;
   it does not assign unverified Play/Pause/seek key codes.
