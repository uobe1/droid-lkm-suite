"""Parse the workflows and syntax-check every embedded shell block."""

import pathlib
import subprocess
import tempfile
import unittest

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOWS = [ROOT / ".github/workflows/qemu-core.yml",
             ROOT / ".github/workflows/device-image.yml"]


class WorkflowSyntaxTest(unittest.TestCase):
    def test_run_blocks_parse_as_bash(self):
        for wf in WORKFLOWS:
            data = yaml.safe_load(wf.read_text())
            for job, spec in data.get("jobs", {}).items():
                for i, step in enumerate(spec.get("steps", [])):
                    run = step.get("run")
                    if not run:
                        continue
                    with tempfile.NamedTemporaryFile(
                            "w", suffix=".sh", delete=False) as handle:
                        handle.write(run)
                        path = handle.name
                    try:
                        result = subprocess.run(["bash", "-n", path],
                                                capture_output=True,
                                                text=True)
                    finally:
                        pathlib.Path(path).unlink()
                    self.assertEqual(result.returncode, 0,
                                     (str(wf), job, i, result.stderr))

    def test_cgpids_probe_wiring(self):
        probe = "dnsk_lkm=1 cgpids_native=1 cgpids_rw_probe=1"
        for wf in WORKFLOWS:
            self.assertIn(probe, wf.read_text(), str(wf))
        device = (ROOT / ".github/workflows/device-image.yml").read_text()
        self.assertIn("- cgpids", device)
        self.assertIn("DNSK_CGPIDS_OK", device)
        self.assertIn("dnsk_rwprobe:", device)


if __name__ == "__main__":
    unittest.main()