# Native RTC fallback build candidate

This is the conditional Plan 3 fallback after two measured Pion gate failures.
Do not promote this candidate until the hosted ABI tests and a local full
1800-second gate pass. Syntax checking and core tests are not transport
qualification.

The public ABI is the canonical `src/rtcbridge/include/gamebridge_rtc.h`.
The native include forwards to that header. Production exports nine functions;
the probe DLL adds one test export, and the benchmark DLL adds two measurement
exports. Code and instrumentation remain separate build variants.

Pinned libwebrtc `c250ac7568212f05892447d8e8673f8e55d716d9` exposes encoded-frame
injectors. The bridge submits H.264/Opus through those APIs and receives encoded
frames through receiver transforms, before native decoding. A dummy audio
device prevents microphone capture and system audio playback. Sender transforms
preserve the ABI's exact caller RTP timestamps after libwebrtc's random offset.
Application video queues have one pending AU; audio queues have a 200 ms bound.
Only one additional injection per stream is admitted until its sender transform
completes, bounding the upstream injector entry queue. Libwebrtc still owns its
internal RTP pacer/retransmission buffers; these are not application AU queues.
The hash-pinned `patches/encoder-ready-drain.patch` drains the first buffered
video frame when its encoder becomes ready, under upstream's existing lock and
sequence guard. No second user frame or duplicate/dummy encoded AU is injected.
Metrics include receiver queue replacement and receiver lock-contention drops,
not a direct packet-loss count.

Production handles are generation checked and never wrap retired generations.
Each session has serialized callbacks. External close cancels admission and
waits for borrowed callbacks; callback-originated close across any sessions
does not block. Unused sessions do not instantiate a PeerConnection, preserving
cheap create/close and stale-handle tests. Libwebrtc worker threads/factory live
for the process lifetime, so unloading the DLL while the process runs is unsupported.

Build and artifact flow:

1. Run `scripts/export-rtc-builder.ps1` into a new D: directory. Review its
   deterministic file/hash manifest. It exports only this bridge, the canonical
   ABI, shared ABI/benchmark tests, and bounded build scripts. It creates no Git
   repository, remote, commit or push.
2. Publish that reviewed minimal export to an explicitly approved GitHub
   repository, with `main` as its default branch. Public repositories support
   attestations without the private-repository Enterprise Cloud requirement.
3. Dispatch `.github/workflows/build-libwebrtc.yml`. The script refuses to run
   locally. The `windows-2025-vs2026` hosted image supplies VS2026 C++/ATL/MFC;
   the pinned Microsoft-signed SDK installer adds 10.0.28000.2270 and debugger
   tools. Before cleanup, measured free space selects D: preferentially and
   skips all deletion if D: or C: already meets 60 GiB. Only if neither fits,
   fixed, exact-identity, non-reparse cache directories may be reclaimed until
   a disk reaches the threshold. VS/Windows Kits are never cleanup targets.
   Before/after measurements and skipped cleanup are recorded; sync still
   fails unless at least 60 GiB remains after SDK provisioning.
4. Pinned depot_tools, libwebrtc and DEPS plus the hash-checked startup patch
   build the three DLL variants plus the actual shared ABI/native benchmark
   consumers. Depot auto-update remains disabled; its pinned official Windows
   bootstrap is called explicitly to generate the required Git wrappers. PATH
   keeps depot_tools first and the selected native Git directory second. Ordered
   application matches are reduced to the first valid Git installation's
   canonical `cmd/git.exe`, including when its `bin/git.exe` matched first. A real
   `Mirror.GetCachePath` subprocess preflight verifies the wrapper and exact
   `git.exe` binding before sync; `gclient root` verifies command readiness.
   The workflow runs core,
   production and probe ABI tests, then the real five-second media gate. Only
   success produces the SHA-256 archive and GitHub provenance attestation.
   The cold-start regression compiles the actual pinned injector method bodies
   with isolated engine dependencies; actual ABI CI also waits for one video
   frame before making any further media/data call.
5. `scripts/fetch-rtc-artifact.ps1 -Repository owner/repo -RunId number
   -SourceRevision forty_hex_commit` downloads to D:, validates successful run,
   workflow/source identity, SHA-256 and GitHub attestation before extracting
   executables. Verified statement and certificate invocation URLs must match
   the requested run and its successful attempt. Binary download is streaming
   capped at 300 MiB; ZIP validation/extraction counts actual bytes, caps each
   entry and aggregate expansion at 256 MiB, and rejects declared-size mismatch.
   ZIP entries are allowlisted and reparse paths rejected.
   It never invokes an installer or overwrites a prior verified artifact.
6. `scripts/test-rtc-native-gate.ps1 -ArtifactDirectory D:\...\verified
   -DurationSeconds 1800 -Output artifacts/rtc-native-gate.json` uses the same
   deterministic 1080p60/~25 Mbps fixture and native consumer. All performance
   thresholds are unchanged. Native GC is explicitly not applicable: no Go
   trace or pause is invented. The Pion launcher/validator remains unchanged.

Upstream references (pins are recorded separately in `pins.json`):

- [Chromium Windows requirements](https://chromium.googlesource.com/chromium/src/+/main/docs/windows_build_instructions.md)
- [Pinned Chromium toolchain requirements](https://chromium.googlesource.com/chromium/src/build/+/a56d226360eb0b3eaf423bfbf3cfe35a19e4951a/vs_toolchain.py)
- [Microsoft Windows SDK release downloads](https://learn.microsoft.com/en-us/windows/apps/windows-sdk/downloads)
- [Hosted Windows runner limits](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
- [VS2026 runner image](https://github.com/actions/runner-images/blob/main/images/windows/Windows2025-VS2026-Readme.md)

Hosted image contents can change; its actual image version, source pins, GN
arguments, SDK installer hash, reclaimed paths and disk readings are retained in
the attested `native-build.json`. The selected September 2026 image documents
VS2026 18.9.12120.119 but only SDK 26100 preinstalled, hence explicit SDK setup.
