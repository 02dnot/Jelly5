# Jelly5 development environment plan

This is the plan for short, repeatable PS5 development cycles. It describes the
current tools, the missing pieces, and the checks that must pass before an agent
may run a console cycle. Keep credentials and raw captures out of Git.

## Current state (2026-10-16)

- `app/scripts/build.sh` builds the PS5 app; host tests live under
  `app/tests/host/`. `scripts/deploy.py` uploads a built app folder over FTP to
  `/data/homebrew/PPSA99505`. First installs are staged; updates overwrite the
  existing directory in place. It asks whether the app is closed, but cannot
  verify that answer itself. It does not compare remote files with the build.
- `scripts/log.sh` receives the app's UDP log into ignored
  `build/console.log`. A development build discovers the Mac's LAN address
  from `PS5_HOST`; release builds disable this log destination.
- `app/src/nuvio_input.c` has an input injection function, and the Nuvio header
  describes a command and screenshot channel. Jelly5's
  `app/src/jelly5_bridge.cpp` currently stubs `nuvio_bridge_commands()` to
  return zero. There is no working remote input or app screenshot path here.
- ShadowMount Plus owns title registration and lifecycle. Do not replace the
  mounted `/data/homebrew/PPSA99505` directory or upload over a live app.

## Target loop

1. Build and run focused host tests; freeze the exact app folder and record its
   SHA-256 manifest, Git revision, and build mode.
2. Acquire a local console lock. Check FTP, the title's lifecycle state, and
   the artifact. Stop if the app may still be resident or evidence is missing.
3. Upload changed files via temporary names, promote them in place, and verify
   size/hash or readback. Upload `eboot.bin` and `sce_sys/param.json` last.
4. Wait for ShadowMount to report the title ready. Launch only when the slot is
   proven free; never stack launches or retry a failed launch automatically.
5. Run one bounded test scenario. Collect app log, loader/kernel log, lifecycle
   events, and screenshots where available. Scrub tokens, server addresses, and
   user information before sharing a report.
6. Close the title, prove the runtime layers were released, release the lock,
   and classify the run: pass, failed, inconclusive, or no-run. Preserve the
   raw evidence locally for diagnosis.

## Everyday agent workflow

For each issue or PR, the agent should work in one descriptive branch, keep
changes focused, and leave a short test contract in the PR: starting state,
actions, expected result, and what would count as a failure. Run host tests
first. Console-dependent work stays in a draft PR until a run receipt exists.
The receipt should identify the exact Git commit and artifact hash, plus the
console firmware and loader versions, test scenario, timed steps, log paths,
screenshots, and one verdict. A screenshot is evidence of visible state, not
proof that playback audio, HDR output, or system dialogs behaved correctly.

The agent may repeat offline builds and host tests freely. One PS5 cycle has a
bounded duration, one candidate artifact, and one scenario. After a crash,
unknown lifecycle state, Settings/Store/update screen, or unexpected prompt,
stop and ask the person at the console; do not retry automatically. Before a
PR is marked ready, the agent compares the receipt with the test contract and
lists anything a person still needs to see or hear. Merging and releases use
the project's normal review process.

The first pilot should be a simple navigation test on a development build:
launch Jelly5, reach the home screen, open one library, capture the frame and
log, close the app, and prove the slot is free. This exercises the whole loop
without involving playback, server writes, or private media.

## Delivery steps

### 1. Make the existing manual loop reliable

- Add a `--dry-run` and artifact manifest to `scripts/deploy.py`; validate the
  title ID and required files before connecting. Add remote readback and file
  mode checks. Use the same in-place folder semantics that already work.
- Add one local command for `build → host tests → artifact check`. Show a short
  result with paths to compile warnings and the ZIP. Keep console contact a
  separate, explicit command.
- Add a read-only `console doctor` for FTP, available log sources, ShadowMount
  version/API, and optional ELF loader. It must report `unknown` when it cannot
  prove the app is closed.

**Done when:** a dry run and offline tests cover invalid paths, partial uploads,
missing prerequisites, and a failed FTP transfer. No hardware write is needed
for this step.

### 2. Add a bounded console cycle

- Adapt the title-aware lifecycle and controller approach in
  [`ps5-homebrew-dev-protocol`](https://github.com/blackbearreloaded/ps5-homebrew-dev-protocol).
  Use a lock, ShadowMount's lifecycle log, and a title-scoped launch/close
  payload. First prove the protocol manually on this console and its loader;
  support macOS instead of assuming the reference repo's Linux/WSL environment.
- Save one ignored `build/test-runs/<timestamp>/` directory per run with the
  manifest, observations, logs, timings, and verdict. Bound every wait and
  refuse to infer success from mere network silence.
- Never turn a missing heartbeat into permission to deploy. If the app cannot
  be proved closed, ask the person at the console to close it.

**Done when:** two consecutive runs can deploy, start, collect evidence, close,
and report a verdict without manual file transfer; an uncertain state stops
without writing to the console.

### 3. Add input and visual evidence

- For a hardware-free route, add a development-only, explicitly enabled
  command channel in Jelly5. Reuse the existing input injector; accept only
  allowlisted buttons and bounded hold times. Capture the rendered frame on
  demand, downsample 4K, and store/serve it locally. The related Nuvio PS5
  code shows a command mailbox and scanout capture; it is a design reference,
  not an active Jelly5 feature.
- Gate the channel in both the build and runtime, bind any listener to the
  console's loopback or require a deliberate LAN debug flag, and never include
  it in release artifacts. Screenshots of the app cannot reveal PS5 system
  dialogs; manual observation or HDMI capture remains necessary there.
- If the user has an HDMI capture card, evaluate
  [`ps5-mcp`](https://github.com/leolobato/ps5-mcp) as an optional external
  visual/control layer. It requires macOS, the card, and its `padd` payload.
  Do not make it a build dependency.
- Without a capture card, prefer Jelly5's own dev screenshot for app screens.
  [Chiaki-ng](https://github.com/chiaki-ng/chiaki-ng) can provide a full-console
  Remote Play image after a one-time pairing, if Remote Play works on this
  jailbroken console. Treat that as an optional experiment: it is not needed
  for the first logged smoke test, and a Remote Play image cannot validate
  the TV's actual HDR or audio output.

**Done when:** a bounded script can enter a screen, press a button, capture a
frame, and compare app state/log assertions. A person still checks image
quality and playback behavior before declaring a release ready.

## Reference projects and decisions

| Project | Useful idea | Decision for Jelly5 |
| --- | --- | --- |
| [ps5-homebrew-dev-protocol](https://github.com/blackbearreloaded/ps5-homebrew-dev-protocol) | Console lock, lifecycle evidence, title-aware launch/close, one-run receipts | Adapt the protocol after verifying its assumptions on our PS5 |
| [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) | Temporary FTP names promoted per file, dry run, mode checks | Add the small upload safety improvements to `deploy.py` |
| [Nuvio-PS5](https://github.com/theghostonline/Nuvio-PS5) | Dev command mailbox, input injection, HDR-aware scanout screenshot | Implement only behind an explicit dev gate |
| [ps5-mcp](https://github.com/leolobato/ps5-mcp) | Live HDMI capture, virtual pad, MCP tools and recordings | Optional when capture hardware is available |
| [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng) | Remote Play video and controller input | Optional full-console view without a capture card, after pairing |
| [ps5-forwarder-manager](https://github.com/heydemoura/ps5-forwarder-manager) | One-shot scripted input and screenshot in app data | Simpler fallback for deterministic smoke tests |

Researching or adapting an idea does not make a project a runtime dependency.
Review licenses and add attribution when code is actually copied or vendored.
