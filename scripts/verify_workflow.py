#!/usr/bin/env python3
"""Verify GitHub Actions workflow YAML syntax and structure."""

import yaml
import sys
import os


def check_yaml(path):
    try:
        with open(path) as f:
            data = yaml.safe_load(f)
        return True, data
    except Exception as e:
        return False, str(e)


def main():
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    errors = 0

    # Check main workflow
    wf_path = os.path.join(base, ".github", "workflows", "build.yml")
    ok, result = check_yaml(wf_path)
    if ok:
        print(f"[OK] {wf_path}")
        print(f"     Name: {result.get('name', 'N/A')}")
        jobs = list(result.get("jobs", {}).keys())
        print(f"     Jobs: {jobs}")

        on = result.get("on", {})
        triggers = []
        if "push" in on:
            triggers.append("push")
        if "pull_request" in on:
            triggers.append("pull_request")
        if "workflow_dispatch" in on:
            triggers.append("workflow_dispatch")
        print(f"     Triggers: {triggers}")

        for job_name, job in result.get("jobs", {}).items():
            if "needs" in job:
                print(f"     {job_name}: needs={job['needs']}")
            if "strategy" in job:
                matrix = job["strategy"].get("matrix", {})
                print(f"     {job_name}: matrix={matrix}")
    else:
        print(f"[FAIL] {wf_path}: {result}")
        errors += 1

    # Check composite action
    act_path = os.path.join(base, ".github", "actions", "qemu-boot-test", "action.yml")
    ok, result = check_yaml(act_path)
    if ok:
        print(f"\n[OK] {act_path}")
        print(f"     Name: {result.get('name', 'N/A')}")
        print(f"     Inputs: {list(result.get('inputs', {}).keys())}")
        print(f"     Outputs: {list(result.get('outputs', {}).keys())}")
    else:
        print(f"\n[FAIL] {act_path}: {result}")
        errors += 1

    if errors == 0:
        print("\nAll workflow files validated successfully.")
    else:
        print(f"\n{errors} file(s) failed validation.")
        sys.exit(1)


if __name__ == "__main__":
    main()