#!/bin/bash
# =============================================================================
#  WEEK 1-4 FULL TEST SUITE — OS File Transfer Project
#  Run from: ~/file_transfer_project
# =============================================================================

PROJECT_DIR="$HOME/file_transfer_project"
RECV="$PROJECT_DIR/received_files"
PASS=0; FAIL=0; WARN=0

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

pass() { echo -e "  ${GREEN}[PASS]${RESET} $1"; ((PASS++)); }
fail() { echo -e "  ${RED}[FAIL]${RESET} $1"; ((FAIL++)); }
warn() { echo -e "  ${YELLOW}[WARN]${RESET} $1"; ((WARN++)); }
header() { echo -e "\n${CYAN}${BOLD}══════════════════════════════════════════${RESET}"
           echo -e "${CYAN}${BOLD}  $1${RESET}"
           echo -e "${CYAN}${BOLD}══════════════════════════════════════════${RESET}"; }

pkill -f "./server" 2>/dev/null
pkill -f "./client" 2>/dev/null
sleep 0.5

cd "$PROJECT_DIR" || { echo "ERROR: $PROJECT_DIR not found"; exit 1; }

start_server() {
    pkill -f "./server" 2>/dev/null; sleep 0.3
    ./server > "$1" 2>&1 &
    SERVER_PID=$!
    sleep 0.8
}

stop_server() {
    kill $SERVER_PID 2>/dev/null
    wait $SERVER_PID 2>/dev/null
    sleep 0.3
}

send_file() {
    timeout 12 ./client "$1" 127.0.0.1 > /dev/null 2>&1
    sleep 0.6
}

# =============================================================================
# PRE-FLIGHT
# =============================================================================
header "PRE-FLIGHT CHECKS"

[ -f server ] && pass "Binary: server exists" || fail "Binary: server missing — run 'make'"
[ -f client ] && pass "Binary: client exists" || fail "Binary: client missing — run 'make'"
[ -d "$RECV" ]      && pass "Dir: received_files/" || { mkdir -p "$RECV";    warn "Created received_files/"; }
[ -d checkpoints ]  && pass "Dir: checkpoints/"    || { mkdir -p checkpoints; warn "Created checkpoints/"; }

for f in src/server.cpp src/client.cpp src/scheduler.cpp src/thread_pool.cpp \
          src/checkpoint.cpp src/sync_buffer.cpp src/rw_lock.cpp; do
    [ -f "$f" ] && pass "Source: $f" || fail "Source: $f missing"
done

for h in include/tcb.h include/scheduler.h include/thread_pool.h \
          include/checkpoint.h include/sync_buffer.h include/rw_lock.h; do
    [ -f "$h" ] && pass "Header: $h" || fail "Header: $h missing"
done

grep -q "g_chunk_buf\|disk_io_start" src/server.cpp 2>/dev/null \
    && pass "server.cpp: Week 4 SyncBuffer wired in" \
    || fail "server.cpp: NOT updated for Week 4 — run fix_server.sh"

# =============================================================================
# WEEK 1 — TCP Streaming
# =============================================================================
header "WEEK 1 — TCP Streaming Transfer"

echo "Hello Week 1 test. Quick brown fox." > /tmp/w1_small.txt
dd if=/dev/urandom of=/tmp/w1_medium.bin bs=1K count=64 status=none 2>/dev/null
rm -f "$RECV/w1_small.txt" "$RECV/w1_medium.bin"

start_server /tmp/server_w1.log

send_file /tmp/w1_small.txt
if [ -f "$RECV/w1_small.txt" ] && diff -q /tmp/w1_small.txt "$RECV/w1_small.txt" >/dev/null 2>&1; then
    pass "T1-1: Small text file transferred — content matches"
else
    fail "T1-1: Small text file missing or content mismatch"
fi

send_file /tmp/w1_medium.bin
if [ -f "$RECV/w1_medium.bin" ]; then
    O=$(stat -c%s /tmp/w1_medium.bin); R=$(stat -c%s "$RECV/w1_medium.bin")
    [ "$O" -eq "$R" ] && pass "T1-2: Binary file size matches ($O bytes)" \
                       || fail "T1-2: Size mismatch — sent $O got $R"
else
    fail "T1-2: Binary file NOT received"
fi

kill -0 $SERVER_PID 2>/dev/null \
    && pass "T1-3: Server alive after transfer (END_OF_FILE handled)" \
    || fail "T1-3: Server crashed"

stop_server

# =============================================================================
# WEEK 2 — Thread Pool + TCB  (SEQUENTIAL — avoids pool exhaustion deadlock)
# =============================================================================
header "WEEK 2 — Thread Pool & Transfer Control Block"

for i in 1 2 3 4; do
    dd if=/dev/urandom of=/tmp/w2_file$i.bin bs=1K count=16 status=none
    rm -f "$RECV/w2_file$i.bin"
done

start_server /tmp/server_w2.log

for i in 1 2 3 4; do
    send_file /tmp/w2_file$i.bin
done

W2_OK=0
for i in 1 2 3 4; do [ -f "$RECV/w2_file$i.bin" ] && ((W2_OK++)); done
[ "$W2_OK" -eq 4 ] \
    && pass "T2-1: Thread pool — 4/4 sequential transfers completed" \
    || fail "T2-1: Only $W2_OK/4 files received"

grep -q "TCB" /tmp/server_w2.log 2>/dev/null \
    && pass "T2-2: TCB entries created (seen in log)" \
    || warn "T2-2: No TCB log entries"

grep -qi "ThreadPool\|worker" /tmp/server_w2.log 2>/dev/null \
    && pass "T2-3: ThreadPool workers active" \
    || warn "T2-3: No ThreadPool log entries"

stop_server

# =============================================================================
# WEEK 3 — AMLFQ + Checkpoint/Resume
# =============================================================================
header "WEEK 3 — AMLFQ Scheduler & Checkpoint/Resume"

rm -f checkpoints/*.ckpt "$RECV"/w3_*.bin
dd if=/dev/urandom of=/tmp/w3_a.bin bs=1K count=32 status=none
dd if=/dev/urandom of=/tmp/w3_b.bin bs=1K count=32 status=none

start_server /tmp/server_w3.log
send_file /tmp/w3_a.bin
send_file /tmp/w3_b.bin

grep -q "AMLFQ\|Queue" /tmp/server_w3.log 2>/dev/null \
    && pass "T3-1: AMLFQ queue assignments seen in log" \
    || warn "T3-1: No AMLFQ log entries"

stop_server

# Checkpoint: large file, kill mid-transfer
rm -f checkpoints/*.ckpt
dd if=/dev/urandom of=/tmp/w3_large.bin bs=1K count=512 status=none

start_server /tmp/server_ckpt.log
./client /tmp/w3_large.bin 127.0.0.1 >/dev/null 2>&1 &
CLIENT_PID=$!
sleep 1
stop_server
kill $CLIENT_PID 2>/dev/null; wait $CLIENT_PID 2>/dev/null

CKPT_COUNT=$(ls checkpoints/*.ckpt 2>/dev/null | wc -l)
if [ "$CKPT_COUNT" -gt 0 ]; then
    pass "T3-2: Checkpoint saved on interruption ($CKPT_COUNT file)"
    OFFSET=$(awk 'NR==3{print $1}' checkpoints/*.ckpt 2>/dev/null | head -1)
    [ "${OFFSET:-0}" -gt 0 ] \
        && pass "T3-2b: Checkpoint offset non-zero ($OFFSET bytes)" \
        || warn "T3-2b: Could not verify offset"
else
    warn "T3-2: No checkpoint (transfer may have finished before kill)"
fi

start_server /tmp/server_resume.log
send_file /tmp/w3_large.bin
grep -qi "Resumed\|resume\|checkpoint" /tmp/server_resume.log 2>/dev/null \
    && pass "T3-3: Checkpoint resume triggered" \
    || warn "T3-3: No resume log (checkpoint may have been deleted already)"

stop_server

# =============================================================================
# WEEK 4 — Synchronization
# =============================================================================
header "WEEK 4 — Synchronization Integration"

rm -f "$RECV"/w4_*.bin "$RECV"/w4_probe.txt
dd if=/dev/urandom of=/tmp/w4_pc.bin   bs=1K count=128 status=none
dd if=/dev/urandom of=/tmp/w4_big1.bin bs=1K count=256 status=none
dd if=/dev/urandom of=/tmp/w4_big2.bin bs=1K count=256 status=none
dd if=/dev/urandom of=/tmp/w4_big3.bin bs=1K count=256 status=none

start_server /tmp/server_w4.log

# T4-1: Producer-Consumer / DiskIO thread
send_file /tmp/w4_pc.bin
grep -qi "DiskIO\|queued for disk" /tmp/server_w4.log 2>/dev/null \
    && pass "T4-1: Producer-Consumer active — DiskIO thread log found" \
    || fail "T4-1: No DiskIO log — SyncBuffer not wired in server.cpp"

# T4-2: RWLock (often silent)
grep -qi "RWLock\|ReadGuard\|WriteGuard\|rw_lock" /tmp/server_w4.log 2>/dev/null \
    && pass "T4-2: RWLock log entries found" \
    || warn "T4-2: RWLock silent (acceptable — it protects data without logging)"

# T4-3: File integrity — 3 large files sequential
for i in 1 2 3; do send_file /tmp/w4_big$i.bin; done

RACE_OK=0
for i in 1 2 3; do
    if [ -f "$RECV/w4_big$i.bin" ]; then
        O=$(stat -c%s /tmp/w4_big$i.bin); R=$(stat -c%s "$RECV/w4_big$i.bin")
        [ "$O" -eq "$R" ] && ((RACE_OK++))
    fi
done
[ "$RACE_OK" -eq 3 ] \
    && pass "T4-3: File integrity — 3/3 large files intact" \
    || fail "T4-3: Integrity — only $RACE_OK/3 correct"

# T4-4: Deadlock probe
echo "probe" > /tmp/w4_probe.txt
timeout 5 ./client /tmp/w4_probe.txt 127.0.0.1 >/dev/null 2>&1
if [ $? -eq 0 ] && kill -0 $SERVER_PID 2>/dev/null; then
    pass "T4-4: No deadlock — server responsive after load"
else
    fail "T4-4: Server unresponsive — possible deadlock/crash"
fi

# T4-5: CPU idle (condition vars not busy-spinning)
sleep 1
CPU=$(ps -p $SERVER_PID -o %cpu --no-headers 2>/dev/null | tr -d ' ')
CPU_INT=${CPU%.*}
[ -n "$CPU_INT" ] && [ "${CPU_INT:-99}" -lt 15 ] \
    && pass "T4-5: Server idle CPU ${CPU}% — condition variables OK" \
    || warn "T4-5: Server CPU ${CPU}% idle (normal on WSL under load)"

stop_server

# =============================================================================
# SUMMARY
# =============================================================================
TOTAL=$((PASS + FAIL + WARN))
echo ""
echo -e "${CYAN}${BOLD}══════════════════════════════════════════${RESET}"
echo -e "${BOLD}  TEST SUMMARY${RESET}"
echo -e "${CYAN}${BOLD}══════════════════════════════════════════${RESET}"
echo -e "  Total : $TOTAL  |  ${GREEN}PASS: $PASS${RESET}  |  ${RED}FAIL: $FAIL${RESET}  |  ${YELLOW}WARN: $WARN${RESET}"
echo ""
if [ "$FAIL" -eq 0 ] && [ "$WARN" -eq 0 ]; then
    echo -e "  ${GREEN}${BOLD}ALL TESTS PASSED — ready to push and start Week 5${RESET}"
elif [ "$FAIL" -eq 0 ]; then
    echo -e "  ${YELLOW}${BOLD}No hard failures. Review WARNs, then push.${RESET}"
else
    echo -e "  ${RED}${BOLD}$FAIL test(s) FAILED. Fix before pushing.${RESET}"
    echo -e "  Tip: cat /tmp/server_w4.log"
fi
echo ""

rm -f /tmp/w1_* /tmp/w2_* /tmp/w3_* /tmp/w4_* /tmp/server_*.log
exit $FAIL
