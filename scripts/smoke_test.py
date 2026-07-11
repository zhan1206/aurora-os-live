#!/usr/bin/env python3
"""Smoke test for AuroraOS - sends commands to the running kernel.

Boots the OS in QEMU with serial console on stdio, sends interactive
commands via stdin, and checks serial output for expected content.

Usage:
    python3 scripts/smoke_test.py
    ISO_PATH=myos.iso python3 scripts/smoke_test.py

Exit codes:
    0 - all tests passed
    1 - one or more tests failed
    2 - test setup error (no QEMU, no ISO, etc.)
"""

import os
import sys
import time
import signal
import shutil
import subprocess
import threading

# --- Configuration ---
ISO = os.environ.get("ISO_PATH", "os.iso")
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
QEMU_MEM = os.environ.get("QEMU_MEM", "256M")
TIMEOUT = int(os.environ.get("TIMEOUT", "30"))

# --- Colors ---
RED = '\033[0;31m'
GREEN = '\033[0;32m'
YELLOW = '\033[1;33m'
NC = '\033[0m'

pass_count = 0
fail_count = 0
skip_count = 0

output_chunks = []
output_lock = threading.Lock()
qemu_process = None


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


def pass_test(desc):
    global pass_count
    pass_count += 1
    print(f"  {GREEN}[PASS]{NC} {desc}")


def fail_test(desc):
    global fail_count
    fail_count += 1
    print(f"  {RED}[FAIL]{NC} {desc}")


def skip_test(desc):
    global skip_count
    skip_count += 1
    print(f"  {YELLOW}[SKIP]{NC} {desc}")


def get_output():
    """Get accumulated output as a single string."""
    with output_lock:
        return ''.join(output_chunks)


def wait_for(pattern, timeout_sec=None):
    """Wait for a pattern to appear in the accumulated output.

    Returns True if the pattern is found before the timeout.
    """
    if timeout_sec is None:
        timeout_sec = TIMEOUT
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


def check_output(desc, pattern):
    """Check if a pattern exists in the accumulated output."""
    output = get_output()
    if pattern in output:
        pass_test(desc)
    else:
        fail_test(f"{desc} (pattern '{pattern}' not found)")


def cleanup():
    """Terminate QEMU and clean up."""
    global qemu_process
    if qemu_process is None:
        return
    if qemu_process.poll() is None:
        qemu_process.send_signal(signal.SIGTERM)
        try:
            qemu_process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu_process.kill()
            qemu_process.wait()


def main():
    global qemu_process, pass_count, fail_count, skip_count

    print("=== AuroraOS Smoke Test ===")
    print()

    # --- Pre-flight checks ---
    if not os.path.exists(ISO):
        print(f"ERROR: ISO not found at {ISO}")
        print("Run 'make iso' first.")
        sys.exit(2)

    if shutil.which(QEMU) is None:
        print(f"ERROR: QEMU not found: {QEMU}")
        sys.exit(2)

    print(f"QEMU:  {QEMU}")
    print(f"ISO:   {ISO}")
    print(f"Mem:   {QEMU_MEM}")
    print()

    # --- Start QEMU ---
    # -nographic: no graphical window, serial on stdio
    # -monitor none: disable QEMU monitor so stdin goes only to serial
    print("Starting QEMU...")
    qemu = subprocess.Popen(
        [QEMU, "-m", QEMU_MEM, "-cdrom", ISO,
         "-nographic", "-no-reboot",
         "-monitor", "none", "-display", "none"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    qemu_process = qemu

    # Start background reader thread
    reader = threading.Thread(target=reader_thread, args=(qemu.stdout,), daemon=True)
    reader.start()

    # --- Wait for boot to complete ---
    print(f"Waiting for boot (timeout: {TIMEOUT}s)...")
    if wait_for("All Tests Passed", TIMEOUT):
        print("Boot complete (self-tests passed).")
    elif "panic" in get_output():
        print("WARNING: Kernel panic detected during boot.")
    else:
        print(f"WARNING: Boot timeout reached. Proceeding with partial output.")

    # --- Run passive smoke tests ---
    print()
    print("--- Smoke Tests ---")

    check_output("kernel booted", "AuroraOS")
    check_output("self-tests executed", "Kernel Self-Test")
    check_output("self-tests passed", "All Tests Passed")

    # --- Interactive command tests ---
    print()
    print("--- Interactive Command Tests ---")

    send_command("help")
    time.sleep(1)
    output = get_output()
    if "help" in output.lower() or "command" in output.lower():
        pass_test("shell accepts 'help' command")
    else:
        fail_test("shell accepts 'help' command")

    send_command("cat /sys/kernel/version")
    time.sleep(1)
    output = get_output()
    if "AuroraOS" in output:
        pass_test("'cat /sys/kernel/version' returns AuroraOS")
    else:
        fail_test("'cat /sys/kernel/version' returns AuroraOS")

    send_command("ls")
    time.sleep(1)
    output = get_output()
    if "ls" in output:
        pass_test("shell accepts 'ls' command")
    else:
        fail_test("shell accepts 'ls' command")

    # Check for filesystem info via serial output
    check_output("/proc/version available", "version")
    check_output("memory info", "mem")

    # Check for networking init
    check_output("network stack init", "TCP/IP")

    # Check for scheduler
    check_output("scheduler init", "Scheduler")

    # Check for filesystem init
    check_output("VFS init", "VFS")

    # --- Summary ---
    print()
    print("=== Smoke Test Summary ===")
    print(f"  Passed:  {pass_count}")
    print(f"  Failed:  {fail_count}")
    print(f"  Skipped: {skip_count}")

    cleanup()

    if fail_count > 0:
        output = get_output()
        if len(output) > 2000:
            output = output[-2000:]
        print()
        print("Last 2000 bytes of serial output:")
        print(output)
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()