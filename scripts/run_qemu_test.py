#!/usr/bin/env python3
"""Run AuroraOS in QEMU and capture serial output to verify self-tests."""
import subprocess, time, os, sys

log_path = '/tmp/qemu_serial.log'
if os.path.exists(log_path):
    os.remove(log_path)

p = subprocess.Popen(
    ['qemu-system-x86_64', '-m', '256M', '-cdrom', 'os.iso',
     '-nographic', '-no-reboot',
     '-serial', 'file:' + log_path, '-display', 'none'],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
)

time.sleep(10)
p.kill()
p.wait()

if os.path.exists(log_path):
    with open(log_path) as f:
        content = f.read()
    print(content[:5000])
    # Check for PASS/FAIL
    pass_count = content.count('PASS')
    fail_count = content.count('FAIL')
    print(f'\n=== TEST SUMMARY: {pass_count} PASS, {fail_count} FAIL ===')
    sys.exit(0 if fail_count == 0 and pass_count > 0 else 1)
else:
    print('NO LOG FILE')
    sys.exit(2)