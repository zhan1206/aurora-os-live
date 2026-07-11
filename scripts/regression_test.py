#!/usr/bin/env python3
"""regression_test.py - AuroraOS automated regression test framework.

Runs system call tests, filesystem tests, and network tests by booting
the OS in QEMU with a test script and checking serial output.

Usage:
    python3 scripts/regression_test.py [--verbose] [--timeout SECONDS]
    python3 scripts/regression_test.py --report-only

Exit codes:
    0 - all tests passed
    1 - one or more tests failed
    2 - test setup error
"""

import os
import sys
import time
import json
import signal
import argparse
import subprocess
from datetime import datetime

# --- Configuration ---
ISO_PATH = os.environ.get("ISO_PATH", "os.iso")
QEMU_BIN = os.environ.get("QEMU", "qemu-system-x86_64")
QEMU_MEM = os.environ.get("QEMU_MEM", "256M")
LOG_FILE = "/tmp/aurora_regression_test.log"
REPORT_FILE = "test_report.json"

# --- Test registry ---
pass_count = 0
fail_count = 0
skip_count = 0
test_results = []


class TestResult:
    def __init__(self, name, suite, status, message=""):
        self.name = name
        self.suite = suite
        self.status = status  # "pass", "fail", "skip"
        self.message = message
        self.timestamp = datetime.utcnow().isoformat() + "Z"


def pass_test(name, suite, msg=""):
    global pass_count
    pass_count += 1
    test_results.append(TestResult(name, suite, "pass", msg))
    print(f"  [PASS] {name} {msg}")


def fail_test(name, suite, msg=""):
    global fail_count
    fail_count += 1
    test_results.append(TestResult(name, suite, "fail", msg))
    print(f"  [FAIL] {name} {msg}")


def skip_test(name, suite, msg=""):
    global skip_count
    skip_count += 1
    test_results.append(TestResult(name, suite, "skip", msg))
    print(f"  [SKIP] {name} {msg}")


def check_output(desc, pattern, suite, content):
    if pattern in content:
        pass_test(desc, suite)
    else:
        fail_test(desc, suite, f"pattern '{pattern}' not found")


def check_output_not(desc, pattern, suite, content):
    if pattern not in content:
        pass_test(desc, suite)
    else:
        fail_test(desc, suite, f"pattern '{pattern}' found unexpectedly")


# --- QEMU management ---
def start_qemu(timeout=30):
    """Start QEMU and wait for boot to complete."""
    if not os.path.exists(ISO_PATH):
        print(f"ERROR: ISO not found at {ISO_PATH}")
        sys.exit(2)

    if os.path.exists(LOG_FILE):
        os.remove(LOG_FILE)

    qemu = subprocess.Popen(
        [
            QEMU_BIN, "-m", QEMU_MEM, "-cdrom", ISO_PATH,
            "-nographic", "-no-reboot",
            "-serial", f"file:{LOG_FILE}",
            "-display", "none",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    print(f"QEMU started (PID={qemu.pid})")

    # Wait for boot
    elapsed = 0
    while elapsed < timeout:
        if os.path.exists(LOG_FILE):
            with open(LOG_FILE, "r") as f:
                content = f.read()
            if "All Tests Passed" in content:
                print("Boot complete (self-tests passed).")
                return qemu, content
            if "panic" in content:
                print("WARNING: Kernel panic detected.")
                return qemu, content
        time.sleep(1)
        elapsed += 1

    print(f"WARNING: Boot timeout after {timeout}s.")
    content = ""
    if os.path.exists(LOG_FILE):
        with open(LOG_FILE, "r") as f:
            content = f.read()
    return qemu, content


def stop_qemu(qemu):
    """Stop QEMU gracefully."""
    if qemu.poll() is None:
        qemu.send_signal(signal.SIGTERM)
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()
            qemu.wait()


def read_log():
    """Read the current log file content."""
    if os.path.exists(LOG_FILE):
        with open(LOG_FILE, "r") as f:
            return f.read()
    return ""


# --- Test suites ---
def run_syscall_tests(content):
    """Test system call functionality."""
    print("\n--- System Call Tests ---")
    suite = "syscall"

    # Check for syscall subsystem initialization
    check_output("syscall init", "SYSCALL/SYSRET", suite, content)

    # Check for process creation
    check_output("process creation", "create_task", suite, content)

    # Check for file operations
    check_output("VFS lookup", "vfs_lookup", suite, content)

    # Check for pipe operations
    check_output("pipe support", "pipe", suite, content)

    # Check for signal support
    check_output("signal support", "signal", suite, content)

    # Check for sysfs
    check_output("sysfs mounted", "sysfs", suite, content)

    # Check for procfs
    check_output("procfs available", "proc", suite, content)


def run_filesystem_tests(content):
    """Test filesystem operations."""
    print("\n--- Filesystem Tests ---")
    suite = "filesystem"

    # Check for VFS init
    check_output("VFS init", "VFS", suite, content)

    # Check for RamFS
    check_output("RamFS mounted", "RamFS", suite, content)

    # Check for file I/O
    check_output("file read/write", "vfs_write", suite, content)

    # Check for dentry cache
    check_output("dentry cache", "dentry", suite, content)

    # Check for inode operations
    check_output("inode operations", "inode", suite, content)

    # Check for FAT32 support
    if "FAT32" in content:
        pass_test("FAT32 support", suite)
    else:
        skip_test("FAT32 support", suite, "FAT32 not mounted")


def run_network_tests(content):
    """Test network stack functionality."""
    print("\n--- Network Tests ---")
    suite = "network"

    # Check for network stack init
    check_output("network stack init", "TCP/IP", suite, content)

    # Check for DHCP client
    if "dhcp" in content.lower():
        pass_test("DHCP client", suite)
    else:
        skip_test("DHCP client", suite, "DHCP not initialized")

    # Check for DNS resolver
    if "dns" in content.lower():
        pass_test("DNS resolver", suite)
    else:
        skip_test("DNS resolver", suite, "DNS not available")

    # Check for HTTP client
    if "http" in content.lower():
        pass_test("HTTP client", suite)
    else:
        skip_test("HTTP client", suite, "HTTP not available")

    # Check for ARP
    check_output("ARP support", "ARP", suite, content)

    # Check for ICMP
    if "ICMP" in content or "icmp" in content.lower():
        pass_test("ICMP support", suite)
    else:
        skip_test("ICMP support", suite, "ICMP not in log")

    # Check for TCP
    if "TCP" in content:
        pass_test("TCP support", suite)
    else:
        skip_test("TCP support", suite, "TCP not in log")


def run_memory_tests(content):
    """Test memory management."""
    print("\n--- Memory Tests ---")
    suite = "memory"

    check_output("buddy allocator", "Buddy", suite, content)
    check_output("slab allocator", "Slab", suite, content)
    check_output("page tables", "Page Table", suite, content)
    check_output("physical memory", "Physical memory", suite, content)
    check_output("ASLR", "ASLR", suite, content)


def run_scheduler_tests(content):
    """Test scheduler functionality."""
    print("\n--- Scheduler Tests ---")
    suite = "scheduler"

    check_output("scheduler init", "Scheduler", suite, content)
    check_output("round-robin", "RR", suite, content)
    check_output("task creation", "create_task", suite, content)

    if "SMP" in content:
        pass_test("SMP support", suite)
    else:
        skip_test("SMP support", suite, "SMP not detected")


def generate_report():
    """Generate a JSON test report."""
    report = {
        "title": "AuroraOS Regression Test Report",
        "timestamp": datetime.utcnow().isoformat() + "Z",
        "summary": {
            "total": pass_count + fail_count + skip_count,
            "passed": pass_count,
            "failed": fail_count,
            "skipped": skip_count,
        },
        "results": [
            {
                "name": r.name,
                "suite": r.suite,
                "status": r.status,
                "message": r.message,
                "timestamp": r.timestamp,
            }
            for r in test_results
        ],
    }

    with open(REPORT_FILE, "w") as f:
        json.dump(report, f, indent=2)
    print(f"\nReport saved to {REPORT_FILE}")


# --- Main ---
def main():
    parser = argparse.ArgumentParser(description="AuroraOS Regression Tests")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Verbose output")
    parser.add_argument("--timeout", "-t", type=int, default=30,
                        help="Boot timeout in seconds (default: 30)")
    parser.add_argument("--report-only", action="store_true",
                        help="Only generate report from existing log")
    args = parser.parse_args()

    print("=== AuroraOS Regression Tests ===")
    print(f"Time: {datetime.utcnow().isoformat()}Z")
    print("")

    if args.report_only:
        if os.path.exists(LOG_FILE):
            with open(LOG_FILE, "r") as f:
                content = f.read()
        else:
            print(f"ERROR: Log file not found: {LOG_FILE}")
            sys.exit(2)
    else:
        qemu, content = start_qemu(timeout=args.timeout)

    # Run all test suites
    run_syscall_tests(content)
    run_filesystem_tests(content)
    run_network_tests(content)
    run_memory_tests(content)
    run_scheduler_tests(content)

    if not args.report_only:
        stop_qemu(qemu)

    # Generate report
    generate_report()

    # Summary
    total = pass_count + fail_count + skip_count
    print(f"\n=== Regression Test Summary ===")
    print(f"  Total:   {total}")
    print(f"  Passed:  {pass_count}")
    print(f"  Failed:  {fail_count}")
    print(f"  Skipped: {skip_count}")

    if fail_count > 0:
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()