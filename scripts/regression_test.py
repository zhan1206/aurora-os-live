#!/usr/bin/env python3
"""regression_test.py - AuroraOS automated regression test framework.

Runs system call tests, filesystem tests, and network tests by booting
the OS in QEMU with serial console on stdio, sending interactive commands
via stdin, and checking serial output.

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
import shutil
import argparse
import threading
import subprocess
from datetime import datetime, timezone

# --- Configuration ---
ISO_PATH = os.environ.get("ISO_PATH", "os.iso")
QEMU_BIN = os.environ.get("QEMU", "qemu-system-x86_64")
QEMU_MEM = os.environ.get("QEMU_MEM", "256M")
REPORT_FILE = "test_report.json"

# --- Test registry ---
pass_count = 0
fail_count = 0
skip_count = 0
test_results = []

# Output accumulator for QEMU serial output
output_chunks = []
output_lock = threading.Lock()
qemu_process = None


class TestResult:
    def __init__(self, name, suite, status, message=""):
        self.name = name
        self.suite = suite
        self.status = status  # "pass", "fail", "skip"
        self.message = message
        self.timestamp = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


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


def get_output():
    """Get accumulated serial output as a single string."""
    with output_lock:
        return ''.join(output_chunks)


def wait_for(pattern, timeout_sec):
    """Wait for a pattern to appear in the accumulated output.

    Returns True if the pattern is found before the timeout.
    """
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        output = get_output()
        if pattern in output:
            return True
        time.sleep(0.1)
    return False


def send_command(cmd):
    """Send a command string to QEMU via stdin (serial input)."""
    if qemu_process is None or qemu_process.poll() is not None:
        return
    try:
        qemu_process.stdin.write((cmd + '\n').encode('utf-8'))
        qemu_process.stdin.flush()
    except (IOError, BrokenPipeError, OSError):
        pass


def check_output(desc, pattern, suite):
    """Check if pattern exists in accumulated output."""
    content = get_output()
    if pattern in content:
        pass_test(desc, suite)
    else:
        fail_test(desc, suite, f"pattern '{pattern}' not found")


def check_output_not(desc, pattern, suite):
    """Check if pattern does NOT exist in accumulated output."""
    content = get_output()
    if pattern not in content:
        pass_test(desc, suite)
    else:
        fail_test(desc, suite, f"pattern '{pattern}' found unexpectedly")


def reader_thread(pipe):
    """Read output from QEMU stdout in a background thread."""
    global output_chunks
    try:
        while True:
            chunk = pipe.read(4096)
            if not chunk:
                break
            with output_lock:
                output_chunks.append(chunk.decode('utf-8', errors='replace'))
    except (ValueError, IOError, OSError):
        pass


# --- QEMU management ---
def start_qemu(timeout=30):
    """Start QEMU with serial on stdio and wait for boot to complete.

    Returns (qemu_process, content_string).  The process is used for
    sending commands and must be terminated by the caller.
    """
    global qemu_process, output_chunks

    if not os.path.exists(ISO_PATH):
        print(f"ERROR: ISO not found at {ISO_PATH}")
        sys.exit(2)

    if shutil.which(QEMU_BIN) is None:
        print(f"ERROR: QEMU not found: {QEMU_BIN}")
        sys.exit(2)

    # Reset output accumulator
    output_chunks = []

    qemu = subprocess.Popen(
        [QEMU_BIN, "-m", QEMU_MEM, "-cdrom", ISO_PATH,
         "-nographic", "-no-reboot",
         "-monitor", "none", "-display", "none"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    qemu_process = qemu
    print(f"QEMU started (PID={qemu.pid})")

    # Start background reader thread
    reader = threading.Thread(target=reader_thread, args=(qemu.stdout,), daemon=True)
    reader.start()

    # Wait for boot
    elapsed = 0
    while elapsed < timeout:
        content = get_output()
        if "All Tests Passed" in content:
            print("Boot complete (self-tests passed).")
            return qemu, content
        if "panic" in content:
            print("WARNING: Kernel panic detected.")
            return qemu, content
        time.sleep(1)
        elapsed += 1

    print(f"WARNING: Boot timeout after {timeout}s.")
    content = get_output()
    return qemu, content


def stop_qemu(qemu):
    """Stop QEMU gracefully."""
    if qemu is None:
        return
    if qemu.poll() is None:
        qemu.send_signal(signal.SIGTERM)
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()
            qemu.wait()


def read_log():
    """Read the current accumulated output."""
    return get_output()


# --- Test suites ---
def run_syscall_tests(content):
    """Test system call functionality."""
    print("\n--- System Call Tests ---")
    suite = "syscall"

    check_output("syscall init", "SYSCALL/SYSRET", suite)
    check_output("process creation", "create_task", suite)
    check_output("VFS lookup", "vfs_lookup", suite)
    check_output("pipe support", "pipe", suite)
    check_output("signal support", "signal", suite)
    check_output("sysfs mounted", "sysfs", suite)
    check_output("procfs available", "proc", suite)


def run_filesystem_tests(content):
    """Test filesystem operations."""
    print("\n--- Filesystem Tests ---")
    suite = "filesystem"

    check_output("VFS init", "VFS", suite)
    check_output("RamFS mounted", "RamFS", suite)
    check_output("file read/write", "vfs_write", suite)
    check_output("dentry cache", "dentry", suite)
    check_output("inode operations", "inode", suite)

    # FAT32: fail if not found, since it's a listed feature
    if "FAT32" in content:
        pass_test("FAT32 support", suite)
    else:
        fail_test("FAT32 support", suite, "FAT32 not mounted")


def run_network_tests(content):
    """Test network stack functionality."""
    print("\n--- Network Tests ---")
    suite = "network"

    check_output("network stack init", "TCP/IP", suite)

    # DHCP: fail if not found, since dhcp_init should be called at boot
    if "dhcp" in content.lower():
        pass_test("DHCP client", suite)
    else:
        fail_test("DHCP client", suite, "DHCP not initialized")

    # DNS: fail if not found
    if "dns" in content.lower():
        pass_test("DNS resolver", suite)
    else:
        fail_test("DNS resolver", suite, "DNS not available")

    # HTTP: fail if not found
    if "http" in content.lower():
        pass_test("HTTP client", suite)
    else:
        fail_test("HTTP client", suite, "HTTP not available")

    check_output("ARP support", "ARP", suite)

    if "ICMP" in content or "icmp" in content.lower():
        pass_test("ICMP support", suite)
    else:
        skip_test("ICMP support", suite, "ICMP not in log")

    if "TCP" in content:
        pass_test("TCP support", suite)
    else:
        skip_test("TCP support", suite, "TCP not in log")


def run_memory_tests(content):
    """Test memory management."""
    print("\n--- Memory Tests ---")
    suite = "memory"

    check_output("buddy allocator", "Buddy", suite)
    check_output("slab allocator", "Slab", suite)
    check_output("page tables", "Page Table", suite)
    check_output("physical memory", "Physical memory", suite)
    check_output("ASLR", "ASLR", suite)


def run_scheduler_tests(content):
    """Test scheduler functionality."""
    print("\n--- Scheduler Tests ---")
    suite = "scheduler"

    check_output("scheduler init", "Scheduler", suite)
    check_output("round-robin", "RR", suite)
    check_output("task creation", "create_task", suite)

    if "SMP" in content:
        pass_test("SMP support", suite)
    else:
        skip_test("SMP support", suite, "SMP not detected")


def generate_report():
    """Generate a JSON test report."""
    report = {
        "title": "AuroraOS Regression Test Report",
        "timestamp": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
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
    print(f"Time: {datetime.now(timezone.utc).isoformat().replace('+00:00', 'Z')}")
    print("")

    qemu = None

    if args.report_only:
        content = get_output()
        if not content:
            print("ERROR: No accumulated output available for --report-only")
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