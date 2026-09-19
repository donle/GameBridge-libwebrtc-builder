"""Read-only preflight against the actual pinned Windows depot_tools modules."""
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    if sys.platform != "win32" or len(sys.argv) != 3:
        raise RuntimeError("Usage on Windows: depot_git_tests.py DEPOT_ROOT GIT_EXE")
    depot = Path(sys.argv[1]).resolve()
    expected_git = Path(sys.argv[2]).resolve(strict=True)
    if expected_git.name.lower() != "git.exe":
        raise RuntimeError("Expected a resolved native git.exe, not a batch stub")
    print(f"depot preflight interpreter: {sys.version.split()[0]} ({sys.executable})", flush=True)
    os.environ["DEPOT_TOOLS_METRICS"] = "0"
    sys.path.insert(0, str(depot))
    import git_cache
    import git_common

    # Reproduce the failed sync call itself before doing any network operation.
    # A missing cache config is expected; its fallback must be reachable without
    # FileNotFoundError. Never read or modify the user's/global Git configuration.
    git_cache.Mirror._GIT_CONFIG_LOCATION = ["--file", os.devnull]
    cache_probe = str(depot.parent / "git-cache-preflight-probe")
    os.environ["GIT_CACHE_PATH"] = cache_probe
    if git_cache.Mirror.GetCachePath() != cache_probe:
        raise RuntimeError("Pinned Mirror.GetCachePath did not reach its fallback")

    stub = shutil.which("git.bat")
    if not stub or not os.path.samefile(stub, depot / "git.bat"):
        raise RuntimeError("The pinned depot_tools Git wrapper must win PATH lookup")
    if not os.path.samefile(git_common.GIT_EXE, expected_git):
        raise RuntimeError("Generated Git wrapper resolved to an unexpected executable")
    native_version = subprocess.check_output([str(expected_git), "--version"])
    wrapped_version = subprocess.check_output([stub, "--version"])
    if native_version != wrapped_version:
        raise RuntimeError("Git wrapper and native Git version disagree")
    print("depot preflight: pinned Git wrapper, native executable and actual cache lookup PASS")
    print(native_version.decode("utf-8").strip())


if __name__ == "__main__":
    main()
