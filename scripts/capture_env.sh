#!/usr/bin/env bash
# Write a description of the machine and build to a file, for benchmark and
# profiling output directories.
#
#   scripts/capture_env.sh OUT_FILE BUILD_DIR [EXTRA_LINE...]
#
# Each EXTRA_LINE is appended as-is (for example "bench_args: seed=42").
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out_file="$1"
build_dir="$2"
shift 2

cache_var() { sed -n "s/^$1:[A-Z]*=//p" "$build_dir/CMakeCache.txt" 2>/dev/null | head -n1; }
read_file() { cat "$1" 2>/dev/null || echo "unavailable"; }

{
    echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "git_commit: $(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
    if [ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null)" ]; then echo "git_tree: dirty"; else echo "git_tree: clean"; fi
    echo "kernel: $(uname -srvm)"
    echo "os: $(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-unknown}")"
    echo "cpu_model: $(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -n1)"
    echo "logical_cpus: $(nproc --all)"
    echo "memory_kib: $(sed -n 's/^MemTotal:[[:space:]]*\([0-9]*\) kB/\1/p' /proc/meminfo)"
    command -v lscpu >/dev/null 2>&1 && lscpu | grep -E 'Thread|Core|Socket|L1d|L2|L3|Hypervisor|Virtualization type' | sed 's/^/lscpu: /'
    echo "hypervisor_flag: $(grep -qm1 hypervisor /proc/cpuinfo && echo yes || echo no)"
    echo "cpufreq_governor: $(read_file /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
    echo "intel_pstate_no_turbo: $(read_file /sys/devices/system/cpu/intel_pstate/no_turbo)"
    echo "smt_active: $(read_file /sys/devices/system/cpu/smt/active)"
    echo "perf_event_paranoid: $(read_file /proc/sys/kernel/perf_event_paranoid)"
    echo "kernel_cmdline: $(read_file /proc/cmdline)"
    echo "compiler: $("${CXX:-c++}" --version 2>/dev/null | head -n1)"
    echo "cmake_build_type: $(cache_var CMAKE_BUILD_TYPE)"
    echo "cmake_cxx_compiler: $(cache_var CMAKE_CXX_COMPILER)"
    echo "cmake_cxx_flags: $(cache_var CMAKE_CXX_FLAGS)"
    echo "cmake_cxx_flags_release: $(cache_var CMAKE_CXX_FLAGS_RELEASE)"
    echo "cmake_cxx_flags_relwithdebinfo: $(cache_var CMAKE_CXX_FLAGS_RELWITHDEBINFO)"
    for line in "$@"; do echo "$line"; done
} > "$out_file"
