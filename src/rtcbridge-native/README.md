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
Video transceivers explicitly advertise a 25 Mbps encoding ceiling and 60 fps.
The connection starts its bandwidth estimate at 25 Mbps, with its existing
40 Mbps transport ceiling for overhead. Encoding/connection minima stay at
1 Mbps so congestion control can back off below the normal 12-25 Mbps quality
range. The connection-wide limit alone did not override upstream's 2.5 Mbps
singlecast default; overshoot adjustment could reduce that to 1.25 Mbps. These
are allocation hints and bounds, not forced throughput or disabled GCC. The
external encoder still must obey bitrate/keyframe feedback on slower paths.
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
The pinned Windows `WinsockInitializer` is the runtime's first member, before
socket-server construction. SSL initializes before any thread starts or factory
is created; neither global SSL nor Winsock is torn down while peers can exist.
The ABI test reports only fixed state enums, event/operation counts and result
codes, never SDP, candidate addresses, credentials or opaque error strings. It
forwards both end-of-gathering markers after preceding candidates and retains
the five-second deadline. Empty candidate markers are accepted locally because
the pinned upstream explicitly rejects a null `AddIceCandidate`.

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
   The external preflight explicitly uses depot_tools' pinned `vpython.toml`
   (Python 3.11), rather than vpython's default interpreter. An existing isolated
   Git configuration inside the build directory disables bootstrap global
   configuration changes; the runner user's Git configuration is not modified.
   GN reads the unchanged pinned build arguments directly from UTF-8 `args.gn`,
   avoiding PowerShell property expansion and batch-wrapper quote processing.
   Before the full graph, a regression runs the actual production GN block on
   a tiny SDK-free graph with the synchronized GN binary and checks every value.
   Focused Winsock/core/injector/diagnostic and sender-bitrate tests run first.
   Sender tests execute the verbatim bridge configuration with the hash-pinned
   upstream singlecast-limit algorithm and the actual unmodified pacer. The
   clock/source model covers 900 frames at 25.08 Mbps with periodic keyframes;
   default-rate scenarios must expose truncation and explicit-rate scenarios
   must deliver every frame. It is not a GCC, network or native-gate replay.
   Production ABI runs
   before the remaining probe/benchmark variants are built. No build cache is
   used, and attestation still requires every ABI/media gate to pass.
   The hash-pinned `license-root-target.patch` gives upstream license queries
   the same reachable custom-root graph and preserves GN failure diagnostics.
   Before compilation, upstream's real recursive scanner collects the whole
   `:all` graph, including all binary variants, and rejects unknown libraries
   or missing license files. Packaging requires matching complete `LICENSE.md`
   and `THIRD_PARTY_LICENSES.md` plus `PATENTS.txt`; no license failure is ignored.
   The workflow runs core,
   production and probe ABI tests, then the real five-second media gate. Only
   success produces the SHA-256 archive and GitHub provenance attestation.
   Every gate attempt logs and retains the complete allowlisted numeric report,
   including histogram, producer timing and drop accounting even on failure.
   CI uploads only that sanitized JSON under a separate diagnostics artifact;
   raw consumer output, SDP, candidate addresses and arbitrary error strings
   are never uploaded. A diagnostics artifact is not a passing build or signed
   release. Five-second and 1800-second thresholds remain unchanged; there is
   no automatic retry or timing-based waiver.
   Consumer fatal counters are partitioned by reason (header, timestamp,
   declared payload byte range, duplicate, payload, bridge error, other) and
   callback-observation phase (setup, prewarm, measurement, drain, teardown).
   Both partitions sum to `fatal_errors`; teardown is diagnostic, not an error
   exemption. The context is still marked closed only after close returns.
   Payload diagnostics distinguish size/content mismatch and bytes matching a
   different fixture frame, without logging any payload. Fixed bridge error
   counts distinguish the two channel closures, connection failure and other.
   `bridge_error_code_mask` further uses bits 0..9 for, in order:
   `control_channel_closed`, `pointer_channel_closed`, `connection_failed`,
   `event_oversize`, `control_overflow`, `event_overflow`, `description_rejected`,
   `candidate_rejected`, `channel_failed`, `media_write_failed`; bit 10 is unknown.
   Fatal/stale/metrics errors are checked first in deterministic validation
   order, with all original zero-error and performance bounds retained.
   Missing-frame diagnostics include only accepted-but-unseen measurement
   indices: first 64 sorted indices, exact total/omitted count, head/tail/interior
   counts and contiguous-run sizes. Head and tail overlap if every frame is
   missing; interior does not double-count that overlap. RTP timestamps are
   derived from the fixed first timestamp and step, not network identifiers.
   Counts before/during/after the unchanged two-second drain and last valid
   callback age distinguish late delivery from persistent gaps.
   Benchmark-only native injection/sender-transform/receiver-transform and
   bitrate-update totals span the entire session, including prewarm. Numeric
   RTCStats snapshots reuse the existing one-second polling, with sample age;
   they are cumulative, possibly cached and not exact measurement-window
   deltas. Absent values are omitted. Raw allocated bitrate and bandwidth
   allocation are separate from candidate-pair available outgoing bitrate.
   Inbound frame/decode-dependent stats need not equal receiver-transform
   counts because this encoded-only bridge stops before the reference finder
   and decoder. No extra stats polling or production diagnostics are enabled.
   Both initial and final public reports use same-directory staged writes,
   flush-to-disk and close before an atomic replacement. Existing reports use
   `File.Replace` (PowerShell `Move-Item -Force` has a delete window); first
   publication uses a non-overwriting `Move-Item`. Only the same `File.Replace`
   operation may retry recognized Win32 IOExceptions 5/32/33/1175: at most 41
   attempts within a 1000 ms elapsed retry budget, with delays at most 25 ms.
   Individual synchronous OS calls are not timed out. Other errors, including
   1176/1177 with different recovery semantics, fail without a fallback.
   Serialization failure, tested persistent locks and cancellation before the
   replace preserve the preceding valid report byte-for-byte. A transient lock
   can release within the bounded window; this never reruns preparation or the
   benchmark. Initial publication failure prevents preparation from starting.
   Temporary/raw files are not upload inputs.
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
- [Windows atomic replacement and error recovery semantics](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)

Hosted image contents can change; its actual image version, source pins, GN
arguments, SDK installer hash, reclaimed paths and disk readings are retained in
the attested `native-build.json`. The selected September 2026 image documents
VS2026 18.9.12120.119 but only SDK 26100 preinstalled, hence explicit SDK setup.
