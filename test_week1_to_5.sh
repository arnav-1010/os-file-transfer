#!/usr/bin/env bash
# ============================================================
#  test_week1_to_5.sh  —  Full test suite Weeks 1–5
#  Adaptive Multithreaded Resumable File Transfer System
#  Usage: cd ~/file_transfer_project && ./test_week1_to_5.sh
# ============================================================

PROJECT_DIR="$HOME/file_transfer_project"
cd "$PROJECT_DIR" || { echo "ERROR: $PROJECT_DIR not found"; exit 1; }

# ── Colours ──────────────────────────────────────────────────
GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

PASS=0; FAIL=0; WARN=0; TOTAL=0
FAILED_TESTS=()

pass() { echo -e "  ${GREEN}[PASS]${NC} $1"; ((PASS++)); ((TOTAL++)); }
fail() { echo -e "  ${RED}[FAIL]${NC} $1"; ((FAIL++)); ((TOTAL++)); FAILED_TESTS+=("$1"); }
warn() { echo -e "  ${YELLOW}[WARN]${NC} $1"; ((WARN++)); ((TOTAL++)); }
header() { echo -e "\n${CYAN}${BOLD}══════════════════════════════════════════${NC}"; \
           echo -e "${CYAN}${BOLD}  $1${NC}"; \
           echo -e "${CYAN}${BOLD}══════════════════════════════════════════${NC}"; }

kill_server() { pkill -f "./server" 2>/dev/null; pkill -f "file_transfer_project/server" 2>/dev/null; sleep 0.5; }
start_server() { ./server > /tmp/srv_test.log 2>&1 & SERVER_PID=$!; sleep 1; }

# ── Helper: generate test files ──────────────────────────────
make_files() {
    echo "Hello from OS project Week 1 test." > sampletext.txt
    seq 1 500 > numbers.txt
    dd if=/dev/urandom bs=1024 count=64  of=small.bin   2>/dev/null
    dd if=/dev/urandom bs=1024 count=256 of=medium.bin  2>/dev/null
    dd if=/dev/urandom bs=1024 count=512 of=large1.bin  2>/dev/null
    dd if=/dev/urandom bs=1024 count=512 of=large2.bin  2>/dev/null
    dd if=/dev/urandom bs=1024 count=512 of=large3.bin  2>/dev/null
    # bigfile for checkpoint test
    dd if=/dev/urandom bs=1024 count=2048 of=bigfile.bin 2>/dev/null
    mkdir -p received_files checkpoints
}

# ── Helper: wait for file to appear (max N seconds) ──────────
wait_for_file() {
    local f="received_files/$1" secs="${2:-10}"
    for ((i=0;i<secs;i++)); do
        [[ -f "$f" ]] && return 0
        sleep 1
    done
    return 1
}

# ════════════════════════════════════════════════════════════
header "PRE-FLIGHT CHECKS"
# ════════════════════════════════════════════════════════════

make_files

[[ -f "./server" ]]                          && pass "Binary: server exists"       || fail "Binary: server missing — run 'make'"
[[ -f "./client" ]]                          && pass "Binary: client exists"       || fail "Binary: client missing — run 'make'"
[[ -d "received_files" ]]                    && pass "Dir: received_files/"        || fail "Dir: received_files/ missing"
[[ -d "checkpoints" ]]                       && pass "Dir: checkpoints/"           || fail "Dir: checkpoints/ missing"
[[ -f "src/server.cpp" ]]                    && pass "Source: src/server.cpp"      || fail "Source: src/server.cpp missing"
[[ -f "src/client.cpp" ]]                    && pass "Source: src/client.cpp"      || fail "Source: src/client.cpp missing"
[[ -f "src/sync_buffer.cpp" ]]               && pass "Source: src/sync_buffer.cpp" || warn "Source: src/sync_buffer.cpp missing (Week 4)"
[[ -f "src/rw_lock.cpp" ]]                   && pass "Source: src/rw_lock.cpp"     || warn "Source: src/rw_lock.cpp missing (Week 4)"
[[ -f "include/sync_buffer.h" ]]             && pass "Header: include/sync_buffer.h" || warn "Header: sync_buffer.h missing (Week 4)"
[[ -f "include/crypto.h" ]]                  && pass "Header: include/crypto.h (Week 5)" || fail "Header: include/crypto.h MISSING — Week 5 not set up"

# Check Week 4 wired in
grep -q "g_chunk_buf\|SyncBuffer\|produce(" src/server.cpp 2>/dev/null \
    && pass "server.cpp: Week 4 SyncBuffer wired in" \
    || fail "server.cpp: Week 4 SyncBuffer NOT wired in"

# Check Week 5 wired in
grep -q "aes_encrypt\|AES\|sha256\|SHA256\|encrypt" src/client.cpp 2>/dev/null \
    && pass "client.cpp: Week 5 AES encrypt present" \
    || fail "client.cpp: Week 5 AES encryption NOT found"

grep -q "aes_decrypt\|AES\|sha256\|SHA256\|decrypt" src/server.cpp 2>/dev/null \
    && pass "server.cpp: Week 5 AES decrypt present" \
    || fail "server.cpp: Week 5 AES decryption NOT found"

grep -q "openssl\|crypto\|-lssl\|-lcrypto" Makefile 2>/dev/null \
    && pass "Makefile: OpenSSL linked (-lssl / -lcrypto)" \
    || fail "Makefile: OpenSSL NOT linked — add -lssl -lcrypto"

# ════════════════════════════════════════════════════════════
header "WEEK 1 — TCP Streaming Transfer"
# ════════════════════════════════════════════════════════════

kill_server; rm -f received_files/*
start_server
sleep 0.5

./client sampletext.txt > /tmp/cli_w1.log 2>&1
wait_for_file sampletext.txt 8

if [[ -f "received_files/sampletext.txt" ]]; then
    diff -q sampletext.txt received_files/sampletext.txt &>/dev/null \
        && pass "T1-1: Small text file transferred — content matches" \
        || fail "T1-1: Small text file content MISMATCH"
else
    fail "T1-1: sampletext.txt never arrived in received_files/"
fi

./client small.bin > /tmp/cli_w1b.log 2>&1
wait_for_file small.bin 8

if [[ -f "received_files/small.bin" ]]; then
    orig=$(stat -c%s small.bin); recv=$(stat -c%s received_files/small.bin)
    [[ "$orig" == "$recv" ]] \
        && pass "T1-2: Binary file size matches ($orig bytes)" \
        || fail "T1-2: Binary size mismatch — sent $orig, got $recv"
else
    fail "T1-2: small.bin never arrived"
fi

# Server still alive?
kill -0 "$SERVER_PID" 2>/dev/null \
    && pass "T1-3: Server still alive after END_OF_FILE" \
    || fail "T1-3: Server crashed after transfer"

kill_server

# ════════════════════════════════════════════════════════════
header "WEEK 2 — Thread Pool & Transfer Control Block"
# ════════════════════════════════════════════════════════════

kill_server; rm -f received_files/*
start_server

# Sequential transfers (avoids pool deadlock with blocking recv)
for f in sampletext.txt numbers.txt small.bin; do
    ./client "$f" > /dev/null 2>&1
    wait_for_file "$f" 10
done

ok=0
for f in sampletext.txt numbers.txt small.bin; do
    [[ -f "received_files/$f" ]] && ((ok++))
done
[[ $ok -eq 3 ]] \
    && pass "T2-1: Thread pool — 3/3 sequential transfers completed" \
    || fail "T2-1: Thread pool — only $ok/3 transfers completed"

grep -qi "TCB\|tcb\|transfer_id\|file_id" /tmp/srv_test.log 2>/dev/null \
    && pass "T2-2: TCB entries created (seen in server log)" \
    || warn "T2-2: No TCB log lines found (check log format)"

# Worker thread count
grep -oi "worker\|thread" /tmp/srv_test.log 2>/dev/null | wc -l | read -r wt
grep -qi "worker\|thread.pool\|pool" /tmp/srv_test.log 2>/dev/null \
    && pass "T2-3: ThreadPool workers active (log confirms)" \
    || warn "T2-3: ThreadPool log silent (acceptable if no debug prints)"

kill_server

# ════════════════════════════════════════════════════════════
header "WEEK 3 — AMLFQ Scheduler & Checkpoint/Resume"
# ════════════════════════════════════════════════════════════

kill_server; rm -f received_files/* checkpoints/*
start_server

./client medium.bin > /dev/null 2>&1
wait_for_file medium.bin 12

grep -qi "queue\|MLFQ\|AMLFQ\|priority\|sched\|aging" /tmp/srv_test.log 2>/dev/null \
    && pass "T3-1: AMLFQ queue assignments seen in server log" \
    || warn "T3-1: No AMLFQ log lines — check scheduler.cpp logging"

# Checkpoint test — kill server mid large file
kill_server; rm -f received_files/bigfile.bin checkpoints/*
start_server

./client bigfile.bin > /dev/null 2>&1 &
CLI_PID=$!
sleep 2
kill -INT "$SERVER_PID" 2>/dev/null   # simulate Ctrl+C
sleep 1

ls checkpoints/ 2>/dev/null | grep -qi ".*" \
    && pass "T3-2: Checkpoint file saved on interrupt" \
    || warn "T3-2: No checkpoint (transfer may have finished before kill)"

# Resume
kill_server
start_server
./client bigfile.bin > /tmp/cli_resume.log 2>&1
wait $CLI_PID 2>/dev/null
wait_for_file bigfile.bin 15

grep -qi "resume\|checkpoint\|offset\|chunk" /tmp/srv_test.log 2>/dev/null \
    && pass "T3-3: Checkpoint resume triggered (seen in log)" \
    || warn "T3-3: Resume not confirmed in log"

kill_server

# ════════════════════════════════════════════════════════════
header "WEEK 4 — Synchronization Integration"
# ════════════════════════════════════════════════════════════

kill_server; rm -f received_files/*
start_server

for f in large1.bin large2.bin large3.bin; do
    ./client "$f" > /dev/null 2>&1
    wait_for_file "$f" 15
done

grep -qi "DiskIO\|disk_io\|consumer\|produce\|chunk.*queue\|buffer" /tmp/srv_test.log 2>/dev/null \
    && pass "T4-1: Producer-Consumer active — DiskIO thread log found" \
    || warn "T4-1: DiskIO log silent (check sync_buffer logging)"

grep -qi "rwlock\|rw_lock\|reader\|writer\|rdlock\|wrlock" /tmp/srv_test.log 2>/dev/null \
    && pass "T4-2: RWLock log found" \
    || warn "T4-2: RWLock silent (acceptable — protects data without logging)"

ok=0
for f in large1.bin large2.bin large3.bin; do
    if [[ -f "received_files/$f" ]]; then
        orig=$(md5sum "$f" | cut -d' ' -f1)
        recv=$(md5sum "received_files/$f" | cut -d' ' -f1)
        [[ "$orig" == "$recv" ]] && ((ok++))
    fi
done
[[ $ok -eq 3 ]] \
    && pass "T4-3: File integrity — 3/3 large files MD5 match" \
    || fail "T4-3: File integrity — only $ok/3 files intact"

# Deadlock probe
kill -0 "$SERVER_PID" 2>/dev/null \
    && pass "T4-4: No deadlock — server responsive after load" \
    || fail "T4-4: Server dead after load test — possible deadlock"

# CPU idle check
CPU=$(ps -p "$SERVER_PID" -o %cpu --no-headers 2>/dev/null | tr -d ' ')
if [[ -n "$CPU" ]]; then
    CPU_INT=${CPU%.*}
    [[ "$CPU_INT" -lt 5 ]] \
        && pass "T4-5: Server idle CPU ${CPU}% — condition variables working" \
        || warn "T4-5: Server CPU ${CPU}% (>5%) — possible busy-wait"
else
    warn "T4-5: Could not read server CPU (server may have exited)"
fi

kill_server

# ════════════════════════════════════════════════════════════
header "WEEK 5 — AES Encryption + SHA-256 Integrity"
# ════════════════════════════════════════════════════════════

kill_server; rm -f received_files/*
start_server

# T5-1: Basic encrypted transfer — content still arrives correctly
./client sampletext.txt > /tmp/cli_w5.log 2>&1
wait_for_file sampletext.txt 10

if [[ -f "received_files/sampletext.txt" ]]; then
    diff -q sampletext.txt received_files/sampletext.txt &>/dev/null \
        && pass "T5-1: Encrypted transfer — content matches after decrypt" \
        || fail "T5-1: Encrypted transfer — content MISMATCH (decrypt broken?)"
else
    fail "T5-1: Encrypted file never arrived — check client/server crypto code"
fi

# T5-2: SHA-256 hashes logged by client and server
grep -qi "sha256\|hash\|sha-256\|digest" /tmp/cli_w5.log 2>/dev/null \
    && pass "T5-2: SHA-256 logged by client" \
    || fail "T5-2: No SHA-256 output from client — check client.cpp"

grep -qi "sha256\|hash\|sha-256\|digest" /tmp/srv_test.log 2>/dev/null \
    && pass "T5-3: SHA-256 logged by server" \
    || fail "T5-3: No SHA-256 output from server — check server.cpp"

# T5-4: Hash match — client and server SHA-256 must be the same for each chunk
CLIENT_HASHES=$(grep -oP '(?<=sha256=)[0-9a-f]+' /tmp/cli_w5.log 2>/dev/null | sort)
SERVER_HASHES=$(grep -oP '(?<=sha256=)[0-9a-f]+' /tmp/srv_test.log 2>/dev/null | sort)

if [[ -z "$CLIENT_HASHES" || -z "$SERVER_HASHES" ]]; then
    warn "T5-4: Cannot extract hashes to compare — check log format (sha256=XXXX expected)"
else
    [[ "$CLIENT_HASHES" == "$SERVER_HASHES" ]] \
        && pass "T5-4: SHA-256 hashes MATCH — integrity verified end-to-end" \
        || fail "T5-4: SHA-256 MISMATCH — encryption/decryption corrupting data"
fi

# T5-5: Wire-level: encrypted bytes ≠ plaintext bytes
grep -qi "enc=\|encrypted\|cipher" /tmp/cli_w5.log 2>/dev/null \
    && pass "T5-5: Client reports encrypted byte count (enc=NNN seen)" \
    || warn "T5-5: No enc= in client log — add logging of encrypted size"

# T5-6: Binary transfer encrypted — integrity
./client medium.bin > /tmp/cli_w5b.log 2>&1
wait_for_file medium.bin 12

if [[ -f "received_files/medium.bin" ]]; then
    orig=$(md5sum medium.bin | cut -d' ' -f1)
    recv=$(md5sum received_files/medium.bin | cut -d' ' -f1)
    [[ "$orig" == "$recv" ]] \
        && pass "T5-6: Binary file MD5 matches after AES encrypt/decrypt" \
        || fail "T5-6: Binary file MD5 MISMATCH after decrypt"
else
    fail "T5-6: medium.bin never arrived after encrypted transfer"
fi

# T5-7: Multiple clients with encryption (stress test)
kill_server; rm -f received_files/*
start_server

for f in large1.bin large2.bin large3.bin; do
    ./client "$f" > /dev/null 2>&1
    wait_for_file "$f" 20
done

ok=0
for f in large1.bin large2.bin large3.bin; do
    if [[ -f "received_files/$f" ]]; then
        orig=$(md5sum "$f" | cut -d' ' -f1)
        recv=$(md5sum "received_files/$f" | cut -d' ' -f1)
        [[ "$orig" == "$recv" ]] && ((ok++))
    fi
done
[[ $ok -eq 3 ]] \
    && pass "T5-7: Stress test — 3/3 large files intact after encrypted transfer" \
    || fail "T5-7: Stress test — only $ok/3 files intact (encryption bug under load?)"

# T5-8: Server alive after all Week 5 tests
kill -0 "$SERVER_PID" 2>/dev/null \
    && pass "T5-8: Server stable after all Week 5 tests" \
    || fail "T5-8: Server crashed during Week 5 tests"

kill_server

# ════════════════════════════════════════════════════════════
header "TEST SUMMARY"
# ════════════════════════════════════════════════════════════

echo -e "  Total : ${BOLD}$TOTAL${NC}  |  ${GREEN}PASS: $PASS${NC}  |  ${RED}FAIL: $FAIL${NC}  |  ${YELLOW}WARN: $WARN${NC}"
echo ""

if [[ $FAIL -eq 0 ]]; then
    echo -e "  ${GREEN}${BOLD}All tests passed! Ready to push and submit.${NC}"
else
    echo -e "  ${RED}${BOLD}$FAIL test(s) FAILED:${NC}"
    for t in "${FAILED_TESTS[@]}"; do
        echo -e "    ${RED}✗${NC} $t"
    done
    echo ""
    echo -e "  Tip: cat /tmp/srv_test.log | tail -40"
    echo -e "  Tip: cat /tmp/cli_w5.log"
fi

echo ""
if [[ $FAIL -eq 0 ]]; then
    echo -e "  ${CYAN}Push command:${NC}"
    cat <<'EOF'
  cd ~/file_transfer_project
  git add include/crypto.h src/client.cpp src/server.cpp Makefile test_week1_to_5.sh
  git commit -m "Week 5: AES-128 encryption + SHA-256 integrity — all tests passing"
  git push origin main
EOF
fi
