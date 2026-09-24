"""Tests for the paper's experiment selection and artifact execution boundaries."""

from collections import Counter
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import run_artifact as artifact
from scripts.yoked_codes import run_all_workloads as workloads


class PaperPlanTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="artifact-plan-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.paths = dict(
            build_dir=self.root / "build",
            raw_bin_dir=self.root / "raw",
            run_dir=self.root / "run",
        )
        self.benchmark = workloads.BENCHMARKS[0]

    def plan(self, **kwargs):
        return workloads.build_paper_plan([self.benchmark], **self.paths, **kwargs)

    def test_exact_simulation_union_and_no_extra_parameter_sweeps(self):
        expected = set()
        for compute in (4, 8, 12, 16):
            for baseline in (0, 1):
                expected.add(("firstpass", f"c{compute}_baseline{baseline}_i0_csm194.log"))
            expected.add(("cache", f"c{compute}_i8_rri_csm194.log"))
            expected.add(("prefetch", f"c{compute}_i8_lru_mld0_csm194.log"))
        expected.add(("cache", "c4_i8_lru_csm194.log"))
        for intermediate in (4, 16, 24):
            expected.add(("cache", f"c4_i{intermediate}_rri_csm194.log"))
            expected.add(("prefetch", f"c4_i{intermediate}_lru_mld0_csm194.log"))
        for cold_capacity in (34, 98, 322, 482):
            expected.add(("cache", f"c4_i8_rri_csm{cold_capacity}.log"))
            expected.add(("prefetch", f"c4_i8_lru_mld0_csm{cold_capacity}.log"))

        plan = self.plan()
        simulations = [step for step in plan if step.stage == "simulate"]
        actual = {(step.log_path.parts[-3], step.log_path.name) for step in simulations}
        self.assertEqual(actual, expected)
        self.assertEqual(len(simulations), len(expected))
        self.assertEqual(Counter(step.stage for step in plan), {"compile": 19, "simulate": 31})
        for step in simulations:
            self.assertNotIn("--cold-storage-inner-code-distance", step.command)
            self.assertNotIn("--effective-code-distance", step.command)
            self.assertEqual(step.command[2], str(self.benchmark.sim_instructions))
            self.assertTrue(Path(step.command[0]).is_absolute())

    def test_all_ten_benchmarks_have_500_unique_steps(self):
        plan = workloads.build_paper_plan(workloads.BENCHMARKS, **self.paths)
        self.assertEqual(len(plan), 500)
        self.assertEqual(Counter(step.stage for step in plan), {"compile": 190, "simulate": 310})
        self.assertEqual(len({step.log_path for step in plan}), 500)
        self.assertEqual(
            Counter(step.benchmark for step in plan),
            {benchmark.label: 50 for benchmark in workloads.BENCHMARKS},
        )

    def test_compilation_is_deduplicated_and_precedes_every_consumer(self):
        produced = set()
        simulation_uses = Counter()
        for step in self.plan():
            if step.stage == "compile":
                self.assertNotIn(step.output_path, produced)
                for flag in ("--rri-input", "--singlepass-prefetch-input"):
                    if flag in step.command:
                        self.assertIn(Path(step.command[step.command.index(flag) + 1]), produced)
                produced.add(step.output_path)
            else:
                source = Path(step.command[1])
                self.assertIn(source, produced)
                simulation_uses[source] += 1
        self.assertEqual(len(produced), 19)
        self.assertEqual(set(simulation_uses), produced)
        # These two c4/i8 traces serve the five cold-storage-capacity points.
        self.assertEqual(sorted(simulation_uses.values()).count(5), 2)

    def test_short_simulation_limit_keeps_required_compiler_lookahead_headroom(self):
        for limit in (100, 1000):
            with self.subTest(limit=limit):
                for step in self.plan(instruction_limit=limit):
                    expected = limit
                    if step.stage == "compile":
                        actual = step.command[step.command.index("-i") + 1]
                        secondpass = (
                            "--rri-input" in step.command
                            or "--singlepass-prefetch-input" in step.command
                        )
                        # Existing loaders need more trace data than tiny N;
                        # firstpass additionally supplies secondpass lookahead.
                        expected += workloads.SECOND_PASS_INST_LIMIT_DELTA * (1 if secondpass else 2)
                    else:
                        actual = step.command[2]
                    self.assertEqual(actual, str(expected))
        for limit in (0, -1):
            with self.subTest(invalid_limit=limit):
                with self.assertRaises(ValueError):
                    self.plan(instruction_limit=limit)

    def test_full_compilation_limits_and_legacy_paths_are_preserved(self):
        original_paths = (
            workloads.BUILD_DIR, workloads.RAW_BIN_DIR, workloads.MEM_DIR,
            workloads.MEM_FIRSTPASS_DIR, workloads.LOG_DIR,
        )
        for step in self.plan():
            if step.stage != "compile":
                continue
            expected = self.benchmark.compile_inst_limit
            if "--rri-input" in step.command or "--singlepass-prefetch-input" in step.command:
                expected -= workloads.SECOND_PASS_INST_LIMIT_DELTA
            self.assertEqual(step.command[step.command.index("-i") + 1], str(expected))
        self.assertEqual(original_paths, (
            workloads.BUILD_DIR, workloads.RAW_BIN_DIR, workloads.MEM_DIR,
            workloads.MEM_FIRSTPASS_DIR, workloads.LOG_DIR,
        ))
        self.assertFalse(any(self.root.iterdir()), "Planning created output files or directories")


class ArtifactRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="artifact-cli-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.benchmark = workloads.BENCHMARKS[0]
        self.argv = [
            "--benchmarks", self.benchmark.label,
            "--build-dir", str(self.root / "missing-build"),
            "--binary-dir", str(self.root / "missing-inputs"),
            "--run-dir", str(self.root / "run"),
            "--results-dir", str(self.root / "results"),
        ]

    def test_invalid_instruction_limits_are_cli_errors(self):
        for value in ("0", "-1", "not-a-number"):
            with self.subTest(value=value), redirect_stderr(StringIO()):
                with self.assertRaises(SystemExit) as result:
                    artifact.parse_args(["--instructions", value])
                self.assertEqual(result.exception.code, 2)

    def test_short_runs_automatically_isolate_logs_results_and_plots(self):
        with patch.object(artifact, "ROOT", self.root):
            full = artifact.parse_args([])
            short = artifact.parse_args(["--instructions", "100"])
        self.assertEqual(full.run_dir, self.root / "build/yoked_codes_run_all_workloads")
        self.assertEqual(full.results_dir, self.root / "results")
        self.assertEqual(short.run_dir, self.root / "build/artifact_smoke_100")
        self.assertEqual(short.log_dir, short.run_dir / "logs")
        self.assertEqual(short.results_dir, self.root / "results/smoke_100")
        self.assertEqual(short.plot_dir, short.results_dir)
        explicit = artifact.parse_args(self.argv + ["--instructions", "100"])
        self.assertEqual(explicit.run_dir, self.root / "run")
        self.assertEqual(explicit.results_dir, self.root / "results")
        self.assertFalse(any(self.root.iterdir()))

    def test_dry_run_needs_no_inputs_or_executables_and_writes_nothing(self):
        output = StringIO()
        with (
            patch.object(artifact, "check_prerequisites", side_effect=AssertionError("checked inputs")),
            patch.object(artifact.subprocess, "run", side_effect=AssertionError("executed command")),
            patch.object(artifact, "render_plots", side_effect=AssertionError("rendered plots")),
            redirect_stdout(output),
        ):
            result = artifact.main(self.argv + ["--dry-run", "--instructions", "100", "--plots"])
        self.assertEqual(result, 0)
        self.assertIn("19 compilation passes, 31 simulations", output.getvalue())
        self.assertIn("extract_ipc_c4_i8.py", output.getvalue())
        self.assertFalse(any(self.root.iterdir()))

    def test_failed_subprocess_stops_workflow_before_extraction(self):
        with (
            patch.object(artifact, "check_prerequisites"),
            patch.object(artifact.subprocess, "run", return_value=SimpleNamespace(returncode=7)) as run,
            patch.object(artifact, "extract_results") as extract,
            redirect_stdout(StringIO()),
            redirect_stderr(StringIO()) as errors,
        ):
            result = artifact.main(self.argv)
        self.assertEqual(result, 1)
        self.assertIn("exit 7", errors.getvalue())
        run.assert_called_once()
        extract.assert_not_called()
        manifest = json.loads((self.root / "run/artifact_manifest.json").read_text())
        self.assertEqual(manifest["status"], "failed")
        partial_logs = list((self.root / "run/logs").rglob("*.partial"))
        self.assertEqual(len(partial_logs), 1)
        self.assertFalse((self.root / "results").exists())

    def _write_existing_logs(self):
        args = artifact.parse_args(
            self.argv + ["--extract-only", "--log-dir", str(self.root / "old-run/logs")]
        )
        plan = workloads.build_paper_plan(
            args.benchmarks, build_dir=args.build_dir,
            raw_bin_dir=args.binary_dir, run_dir=args.run_dir,
        )
        required = []
        for step in plan:
            if step.stage != "simulate":
                continue
            path = args.log_dir / step.log_path.relative_to(args.run_dir / "logs")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(
                "IPC 1.0\nTOTAL_SIMULATION_CYCLES 100\nUNROLLED_INSTRUCTIONS_DONE 100\n"
            )
            required.append(path)
        coverage = args.log_dir / "compile/prefetch" / self.benchmark.label / "c4_i8_lru_mld0.log"
        coverage.parent.mkdir(parents=True, exist_ok=True)
        coverage.write_text("SP_PREFETCH_OPERATIONS 10\n")
        required.append(coverage)
        args.results_dir.mkdir(parents=True)
        previous_csv = args.results_dir / "main_results.csv"
        previous_csv.write_text("previous results\n")
        return args, plan, required, previous_csv

    def test_failed_or_running_manifest_cannot_extract_stale_complete_logs(self):
        args, plan, _, previous_csv = self._write_existing_logs()
        # Existing logs from the legacy runner have no manifest and remain valid.
        artifact.validate_logs(plan, args)
        for status in ("failed", "running"):
            with self.subTest(status=status):
                (args.log_dir.parent / "artifact_manifest.json").write_text(
                    json.dumps({"status": status})
                )
                with (
                    patch.object(artifact.subprocess, "run") as run,
                    self.assertRaises(RuntimeError),
                ):
                    artifact.extract_results(plan, args)
                run.assert_not_called()
                self.assertEqual(previous_csv.read_text(), "previous results\n")

    def test_partial_replacement_invalidates_simulation_and_coverage_logs(self):
        args, plan, required, previous_csv = self._write_existing_logs()
        (args.log_dir.parent / "artifact_manifest.json").write_text(
            json.dumps({"status": "complete"})
        )
        artifact.validate_logs(plan, args)
        # Both log types are consumed by the notebook's extractors.
        for path in (required[0], required[-1]):
            with self.subTest(log=path):
                partial = path.with_suffix(".log.partial")
                partial.write_text("failed replacement\n")
                try:
                    with (
                        patch.object(artifact.subprocess, "run") as run,
                        self.assertRaises(RuntimeError),
                    ):
                        artifact.extract_results(plan, args)
                    run.assert_not_called()
                    self.assertEqual(previous_csv.read_text(), "previous results\n")
                finally:
                    partial.unlink()

    def test_extract_only_skips_compilers_and_build_prerequisites(self):
        fake_plan = [object()]
        with (
            patch.object(workloads, "build_paper_plan", return_value=fake_plan) as plan,
            patch.object(artifact, "check_prerequisites", side_effect=AssertionError("checked build")),
            patch.object(artifact, "execute_plan", side_effect=AssertionError("compiled workloads")),
            patch.object(artifact.subprocess, "run", side_effect=AssertionError("unexpected subprocess")),
            patch.object(artifact, "extract_results") as extract,
            redirect_stdout(StringIO()),
        ):
            # The main summary reads only each step's stage before extraction.
            fake_plan[0] = SimpleNamespace(stage="simulate")
            result = artifact.main(self.argv + ["--extract-only", "--log-dir", str(self.root / "old-logs")])
        self.assertEqual(result, 0)
        plan.assert_called_once()
        extract.assert_called_once()
        self.assertIs(extract.call_args.args[0], fake_plan)
        self.assertEqual(extract.call_args.args[1].log_dir, self.root / "old-logs")
        self.assertFalse(any(self.root.iterdir()))


if __name__ == "__main__":
    unittest.main()
