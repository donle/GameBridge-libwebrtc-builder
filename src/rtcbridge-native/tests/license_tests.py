"""Exercise the exact upstream license builder and GN, with a small real graph."""
import argparse
import importlib.util
import io
import logging
import os
from pathlib import Path
import shutil
import re
import subprocess
import sys
import tempfile


def run_tests(source_root, depot_root, gn):
    repository = Path(__file__).resolve().parents[3]
    out = repository / "out"
    out.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="rtc-license-test-", dir=out) as temporary:
        scratch = Path(temporary)
        root = scratch / "src"

        def write(relative, text):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")

        (scratch / ".gclient").write_text("solutions = [{'name': 'src', 'url': None}]\n", encoding="utf-8")
        (scratch / ".gclient_entries").write_text("entries = {'src': None}\n", encoding="utf-8")
        write(".gn", 'buildconfig = "//BUILDCONFIG.gn"\n')
        write("BUILDCONFIG.gn", 'set_default_toolchain("//:fixture")\n')
        write("BUILD.gn", 'toolchain("fixture") { tool("stamp") { command = "cmd /c exit 0" } }\n')
        write("gamebridge/rtcbridge-native/BUILD.gn", '''
group("all") { deps = [ ":gamebridge_rtc", ":probe" ] }
group("gamebridge_rtc") { deps = [ "//third_party/abseil-cpp:dep", "//third_party/openh264:dep", "//third_party/ffmpeg:dep" ] }
group("probe") { deps = [ "//third_party/zlib:dep" ] }
''')
        write("third_party/abseil-cpp/BUILD.gn", 'group("dep") { deps = [ "//third_party/boringssl:dep" ] }\n')
        for library in ("boringssl", "openh264", "ffmpeg", "zlib"):
            write(f"third_party/{library}/BUILD.gn", 'group("dep") {}\n')
        # Resolver-only fixture; the scanner and gn.py are copied/invoked from
        # the exact upstream checkouts. No GN output or license text is mocked.
        write("build/find_depot_tools.py", f"DEPOT_TOOLS_PATH = {str(depot_root)!r}\n")
        write("buildtools/win/.keep", "")
        shutil.copy2(gn, root / "buildtools/win/gn.exe")
        write("tools_webrtc/libs/.keep", "")
        scanner = root / "tools_webrtc/libs/generate_licenses.py"
        shutil.copy2(source_root / "tools_webrtc/libs/generate_licenses.py", scanner)
        licenses = {
            "LICENSE": "WebRTC fixture license\n",
            "third_party/abseil-cpp/LICENSE": "Abseil fixture license\n",
            "third_party/boringssl/src/LICENSE": "BoringSSL transitive fixture license\n",
            "third_party/openh264/src/LICENSE": "OpenH264 fixture license\n",
            "third_party/ffmpeg/COPYING.LGPLv2.1": "FFmpeg LGPL fixture license\n",
            "third_party/zlib/LICENSE": "Probe-only zlib fixture license\n",
        }
        for name, text in licenses.items():
            write(name, text)
        generated = subprocess.run([str(gn), "gen", "out/Release", "--root-target=//gamebridge/rtcbridge-native:all"], cwd=root, capture_output=True, text=True)
        if generated.returncode:
            raise AssertionError(generated.stdout + generated.stderr)
        spec = importlib.util.spec_from_file_location("rtc_upstream_licenses", scanner)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        mapping = dict(module.LIB_TO_LICENSES_DICT)
        mapping["openh264"] = ["third_party/openh264/src/LICENSE"]
        mapping["ffmpeg"] = ["third_party/ffmpeg/COPYING.LGPLv2.1"]
        build_script = (repository / "scripts/build-libwebrtc-ci.ps1").read_text(encoding="utf-8")
        target_match = re.search(r"g\.LicenseBuilder\(\['out/Release'\], \['([^']+)'\], licenses\)", build_script)
        assert target_match, "Actual production license query target not found"
        builder = module.LicenseBuilder([str(root / "out/Release")], [target_match.group(1)], mapping)
        destination = root / "package"
        destination.mkdir()
        try:
            builder.generate_license_text(str(destination))
        except subprocess.CalledProcessError as error:
            # The production failure hid GN stdout; surface the exact cause.
            detail = (error.output or b"").decode("utf-8", errors="replace")
            raise AssertionError(f"Actual pinned GN license query failed:\n{detail}") from error
        combined = (destination / "LICENSE.md").read_text(encoding="utf-8")
        for text in licenses.values():
            assert text.strip() in combined, f"Missing real fixture license: {text}"
        assert "# boringssl" in combined, "Transitive dependency was omitted"
        diagnostics = io.StringIO()
        handler = logging.StreamHandler(diagnostics)
        previous_handlers = logging.getLogger().handlers[:]
        logging.getLogger().handlers = [handler]
        try:
            missing = dict(mapping)
            del missing["boringssl"]
            try:
                module.LicenseBuilder(builder.buildfile_dirs, builder.targets, missing).generate_license_text(str(destination))
            except Exception as error:
                assert "Missing licenses" in str(error) and "boringssl" in str(error)
            else:
                raise AssertionError("Unknown transitive library did not fail closed")
            (root / "third_party/boringssl/src/LICENSE").unlink()
            try:
                builder.generate_license_text(str(destination))
            except FileNotFoundError:
                pass
            else:
                raise AssertionError("Missing real license file did not fail closed")
            try:
                module.LicenseBuilder._run_gn(str(root / "out/Release"), "//missing:target")
            except subprocess.CalledProcessError:
                assert "gn desc failed (exit 1)" in diagnostics.getvalue()
                assert "missing" in diagnostics.getvalue(), "GN root failure detail was lost"
            else:
                raise AssertionError("Invalid GN root did not fail closed")
        finally:
            logging.getLogger().handlers = previous_handlers
        print("licenses: real GN custom-root query, all variants/transitive licenses, missing-license rejection and GN diagnostics PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--depot-root", required=True, type=Path)
    parser.add_argument("--gn", required=True, type=Path)
    args = parser.parse_args()
    os.environ["DEPOT_TOOLS_METRICS"] = "0"
    run_tests(args.source_root.resolve(), args.depot_root.resolve(), args.gn.resolve())
