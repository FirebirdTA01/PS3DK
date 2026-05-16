#!/usr/bin/env bash
# rpcs3-sweep.sh -- sweep RPCS3 samples with per-run log preservation
#
# This is a new harness. It does not replace scripts/rpcs3-run.sh.
#
# Modes:
#   1. Run mode:
#        scripts/rpcs3-sweep.sh [options] sample1.fake.self [sample2.fake.self ...]
#      Runs each sample under RPCS3, preserves the full RPCS3.log to an
#      immutable per-sample results directory, and classifies the run.
#
#   2. Log classification mode:
#        scripts/rpcs3-sweep.sh --classify-log label=path/to/RPCS3.log [...]
#      Classifies preserved logs without launching RPCS3. Used for validation.
#
# Exit policy:
#   nonzero iff any sample classifies as BOOT-FAIL or RAN-THEN-FATAL.
#
# Classification states:
#   BOOT-FAIL
#   RAN-THEN-FATAL
#   RAN-CLEAN
#   RENDER-LOOP
#   UNKNOWN

set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
DEFAULT_RPCS3_BIN="${RPCS3_BIN:-rpcs3}"
DEFAULT_LOG_FILE="${HOME}/.cache/rpcs3/RPCS3.log"
DEFAULT_RESULTS_DIR="${RPCS3_SWEEP_RESULTS_DIR:-build/rpcs3-sweep-results}"
DEFAULT_TIMEOUT="${RPCS3_SWEEP_TIMEOUT:-20}"

TTY_RE='sys_tty_write\(\): '
PROCESS_FINISHED_RE='sys_process: Process finished|_sys_process_exit\(status='
FATAL_RE='(^·F[[:space:]]|^F[[:space:]]|VM: Access violation|Emulation has been frozen|Emulation frozen|Thread terminated due to fatal error|Segfault|SIGSEGV)'

usage() {
    cat >&2 <<EOF
Usage:
  $SCRIPT_NAME [options] sample1.fake.self [sample2.fake.self ...]
  $SCRIPT_NAME --classify-log label=path/to/RPCS3.log [--classify-log label=...]

Options:
  --rpcs3-bin PATH        RPCS3 binary to run (default: \$RPCS3_BIN or 'rpcs3')
  --results-dir DIR       Root results directory for preserved logs
  --timeout SECONDS       Timeout per sample in run mode
  --classify-log LABEL=PATH
                          Classify an existing log without launching RPCS3
  --help                  Show this help
EOF
    exit 2
}

die() {
    echo "${SCRIPT_NAME}: $*" >&2
    exit 1
}

trim() {
    local s="$1"
    # shellcheck disable=SC2001
    printf '%s' "$(printf '%s' "$s" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
}

slugify() {
    local s="$1"
    s="${s##*/}"
    s="${s%.fake.self}"
    s="${s// /_}"
    s="${s//[^A-Za-z0-9._-]/_}"
    printf '%s' "$s"
}

count_matches() {
    local pattern="$1"
    local file="$2"
    grep -Ec "$pattern" "$file" 2>/dev/null || true
}

has_match() {
    local pattern="$1"
    local file="$2"
    grep -Eq "$pattern" "$file" 2>/dev/null
}

classify_log() {
    local label="$1"
    local log_path="$2"
    local timeout_hit="${3:-n/a}"

    local tty_count=0
    local fatal_count=0
    local terminal_seen=0
    local state="UNKNOWN"
    local notes=""

    if [[ ! -f "$log_path" ]]; then
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$label" "BOOT-FAIL" "0" "0" "no" "$timeout_hit" "$log_path" "log missing" 
        return 0
    fi

    tty_count="$(count_matches "$TTY_RE" "$log_path")"
    fatal_count="$(count_matches "$FATAL_RE" "$log_path")"
    if has_match "$PROCESS_FINISHED_RE" "$log_path"; then
        terminal_seen=1
    fi

    tty_count="${tty_count:-0}"
    fatal_count="${fatal_count:-0}"

    if [[ "$tty_count" -eq 0 ]]; then
        state="BOOT-FAIL"
        notes="no sys_tty_write lines"
    elif [[ "$fatal_count" -gt 0 ]]; then
        state="RAN-THEN-FATAL"
        notes="emulator fatal markers present"
    elif [[ "$terminal_seen" -eq 1 ]]; then
        state="RAN-CLEAN"
        notes="sys_process: Process finished present, no emulator fatal markers"
    elif [[ "$timeout_hit" == "yes" ]]; then
        state="RENDER-LOOP"
        notes="timeout-hit with tty output and no emulator fatal markers"
    else
        state="UNKNOWN"
        notes="tty output present, but no terminal marker or emulator fatal marker"
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$state" "$tty_count" "$fatal_count" \
        "$([ "$terminal_seen" -eq 1 ] && printf yes || printf no)" \
        "$timeout_hit" "$log_path" "$notes"
}

run_sample() {
    local sample_path="$1"
    local results_root="$2"
    local rpcs3_bin="$3"
    local timeout_s="$4"
    local sample_label
    local run_stamp
    local run_dir
    local preserved_log
    local timeout_rc=0
    local timeout_hit="no"
    local log_for_classification

    [[ -f "$sample_path" ]] || die "sample file not found: $sample_path"

    if ! command -v "$rpcs3_bin" >/dev/null 2>&1; then
        die "RPCS3 binary not found on PATH: $rpcs3_bin"
    fi

    if pgrep -x rpcs3 >/dev/null 2>&1; then
        die "rpcs3 is already running; aborting sweep preflight"
    fi

    sample_label="$(slugify "$sample_path")"
    run_stamp="$(date +%Y%m%d-%H%M%S)-$$-$RANDOM"
    run_dir="${results_root}/${sample_label}/${run_stamp}"
    mkdir -p "$run_dir"

    echo "RUN	${sample_label}	booting" >&2
    : > "$DEFAULT_LOG_FILE"

    set +e
    timeout --signal=KILL "$timeout_s" "$rpcs3_bin" --no-gui "$sample_path" >/dev/null 2>&1
    timeout_rc=$?
    set -e

    if [[ "$timeout_rc" -eq 124 || "$timeout_rc" -eq 137 ]]; then
        timeout_hit="yes"
    fi

    preserved_log="${run_dir}/RPCS3.log"
    if [[ -f "$DEFAULT_LOG_FILE" ]]; then
        cp -p "$DEFAULT_LOG_FILE" "$preserved_log"
    else
        : > "$preserved_log"
    fi

    log_for_classification="$preserved_log"
    classify_log "$sample_label" "$log_for_classification" "$timeout_hit"
}

main() {
    local rpcs3_bin="$DEFAULT_RPCS3_BIN"
    local results_dir="$DEFAULT_RESULTS_DIR"
    local timeout_s="$DEFAULT_TIMEOUT"
    local -a classify_inputs=()
    local -a sample_inputs=()
    local arg

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --help)
                usage
                ;;
            --rpcs3-bin)
                [[ $# -ge 2 ]] || usage
                rpcs3_bin="$2"
                shift 2
                ;;
            --results-dir)
                [[ $# -ge 2 ]] || usage
                results_dir="$2"
                shift 2
                ;;
            --timeout)
                [[ $# -ge 2 ]] || usage
                timeout_s="$2"
                shift 2
                ;;
            --classify-log)
                [[ $# -ge 2 ]] || usage
                classify_inputs+=("$2")
                shift 2
                ;;
            --)
                shift
                while [[ $# -gt 0 ]]; do
                    sample_inputs+=("$1")
                    shift
                done
                ;;
            -*)
                usage
                ;;
            *)
                sample_inputs+=("$1")
                shift
                ;;
        esac
    done

    if [[ "${#classify_inputs[@]}" -gt 0 && "${#sample_inputs[@]}" -gt 0 ]]; then
        die "choose either run mode or classify-log mode, not both"
    fi

    printf 'sample\tstate\ttty_count\tfatal_count\tterminal_seen\ttimeout_hit\tlog_path\tnotes\n'

    local total=0
    local boot_fail=0
    local ran_then_fatal=0
    local ran_clean=0
    local render_loop=0
    local unknown=0
    local fail_exit=0
    local row
    local label
    local log_path
    local state

    if [[ "${#classify_inputs[@]}" -gt 0 ]]; then
        for row in "${classify_inputs[@]}"; do
            label="${row%%=*}"
            log_path="${row#*=}"
            if [[ -z "$label" || -z "$log_path" || "$label" == "$log_path" ]]; then
                die "--classify-log expects LABEL=PATH"
            fi
            row="$(classify_log "$label" "$log_path" "n/a")"
            printf '%s\n' "$row"
            total=$((total + 1))
            state="$(printf '%s' "$row" | awk -F '\t' '{print $2}')"
            case "$state" in
                BOOT-FAIL) boot_fail=$((boot_fail + 1)); fail_exit=1 ;;
                RAN-THEN-FATAL) ran_then_fatal=$((ran_then_fatal + 1)); fail_exit=1 ;;
                RAN-CLEAN) ran_clean=$((ran_clean + 1)) ;;
                RENDER-LOOP) render_loop=$((render_loop + 1)) ;;
                UNKNOWN) unknown=$((unknown + 1)) ;;
            esac
        done
    else
        [[ "${#sample_inputs[@]}" -gt 0 ]] || usage
        mkdir -p "$results_dir"
        for arg in "${sample_inputs[@]}"; do
            row="$(run_sample "$arg" "$results_dir" "$rpcs3_bin" "$timeout_s")"
            printf '%s\n' "$row"
            total=$((total + 1))
            state="$(printf '%s' "$row" | awk -F '\t' '{print $2}')"
            case "$state" in
                BOOT-FAIL) boot_fail=$((boot_fail + 1)); fail_exit=1 ;;
                RAN-THEN-FATAL) ran_then_fatal=$((ran_then_fatal + 1)); fail_exit=1 ;;
                RAN-CLEAN) ran_clean=$((ran_clean + 1)) ;;
                RENDER-LOOP) render_loop=$((render_loop + 1)) ;;
                UNKNOWN) unknown=$((unknown + 1)) ;;
            esac
        done
    fi

    {
        echo
        echo "summary	total=${total}	BOOT-FAIL=${boot_fail}	RAN-THEN-FATAL=${ran_then_fatal}	RAN-CLEAN=${ran_clean}	RENDER-LOOP=${render_loop}	UNKNOWN=${unknown}"
        if [[ "$boot_fail" -gt 0 || "$ran_then_fatal" -gt 0 ]]; then
            echo "failures"
        else
            echo "failures	none"
        fi
    } >&2

    if [[ "$fail_exit" -ne 0 ]]; then
        exit 1
    fi
}

main "$@"
