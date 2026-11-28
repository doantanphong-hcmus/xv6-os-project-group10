#!/usr/bin/env python3
"""Run Project 2 tests on a private disk; validate vmprint against built ELFs."""
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent
PAGE = 4096
MAXVA = 1 << 38
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def expected_leaves(binary):
    elf = binary.read_bytes()
    require(elf[:6] == b"\x7fELF\x02\x01", f"not ELF64 little-endian: {binary}")
    phoff = struct.unpack_from("<Q", elf, 32)[0]
    entsize, count = struct.unpack_from("<HH", elf, 54)
    leaves = {}
    end = 0
    for i in range(count):
        kind, flags, _, va, _, _, size, _ = struct.unpack_from(
            "<IIQQQQQQ", elf, phoff + i * entsize)
        if kind != 1 or va + size <= end:
            continue
        # xv6 allocates each segment plus any preceding gap with R/U and ELF W/X.
        perm = 1 | 2 | 16 | (4 if flags & 2 else 0) | (8 if flags & 1 else 0)
        for address in range((end + PAGE - 1) // PAGE * PAGE, va + size, PAGE):
            leaves[address] = perm
        end = va + size
    guard = (end + PAGE - 1) // PAGE * PAGE
    stack = int(re.search(r"#define\s+USERSTACK\s+(\d+)",
                          (ROOT / "kernel/param.h").read_text())[1])
    leaves[guard] = 7
    for i in range(1, stack + 1):
        leaves[guard + i * PAGE] = 23
    leaves[MAXVA - 2 * PAGE] = 7
    leaves[MAXVA - PAGE] = 11
    return leaves


def validate_tree(output, binary, tail):
    lines = output.strip().splitlines()
    require(len(lines) >= 3, "missing page table or program output")
    root = re.fullmatch(r"page table (0x[0-9a-fA-F]{16})", lines[0])
    require(root is not None, f"bad page-table header: {lines[0]}")
    require(lines[-1] == tail, f"program did not complete: expected {tail}")
    rootpa = int(root[1], 16)
    require(rootpa % PAGE == 0 and 0x80000000 <= rootpa < 0x88000000,
            "root page table outside RAM or unaligned")
    table_pages = {rootpa}
    leaves = {}
    last_index = {}
    branches = []
    for line in lines[1:-1]:
        entry = re.fullmatch(
            r"((?: \.\.){1,3})(\d+): pte (0x[0-9a-fA-F]{16}) pa (0x[0-9a-fA-F]{16})", line)
        require(entry is not None, f"bad PTE format/depth: {line}")
        depth = len(entry[1]) // 3
        index, pte, pa = int(entry[2]), int(entry[3], 16), int(entry[4], 16)
        require(depth <= len(branches) + 1, f"missing parent: {line}")
        parent = tuple(branches[:depth - 1])
        require(0 <= index < 512 and index > last_index.get(parent, -1),
                f"duplicate, unordered or invalid index: {line}")
        last_index[parent] = index
        path = parent + (index,)
        require(pte & 1, f"invalid PTE printed: {line}")
        require(pa == (pte >> 10) << 12, f"PA != PTE2PA: {line}")
        require(0x80000000 <= pa < 0x88000000, f"PA outside RAM: {line}")
        if pte & 14:
            require(depth == 3, f"unexpected superpage: {line}")
            va = sum(n << (12 + 9 * (2 - i)) for i, n in enumerate(path))
            leaves[va] = pte & 31
            branches = list(parent)
        else:
            require(depth < 3 and pte & 31 == 1, f"invalid non-leaf: {line}")
            require(pa not in table_pages, f"page-table cycle/alias: {line}")
            table_pages.add(pa)
            branches = list(path)
    expected = expected_leaves(binary)
    missing = sorted(expected.keys() - leaves.keys())
    extra = sorted(leaves.keys() - expected.keys())
    wrong = [hex(va) for va in expected.keys() & leaves.keys()
             if expected[va] != leaves[va]]
    require(not missing and not extra and not wrong,
            f"leaf mismatch: missing={[hex(v) for v in missing[:3]]}, "
            f"extra={[hex(v) for v in extra[:3]]}, wrong permissions={wrong[:3]}")
    # Require exactly the intermediate tables needed by the expected mappings.
    parents = {(va >> 30,) for va in expected}
    parents |= {(va >> 30, (va >> 21) & 511) for va in expected}
    require(set(last_index) == {()} | parents and len(table_pages) == len(parents) + 1,
            "extra or missing intermediate table")
    return len(leaves), len(table_pages)


def check_validator(tree):
    lines = tree.strip().splitlines()
    leaf = next(i for i, line in enumerate(lines) if line.startswith(" .. .. .."))
    bad_pa = lines.copy()
    bad_pa[leaf] = bad_pa[leaf].split(" pa ")[0] + " pa 0x0000000000000000"
    bad_permissions = lines.copy()
    pte = re.search(r"pte (0x[0-9a-fA-F]+)", lines[leaf])[1]
    bad_permissions[leaf] = lines[leaf].replace(pte, f"0x{int(pte, 16) ^ 16:016x}", 1)
    for name, broken in [("missing tree", []), ("missing leaf", lines[:leaf] + lines[leaf + 1:]),
                         ("wrong PA", bad_pa), ("wrong permissions", bad_permissions)]:
        try:
            validate_tree("\n".join(broken), ROOT / "user/_echo", "execvm-exec")
        except AssertionError:
            continue
        raise AssertionError(f"checker accepted {name}")


def validate_project(output):
    output = ANSI.sub("", output).replace("\r", "")
    require("FAIL" not in output and "panic:" not in output, "xv6 assertion or panic")
    require("RESULT: assertions passed in 17/17 groups." in output, "incomplete run")
    groups = [f"{part}.{i}" for part, count in [(1, 11), (2, 6)]
              for i in range(1, count + 1)]
    require(re.findall(r"^\[(\d+\.\d+)\]", output, re.M) == groups,
            "missing or reordered test groups")
    require(len(re.findall(r"^  .*✓ .*PASS$", output, re.M)) == 17,
            "missing group PASS results")
    require(not re.search(r"^page table ", output, re.M), "unexpected table in syscall tests")


def run():
    os.chdir(ROOT)
    subprocess.run(["make", "-j4", "kernel/kernel", "mkfs/mkfs"] +
                   ["user/_" + p for p in ["init", "sh", "echo", "project2test", "vmshow", "vmwide", "execbadtest"]],
                   check=True, stdout=subprocess.DEVNULL)
    with tempfile.TemporaryDirectory(prefix="project2-") as tmp:
        disk = Path(tmp)
        names = ["_init", "_sh", "_echo", "_project2test", "_vmshow", "_vmwide", "_execbadtest"]
        for name in names:
            (disk / name).symlink_to(ROOT / "user" / name)
        (disk / "README").symlink_to(ROOT / "README")
        subprocess.run([str(ROOT / "mkfs/mkfs"), "test.img", "README"] + names,
                       cwd=disk, check=True, stdout=subprocess.DEVNULL)
        args = ["qemu-system-riscv64", "-machine", "virt", "-bios", "none",
                "-kernel", "kernel/kernel", "-m", "128M", "-smp", os.environ.get("CPUS", "3"), "-nographic",
                "-global", "virtio-mmio.force-legacy=false", "-drive",
                f"file={disk}/test.img,if=none,format=raw,id=x0", "-device",
                "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0", "-snapshot"]
        with (ROOT / "project2.out").open("wb") as log:
            process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT)

            def prompt(show=False):
                data = b""
                deadline = time.monotonic() + 180
                while time.monotonic() < deadline:
                    if not select.select([process.stdout], [], [], 1)[0]:
                        continue
                    chunk = os.read(process.stdout.fileno(), 65536)
                    require(chunk, "QEMU exited before shell prompt; see project2.out")
                    data += chunk
                    log.write(chunk)
                    log.flush()
                    if show:
                        sys.stdout.buffer.write(chunk)
                        sys.stdout.buffer.flush()
                    if data.endswith(b"$ "):
                        return data.decode("utf-8", "replace").replace("\r", "")
                raise AssertionError("QEMU timeout; see project2.out")

            def command(cmd):
                process.stdin.write((cmd + "\n").encode())
                process.stdin.flush()
                output = prompt(show=True)
                require(output.startswith(cmd + "\n"), "unexpected shell command echo")
                require("panic:" not in output, "kernel panic; see project2.out")
                return output[len(cmd) + 1:-2]

            try:
                prompt()
                validate_project(command("project2test"))
                print("\nVMPRINT | Linux checks console output against ELF", flush=True)
                require(command("echo normal-exec") == "normal-exec\n", "ordinary exec output changed")
                tree = command("vmshow echo execvm-exec")
                small = validate_tree(tree, ROOT / "user/_echo", "execvm-exec")
                large = validate_tree(command("vmshow vmwide"), ROOT / "user/_vmwide", "VMWIDE OK")
                expected = expected_leaves(ROOT / "user/_vmwide")
                require(any(1 << 21 <= va < MAXVA - 2 * PAGE for va in expected),
                        "fixture did not cross 2 MiB")
                print("\nExpected execvm error returns: -1 for each case.", flush=True)
                require(command("execbadtest") ==
                        "missing file: -1\nnon-ELF file: -1\ninvalid argv: -1\n",
                        "execvm error returns/output changed")
                check_validator(tree)
                print("\n  │ Tree       │ Leaves │ Tables │ ELF mappings / permissions / PTE / PA │")
                for name, counts in [("echo", small), ("vmwide", large)]:
                    print(f"  │ {name:10} │ {counts[0]:6} │ {counts[1]:6} │ verified                              │")
                print("  \033[42;97m ✓ \033[0m PASS: ordinary exec is quiet; both trees and three execvm errors validated.")
                print("  Checker self-check: rejected missing tree/leaf, wrong PA and wrong permissions.")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    try:
        run()
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f"\n  \033[41;97m ✗ \033[0m FAIL: {error}")
        raise SystemExit(1)
