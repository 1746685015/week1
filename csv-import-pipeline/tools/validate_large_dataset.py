"""Stream-validate the assignment's large CSV dataset without loading it into memory."""

from __future__ import annotations

import argparse
import hashlib
import json
import time
from pathlib import Path


EXPECTED_SIZE = 1_003_898_922
EXPECTED_HEADER = b"id,name,age,score,active"
EXPECTED_DATA_ROWS = 10_000_000
EXPECTED_INVALID_ROWS = 10_000
MAX_MISMATCH_SAMPLES = 20
EXPECTED_NAME_PADDING = b"x" * 66


def is_one_decimal(value: bytes) -> bool:
	integer, separator, fraction = value.partition(b".")
	return separator == b"." and integer.isdigit() and len(fraction) == 1 and fraction.isdigit()


def validate_dataset(path: Path) -> tuple[dict[str, object], bool]:
	started = time.perf_counter()
	digest = hashlib.sha256()
	data_rows = 0
	invalid_rows = 0
	mismatch_count = 0
	mismatch_samples: list[str] = []
	invalid_samples: list[dict[str, object]] = []
	last_line_ended_with_newline = False
	header = b""

	def record_mismatch(message: str) -> None:
		nonlocal mismatch_count
		mismatch_count += 1
		if len(mismatch_samples) < MAX_MISMATCH_SAMPLES:
			mismatch_samples.append(message)

	with path.open("rb", buffering=1024 * 1024) as source:
		for physical_line, raw_line in enumerate(source, start=1):
			digest.update(raw_line)
			last_line_ended_with_newline = raw_line.endswith(b"\n")
			content = raw_line[:-1] if last_line_ended_with_newline else raw_line
			if content.endswith(b"\r"):
				content = content[:-1]

			if physical_line == 1:
				header = content
				continue

			data_rows += 1
			fields = content.split(b",")
			if len(fields) != 5:
				record_mismatch(f"physical line {physical_line}: expected 5 columns, got {len(fields)}")
				continue

			id_text, name, age, score, active = fields
			try:
				row_id = int(id_text)
			except ValueError:
				record_mismatch(f"physical line {physical_line}: invalid id {id_text!r}")
				continue

			if row_id != data_rows:
				record_mismatch(
					f"physical line {physical_line}: expected sequential id {data_rows}, got {row_id}"
				)

			expected_name_prefix = b"User" + f"{row_id:08d}".encode("ascii")
			if len(name) != 78 or not name.startswith(expected_name_prefix) \
				or not name.endswith(EXPECTED_NAME_PADDING):
				record_mismatch(f"physical line {physical_line}: name does not match the assignment format")

			expected_invalid = row_id % 1000 == 0
			actual_invalid = age == b"N/A"
			if actual_invalid:
				invalid_rows += 1
				if invalid_rows in (1, 5000, EXPECTED_INVALID_ROWS):
					invalid_samples.append(
						{
							"physical_line": physical_line,
							"id": row_id,
							"raw_line": content.decode("ascii"),
						}
					)
			if expected_invalid != actual_invalid:
				record_mismatch(
					f"physical line {physical_line}: id={row_id}, age={age!r}, "
					f"expected_invalid={expected_invalid}"
				)
			if not actual_invalid and not age.isdigit():
				record_mismatch(f"physical line {physical_line}: invalid integer age {age!r}")
			if not is_one_decimal(score):
				record_mismatch(f"physical line {physical_line}: invalid score {score!r}")
			if active not in (b"true", b"false"):
				record_mismatch(f"physical line {physical_line}: invalid active value {active!r}")

	file_size = path.stat().st_size
	elapsed_seconds = time.perf_counter() - started
	result: dict[str, object] = {
		"path": str(path.resolve()),
		"size_bytes": file_size,
		"sha256": digest.hexdigest().upper(),
		"header": header.decode("ascii", errors="replace"),
		"data_rows": data_rows,
		"invalid_rows": invalid_rows,
		"last_line_ended_with_newline": last_line_ended_with_newline,
		"mismatch_count": mismatch_count,
		"mismatch_samples": mismatch_samples,
		"invalid_samples": invalid_samples,
		"validation_elapsed_seconds": round(elapsed_seconds, 3),
	}
	passed = (
		file_size == EXPECTED_SIZE
		and header == EXPECTED_HEADER
		and data_rows == EXPECTED_DATA_ROWS
		and invalid_rows == EXPECTED_INVALID_ROWS
		and last_line_ended_with_newline
		and mismatch_count == 0
	)
	return result, passed


def main() -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("path", type=Path, help="Path to sample_1gb.csv")
	arguments = parser.parse_args()
	if not arguments.path.is_file():
		parser.error(f"file does not exist: {arguments.path}")

	result, passed = validate_dataset(arguments.path)
	result["passed"] = passed
	print(json.dumps(result, ensure_ascii=False, indent=2))
	return 0 if passed else 1


if __name__ == "__main__":
	raise SystemExit(main())
