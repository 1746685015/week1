"""Verify completeness and byte-preserving contents of a Part C JSONL error log."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


EXPECTED_ERROR_COUNT = 10_000


def verify(log_path: Path) -> tuple[dict[str, object], bool]:
	digest = hashlib.sha256()
	record_count = 0
	mismatches: list[str] = []
	samples: list[dict[str, object]] = []
	with log_path.open("rb") as source:
		for raw_record in source:
			digest.update(raw_record)
			record_count += 1
			try:
				record = json.loads(raw_record)
				raw_line = bytes.fromhex(record["raw_line_hex"])
				fields = raw_line.split(b",")
				row_id = int(fields[0])
				cell_errors = record["cell_errors"]
			except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
				mismatches.append(f"record {record_count}: invalid JSONL schema: {error}")
				continue

			expected_id = record_count * 1000
			if row_id != expected_id:
				mismatches.append(f"record {record_count}: expected id {expected_id}, got {row_id}")
			if record["line_number"] != row_id + 1:
				mismatches.append(f"record {record_count}: physical line mismatch")
			if len(fields) != 5 or fields[2] != b"N/A":
				mismatches.append(f"record {record_count}: raw line is not the expected bad row")
			if len(cell_errors) != 1:
				mismatches.append(f"record {record_count}: expected one cell error")
			else:
				cell_error = cell_errors[0]
				if (
					cell_error.get("column_index") != 2
					or cell_error.get("column_name") != "age"
					or bytes.fromhex(cell_error.get("raw_value_hex", "")) != b"N/A"
					or cell_error.get("code") != "TypeConversionError"
					or not cell_error.get("message")
				):
					mismatches.append(f"record {record_count}: cell error detail mismatch")

			if record_count in (1, 5000, EXPECTED_ERROR_COUNT):
				samples.append(
					{
						"record": record_count,
						"physical_line": record["line_number"],
						"id": row_id,
						"raw_line": raw_line.decode("ascii"),
					}
				)

	result: dict[str, object] = {
		"path": str(log_path.resolve()),
		"size_bytes": log_path.stat().st_size,
		"sha256": digest.hexdigest().upper(),
		"record_count": record_count,
		"mismatch_count": len(mismatches),
		"mismatch_samples": mismatches[:20],
		"samples": samples,
	}
	passed = record_count == EXPECTED_ERROR_COUNT and not mismatches
	return result, passed


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("log_path", type=Path, help="Path to the JSONL sidecar")
	arguments = parser.parse_args()
	if not arguments.log_path.is_file():
		parser.error(f"file does not exist: {arguments.log_path}")

	result, passed = verify(arguments.log_path)
	result["passed"] = passed
	print(json.dumps(result, ensure_ascii=False, indent=2))
	return 0 if passed else 1


if __name__ == "__main__":
	raise SystemExit(main())
