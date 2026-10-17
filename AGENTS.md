# Jelly5 agent instructions

Jelly5 is a native Jellyfin and Emby client for jailbroken PS5s (`PPSA99505`, Media category). Its UI is GPU drawn; playback uses the vendored EVO/Nuvio engine. The project is GPL-3.0-or-later. Read the relevant code and current tests before changing behaviour. The README describes the product; this file records working rules.

## Work and review

- Use descriptive branches such as `bug/`, `fix/`, `feature/`, `docs/` or `chore/`. Do not create `codex/` branches.
- Keep a PR focused. State what changed, how it was verified, and a concrete PS5 test plan for console-dependent changes. Leave untested playback changes as draft PRs until the console test is done.
- Treat projects under local `refs/` as technical references. Compare their implementation with Jelly5 before porting anything. Add a project to credits and third-party notices when code is actually reused, with its original notices and license.
- Existing local notes in git-ignored `private/` can provide history but may be stale. Verify decisions against current code, tests and issues. Never require those notes for a clean clone to build.
- Keep credentials, server addresses, user names, tokens, and machine-specific values out of commits and logs. `.env.local` is private.
- For console work, follow `docs/DEVELOPMENT.md`. A network check or a missing log is not proof that Jelly5 has closed; stop a deploy when the app's state is uncertain.

## Map and build

- `app/src/jf/`: Jellyfin/Emby client and host-testable request logic; `app/src/app/`: accounts, settings, services and safe threading; `app/src/ui/`: screens and player UI; `app/src/gfx/`: GPU UI and artwork; `app/src/nuvio_*`: player, input and subtitles; `app/engine/`: vendored media engine.
- `app/scripts/build.sh` builds the PS5 app. On macOS use Homebrew Bash 5, after `scripts/setup-toolchain.sh --env`; use `--release` for a distributable ZIP. The build requires `LC_ALL=C` so the stub selection cannot pick WebKit kernel imports.
- Check build warnings in `app/build/compile.log`, package integrity, and `llvm-readelf -d app/build/llvm-pie.elf`: `libkernel.sprx` is expected; `libkernel_web` is not.
- Run tests that exercise the changed area. `app/tests/host/routes.sh` guards Jellyfin requests; `input.sh` covers controller and media remote; `run.sh` checks the API client; `seerr.sh`, `livetv.sh`, `letters.sh` and `subsync.sh` cover their named areas. Check actual filenames before running commands.
- Never run a test that writes to a user's Jellyfin or Emby server. Read-only API checks are fine. Use dedicated local test servers for writes. Seerr's `--auth quickconnect` and `--for-real`, and Live TV's `--record`, perform writes.

## PS5 safety and platform

- Never deploy over a running Jelly5 process. With the current tools, ask the user to close it with the PS button and wait for confirmation; a future automated cycle must prove the title is closed from lifecycle evidence first. Update files in place under `/data/homebrew/PPSA99505`; do not replace that directory or place another folder with the same title ID under `/data/homebrew`.
- The app must not call `exit()` to close itself. `opendir()` fails in its sandbox; use `evo_opendir`.
- Use `jelly5::spawn` or `jelly5::run_all` for new async work instead of a bare `std::thread`. Handle spawn failure and capture shared lifetime in detached work. Do network work off the render thread. Decide under a mutex, then act after releasing it when the action could reacquire the same lock.
- GPU resources must come from GPU-visible memory. `evo_direct_mem` can fall back to `malloc`; that memory must not reach the GPU. UI textures use `gfx_pool`; `gfx::texture_release` defers freeing for in-flight frames.
- Keep `app/sce_sys/param.json` in the Media category with HDR-capable `attribute` bit `0x20000000`. Avoid changing PS5 title flags without a console test.
- System fonts live under the sandbox word's `common/font/`, not `/preinst`; use `sceKernelGetFsSandboxRandomWord()` when resolving them. The vendored engine uses full cJSON 1.7.18; do not restore its old stub implementation.
- Crash addresses are offsets into `app/build/llvm-pie.elf`; symbolise them with `llvm-symbolizer --obj=app/build/llvm-pie.elf`.

## UI and source conventions

- The logical UI canvas is 1920×1080, rendered at the display's resolution. Use `ui::Anim`, `ui::Drop`, `glass_panel`, and `art::draw` consistently with nearby screens. Keep network, decoding and disk work off the render thread.
- Every viewer-facing string goes through `T("…")`: write the source in Norwegian and add English in `app/src/app/i18n.cpp`. Translate at use time, never in a static initializer.
- New source files use the Jelly5 header and `SPDX-License-Identifier: GPL-3.0-or-later`. Preserve upstream copyright and license notices when adapting code.

## Media and server invariants

- One `jf::Client` belongs to one account/session and one server kind. Initialise it before use; do not mutate its server or session while screens or requests may still use it.
- UI code asks `Client::features()` rather than assuming a Jellyfin or Emby feature from the server kind. Differences in API routes live inside `jf::Client`; preserve Jellyfin requests when changing Emby paths. Item IDs are opaque strings, even when Emby sends numbers.
- Use `Client::token_param()` for token-bearing URLs (`ApiKey=` for Jellyfin, `api_key=` for Emby). Never log or cache such URLs. WebSocket and playback URLs can carry tokens.
- Keep Seerr request seasons explicit; `"all"` drops specials. Seerr status is thread-local, so read it on the same thread as the request. Its server and frontend are references for API behaviour.
- Live TV PlaybackInfo may open a server stream. Close every `LiveStreamId`, including an opened stream that never plays; stop the old channel before zapping to the next. Keep a `GuideRef` alive while using pointers into its snapshot.
- Current platform limits: no true 24p output; AV1 uses CPU dav1d, with 4K capped at 30 fps until measured; Dolby Vision 5 is rebuilt as HDR10 on the GPU; TrueHD and DTS-HD decode to PCM while supported core bitstreams may go over HDMI.
