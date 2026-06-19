#!/usr/bin/env python3
"""
Generate a JUnit-compatible XML test report from QEMU serial output.

Parses the kernel self-test [PASS] / [FAIL] lines and produces a
JUnit XML file compatible with GitHub Actions test reporting.

Usage:
    python3 generate_junit_report.py <qemu_output.log> [output.xml]

The output can be uploaded with actions/upload-artifact and displayed
in the GitHub Actions UI test summary.
"""

import sys
import re
import xml.etree.ElementTree as ET
from datetime import datetime, timezone


def parse_test_results(log_path: str) -> list[tuple[str, bool, str]]:
    """Parse QEMU serial output and extract test results.

    Returns list of (test_name, passed, category) tuples.
    """
    results = []
    current_category = "Unknown"

    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            # Track test category from section headers
            cat_match = re.match(r"--- (.+?) Tests ---", line)
            if cat_match:
                current_category = cat_match.group(1).strip()
                continue

            # Parse [PASS] / [FAIL] lines
            pass_match = re.match(r"\s*\[PASS\]\s+(.+)", line)
            if pass_match:
                results.append((pass_match.group(1).strip(), True, current_category))
                continue

            fail_match = re.match(r"\s*\[FAIL\]\s+(.+)", line)
            if fail_match:
                results.append((fail_match.group(1).strip(), False, current_category))

    return results


def build_junit_xml(results: list[tuple[str, bool, str]]) -> str:
    """Build a JUnit XML string from test results."""
    total = len(results)
    failures = sum(1 for _, passed, _ in results if not passed)
    skipped = 0
    errors = 0

    timestamp = datetime.now(timezone.utc).isoformat(timespec="seconds")

    testsuite = ET.Element(
        "testsuite",
        {
            "name": "AuroraOS Kernel Self-Tests",
            "tests": str(total),
            "failures": str(failures),
            "errors": str(errors),
            "skipped": str(skipped),
            "time": "0.0",
            "timestamp": timestamp,
            "hostname": "qemu-guest",
        },
    )

    for test_name, passed, category in results:
        testcase = ET.SubElement(
            testsuite,
            "testcase",
            {
                "classname": f"kernel.{category.lower().replace(' ', '_')}",
                "name": test_name,
                "time": "0.0",
            },
        )

        if not passed:
            ET.SubElement(
                testcase,
                "failure",
                {
                    "message": f"Self-test failed: {test_name}",
                    "type": "AssertionError",
                },
            ).text = f"Test '{test_name}' in category '{category}' did not pass."

    return ET.tostring(testsuite, encoding="unicode", xml_declaration=True)


def main():
    if len(sys.argv) < 2:
        print("Usage: generate_junit_report.py <qemu_output.log> [output.xml]",
              file=sys.stderr)
        sys.exit(1)

    log_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else "test-results.xml"

    results = parse_test_results(log_path)

    if not results:
        print("Warning: No test results found in log file", file=sys.stderr)
        # Write an empty testsuite to avoid breaking the upload step
        xml = build_junit_xml([])
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(xml)
        print(f"Wrote empty report to {output_path}")
        return

    xml = build_junit_xml(results)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(xml)

    passes = sum(1 for _, p, _ in results if p)
    fails = sum(1 for _, p, _ in results if not p)
    print(f"JUnit report written to {output_path}")
    print(f"  Tests: {len(results)} | Passed: {passes} | Failed: {fails}")


if __name__ == "__main__":
    main()