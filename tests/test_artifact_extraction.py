"""Regression checks for incomplete data changing the paper's averages."""

import csv
import json
from pathlib import Path
import shutil
import runpy
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import run_artifact as artifact


class ExtractedDataTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="artifact-csv-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.data = self.root / "data"
        self.data.mkdir()
        self.label = "bose_hubbard_q"
        benchmark = {"benchmark": self.label}
        self.write("main_results.csv", [{**benchmark, **dict.fromkeys((
            "baseline_ipc", "ideal_ipc", "cache_lru_ipc", "cache_rri_ipc", "prefetch_lru_ipc",
        ), 1.0)}])
        self.write("cache_prefetch_policies.csv", [{**benchmark, "lru_miss_rate": 20, "rri_miss_rate": 10}])
        self.write("prefetch_percentage.csv", [{**benchmark, "prefetch_percentage": 30}])
        readiness = dict(benchmark)
        for prefix in ("baseline", "cache", "prefetch"):
            readiness[f"{prefix}_cycles_stalled_non_clifford_readiness_percent"] = 0
        for prefix in ("cache", "prefetch"):
            for suffix, value in (
                ("loads_2d_cold_reached_non_clifford_ready", 2),
                ("loads_1d_waited_for_verification", 3),
                ("loads_1d_already_verified", 5),
                ("avg_delay_2d_cold_non_clifford_ready_cycles", 194),
                ("avg_delay_1d_non_clifford_ready_cycles", 100),
            ):
                readiness[f"{prefix}_{suffix}"] = value
        for name in ("readiness_latency.csv", "verification_stalls_impact.csv"):
            self.write(name, [readiness])
        fidelity = dict(benchmark)
        for prefix in ("firstpass", "cache", "prefetch"):
            fidelity.update({
                f"{prefix}_total_simulation_cycles": 1000,
                f"{prefix}_unrolled_instructions_done": 100,
                f"{prefix}_cold_storage_error_rate_sum": 1e-15,
            })
            if prefix != "firstpass":
                fidelity[f"{prefix}_storage_1d_error_rate"] = 1e-16
        self.write("logical_qubit_round_error_rates_csm194.csv", [fidelity])
        self.write("sensitivity_compute.csv", [
            {"compute_capacity": capacity, "policy": policy, self.label: 1.0}
            for capacity in (4, 8, 12, 16)
            for policy in ("baseline", "ideal_memory", "cache_rri", "prefetch_lru")
        ])
        self.write("sensitivity_buffer_capacity.csv", [
            {"intermediate_capacity_i": capacity, "policy": policy, self.label: 1.0}
            for capacity, policies in ((0, ("baseline", "ideal")), *((i, ("cache_rri", "prefetch_lru")) for i in (4, 8, 16, 24)))
            for policy in policies
        ])
        self.write("sensitivity_block_size.csv", [
            {**benchmark, "simulation_mode": mode, "csm": capacity, "ipc": 1.0}
            for mode, capacities in (("firstpass", (194,)), ("cache", (34, 98, 194, 322, 482)), ("prefetch", (34, 98, 194, 322, 482)))
            for capacity in capacities
        ])

    def write(self, name, rows):
        with (self.data / name).open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)

    def change(self, name, column, value):
        rows = artifact.read_csv(self.data / name)
        rows[0][column] = value
        self.write(name, rows)

    def validate(self, short=False):
        artifact.validate_csvs(self.data, [self.label], short_run=short)

    def test_complete_dataset_does_not_require_unplotted_metrics(self):
        # No prefetch-RRI IPC, baseline-1D rate, or compiler-policy columns exist.
        self.validate()

    def test_missing_consumed_metrics_cannot_silently_change_full_run_averages(self):
        cases = (
            ("cache_prefetch_policies.csv", "rri_miss_rate"),
            ("prefetch_percentage.csv", "prefetch_percentage"),
            ("readiness_latency.csv", "cache_avg_delay_2d_cold_non_clifford_ready_cycles"),
            ("verification_stalls_impact.csv", "prefetch_loads_1d_already_verified"),
            ("logical_qubit_round_error_rates_csm194.csv", "prefetch_storage_1d_error_rate"),
            ("sensitivity_block_size.csv", "ipc"),
        )
        for name, column in cases:
            with self.subTest(file=name, column=column):
                original = (self.data / name).read_bytes()
                self.change(name, column, "")
                with self.assertRaisesRegex(RuntimeError, column):
                    self.validate()
                (self.data / name).write_bytes(original)

    def test_short_runs_allow_unmeasured_rates_but_full_runs_reject_them(self):
        self.change("cache_prefetch_policies.csv", "rri_miss_rate", "")
        self.change("logical_qubit_round_error_rates_csm194.csv", "prefetch_storage_1d_error_rate", "")
        self.change("readiness_latency.csv", "prefetch_avg_delay_1d_non_clifford_ready_cycles", "")
        self.validate(short=True)
        with self.assertRaises(RuntimeError):
            self.validate()

    def test_nonfinite_negative_and_malformed_present_values_fail_even_in_short_runs(self):
        for value in ("nan", "inf", "-1", "not-a-number"):
            with self.subTest(value=value):
                self.change("logical_qubit_round_error_rates_csm194.csv", "prefetch_storage_1d_error_rate", value)
                with self.assertRaisesRegex(RuntimeError, "prefetch_storage_1d_error_rate"):
                    self.validate(short=True)

    def test_unsampled_delay_is_optional_when_other_readiness_samples_exist(self):
        for name in ("readiness_latency.csv", "verification_stalls_impact.csv"):
            self.change(name, "cache_loads_1d_waited_for_verification", 0)
            self.change(name, "cache_avg_delay_1d_non_clifford_ready_cycles", "")
        self.validate()

    def test_dropped_or_duplicated_sensitivity_rows_are_rejected(self):
        for name in ("sensitivity_compute.csv", "sensitivity_buffer_capacity.csv", "sensitivity_block_size.csv"):
            original = (self.data / name).read_bytes()
            rows = artifact.read_csv(self.data / name)
            for altered in (rows[:-1], rows + rows[:1]):
                with self.subTest(file=name, count=len(altered)):
                    self.write(name, altered)
                    with self.assertRaisesRegex(RuntimeError, "configurations"):
                        self.validate()
            (self.data / name).write_bytes(original)

    def test_extraction_uses_recorded_sample_limit_before_requested_limit(self):
        for recorded, requested, expected_short in ((100, None, True), (None, 100, False)):
            with self.subTest(recorded=recorded, requested=requested):
                run_dir = self.root / f"run-{recorded}-{requested}"
                run_dir.mkdir()
                (run_dir / "artifact_manifest.json").write_text(json.dumps({
                    "status": "complete", "instruction_limit": recorded,
                }))
                args = SimpleNamespace(
                    log_dir=run_dir / "logs", results_dir=run_dir / "results",
                    benchmarks=[SimpleNamespace(label=self.label)], instructions=requested,
                )

                def populate_staging(command, **kwargs):
                    staging = Path(command[command.index("--output-dir") + 1])
                    shutil.copytree(self.data, staging, dirs_exist_ok=True)

                with (
                    patch.object(artifact, "validate_logs"),
                    patch.object(artifact.subprocess, "run", side_effect=populate_staging),
                    patch.object(artifact, "validate_csvs", wraps=artifact.validate_csvs) as validate,
                ):
                    artifact.extract_results([], args)
                self.assertEqual(validate.call_args.kwargs["short_run"], expected_short)


class LogicalErrorParsingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with patch.object(sys, "path", [str(artifact.EXTRACTOR_DIR), *sys.path]):
            cls.extractor = runpy.run_path(str(
                artifact.EXTRACTOR_DIR / "extract_logical_qubit_round_error_rates_csm194.py"
            ))

    def parse(self, text):
        with tempfile.TemporaryDirectory(prefix="artifact-error-log-") as directory:
            log_path = Path(directory) / "simulation.log"
            log_path.write_text(text)
            return self.extractor["component_values"](self.extractor["parse_log"](log_path))

    def test_complete_sections_sum_all_cold_rates_including_measured_zero(self):
        result = self.parse(
            "YOKED_1D_STORAGE Error Stats:\nPer logical-qubit round error rate: 3e-17\n"
            "YOKED_COLD_STORAGE Error Stats:\nPer logical-qubit round error rate: 0\n"
            "YOKED_COLD_STORAGE Error Stats:\nPer logical-qubit round error rate: 2e-16\n"
        )
        self.assertEqual(result["storage_1d_error_rate"], 3e-17)
        self.assertEqual(result["cold_storage_error_rate_sum"], 2e-16)
        self.assertEqual(result["scaled_194x_cold_storage_error_sum"], 194 * 2e-16)

    def test_one_missing_cold_rate_cannot_be_hidden_by_other_sections(self):
        complete = "YOKED_COLD_STORAGE Error Stats:\nPer logical-qubit round error rate: 2e-16\n"
        for missing in (
            "YOKED_COLD_STORAGE Error Stats:\n",
            "YOKED_COLD_STORAGE Error Stats:\nPer logical-qubit round error rate: nan\n",
        ):
            for text in (missing + complete, complete + missing):
                with self.subTest(text=text):
                    result = self.parse(text)
                    self.assertIsNone(result["cold_storage_error_rate_sum"])
                    self.assertIsNone(result["scaled_194x_cold_storage_error_sum"])

    def test_no_cold_storage_preserves_zero_contribution(self):
        result = self.parse("TOTAL_SIMULATION_CYCLES 100\nUNROLLED_INSTRUCTIONS_DONE 100\n")
        self.assertEqual(result["cold_storage_error_rate_sum"], 0)
        self.assertEqual(result["scaled_194x_cold_storage_error_sum"], 0)


if __name__ == "__main__":
    unittest.main()
