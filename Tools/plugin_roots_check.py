#!/usr/bin/env python3
"""Run Linux automation with real companion plugins present before root discovery.

Build the editor first, then run:
    python3 Tools/plugin_roots_check.py ENGINE_ROOT HOST.uproject [--filter VaCuus]

The fixtures contain no modules, so installing them needs no additional compilation.
They live in an owned temporary directory under the host's Plugins directory and are
removed after the editor exits. Logs remain under the host's Saved/Logs directory.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time
import uuid


def write_plugin(parent, folder, name, document=None):
    root = parent / folder
    root.mkdir()
    (root / (name + ".uplugin")).write_text(json.dumps({
        "FileVersion": 3, "Version": 1, "VersionName": "1",
        "FriendlyName": "VaCuus document-root automation fixture",
        "EnabledByDefault": True, "CanContainContent": True,
    }), encoding="utf-8")
    if document:
        content = root / "Content" / "DevUI" / "RootProbe"
        content.mkdir(parents=True)
        (content / document).write_text("<rml><body>root probe</body></rml>\n", encoding="utf-8")
    return root


def run(args):
    editor = args.engine.resolve() / "Engine/Binaries/Linux/UnrealEditor-Cmd"
    project = args.project.resolve()
    if not editor.is_file() or not project.is_file():
        raise RuntimeError("Provide an engine with UnrealEditor-Cmd and an existing absolute .uproject")
    running = subprocess.run(["pgrep", "-a", "UnrealEditor"], capture_output=True, text=True)
    if running.returncode != 1:
        raise RuntimeError("Close existing editors before running this check: " + running.stdout + running.stderr)

    plugins = project.parent / "Plugins"
    plugins.mkdir(exist_ok=True)
    logs = project.parent / "Saved/Logs"
    logs.mkdir(parents=True, exist_ok=True)
    suffix = uuid.uuid4().hex[:12]
    log = logs / ("VaCuusPluginRoots-" + suffix + ".log")
    required = {"VaCuus.Core.ContentRoots", "VaCuus.Core.ContentRootComposition",
                "VaCuus.Core.PluginDocumentRoot", "VaCuus.Core.ShadowedDocuments"}

    with tempfile.TemporaryDirectory(prefix="VaCuusRootProbe_", dir=plugins) as temporary:
        root = Path(temporary)
        # Create Z before A so disk traversal need not agree with plugin-name order.
        write_plugin(root, "Z", "VaCuusRootProbeZ" + suffix, "z.rml")
        write_plugin(root, "A", "VaCuusRootProbeA" + suffix, "a.rml")
        write_plugin(root, "NoUI", "VaCuusRootProbeEmpty" + suffix)
        write_plugin(root, "Case", "VaCuusRootProbeCaseA" + suffix, "upper_unique.rml")
        if not (root / "case").exists():
            write_plugin(root, "case", "VaCuusRootProbeCaseZ" + suffix, "lower_unique.rml")

        print("Plugin-root fixtures installed; log: " + str(log), flush=True)
        with log.with_suffix(".stdout").open("w") as stdout:
            process = subprocess.Popen([
                str(editor), str(project), "-unattended", "-nullrhi", "-nosplash",
                "-abslog=" + str(log), "-ExecCmds=Automation RunTests " + args.filter + ", Quit,",
            ], stdin=subprocess.DEVNULL, stdout=stdout, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline:
                    text = log.read_text(errors="replace") if log.exists() else ""
                    if "Sending StopTestSession" in text:
                        break
                    if process.poll() is not None:
                        text = log.read_text(errors="replace") if log.exists() else ""
                        if "Sending StopTestSession" in text:
                            break
                        raise RuntimeError("Editor exited before test completion; inspect " + str(log))
                    time.sleep(0.25)
                else:
                    raise RuntimeError("Automation timed out; inspect " + str(log))
            finally:
                # Quit can remain deferred after automation. Stop only the PID we launched.
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()

        text = log.read_text(errors="replace")
        results = {name: result for result, name in re.findall(
            r"Test Completed\. Result=\{([^}]+)\}.*Path=\{([^}]+)\}", text)}
        missing = required - results.keys()
        failed = [name for name, result in results.items() if result != "Success"]
        if missing or failed or "Skipped: enabled plugin metadata" in text:
            raise RuntimeError(f"Missing: {sorted(missing)}; failed: {failed}; check skips in {log}")
        print(f"PLUGIN-ROOTS: {len(results)}/{len(results)} tests passed with companion plugins", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("project", type=Path)
    parser.add_argument("--filter", default="VaCuus.Core")
    parser.add_argument("--timeout", type=float, default=300)
    try:
        run(parser.parse_args())
    except (RuntimeError, OSError) as error:
        parser.exit(1, str(error) + "\n")
