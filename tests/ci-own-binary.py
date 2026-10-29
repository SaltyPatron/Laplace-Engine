#!/usr/bin/env python3
"""Every job of the Engine's CI runs the binary its own run built (Laplace-Engine CI).

The build directory ($LAPLACE_BUILD) is the self-hosted runner's, shared by every run: a job that reads the binary from
it reads whatever run built last (a pull request's plan ran another run's binary, which knew no `counted`). So the
job that builds hands its binary over as the run's artifact, and every other job that runs laplace downloads it.
    tests/ci-own-binary.py .github/workflows/laplace.yml
"""
import re, sys, unittest

WORKFLOW = sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else ".github/workflows/laplace.yml"


def jobs(text):
    body = text.split("\njobs:\n", 1)[1]
    parts = re.split(r"\n  (?=[A-Za-z_][\w-]*:\n)", "\n" + body)
    return {p.split(":", 1)[0].strip(): p for p in parts if p.strip()}


def builds(job):
    return "cmake --build" in job


def runs_laplace(job):
    return bool(re.search(r"(/laplace\b|\$L\b|LAPLACE_ENGINE_BIN)", job))


class OwnBinary(unittest.TestCase):
    def setUp(self):
        with open(WORKFLOW, encoding="utf-8") as f:
            self.jobs = jobs(f.read().replace("\r\n", "\n"))

    def test_one_job_builds_and_hands_its_binary_over(self):
        builders = [n for n, j in self.jobs.items() if builds(j)]
        self.assertEqual(len(builders), 1, builders)
        j = self.jobs[builders[0]]
        self.assertIn("actions/upload-artifact", j)
        self.assertIn("name: laplace-engine", j)

    def test_test_only_changes_do_not_dispatch_runtime_deployment(self):
        self.assertIn("needs.engine.outputs.parity == 'true'", self.jobs['deploy'])

    def test_no_other_job_reads_the_shared_build_directory(self):
        for name, j in self.jobs.items():
            if builds(j):
                continue
            self.assertNotRegex(j, r"\$\{?LAPLACE_BUILD\}?/Laplace-Engine", f"job {name} reads the runner's shared build directory")

    def test_every_job_that_runs_laplace_downloads_the_runs_own(self):
        for name, j in self.jobs.items():
            if builds(j) or not runs_laplace(j):
                continue
            self.assertIn("actions/download-artifact", j, f"job {name} runs laplace without the run's own binary")
            self.assertIn("name: laplace-engine", j, name)
            self.assertIn("sha256sum -c", j, f"job {name} does not check the binary it was handed")


if __name__ == "__main__":
    unittest.main(verbosity=1)
