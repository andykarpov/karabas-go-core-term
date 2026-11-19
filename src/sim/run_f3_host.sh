#!/bin/bash
# Ф3 host tests of the SSH client (src/fw/ssh.c) against the asyncssh test server (src/tools/ssh_test_server.py):
# seconds, no FPGA sim. usage: run_f3_host.sh [work dir, default /tmp/term_f3]
set -u
cd "$(dirname "$0")/../fw"
W=${1:-/tmp/term_f3}
mkdir -p "$W"
MC=../../upstream/monocypher/src
mkdir -p build-host
cc -O1 -g -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-format-truncation -fsanitize=address,undefined \
   -I$MC -I$MC/optional -I. -o build-host/test_ssh test_ssh.c ssh.c sha256.c $MC/monocypher.c $MC/optional/monocypher-ed25519.c || exit 1
export ASAN_OPTIONS=detect_leaks=0
T=./build-host/test_ssh
PIDS=()
server() {   # port, extra options
    local port=$1; shift
    python3 -u ../tools/ssh_test_server.py --port "$port" --keydir "$W/keys" "$@" > "$W/server_$port.log" 2>&1 &
    PIDS+=($!)
    for _ in $(seq 40); do grep -q "server on" "$W/server_$port.log" 2>/dev/null && return 0; sleep 0.25; done
    echo "server $port did not start"; cat "$W/server_$port.log"; exit 1
}
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT

rm -f "$W/known" "$W/user.key"
$T 127.0.0.1 1 x -k "$W/user.key" -p > "$W/user.pub" || exit 1
server 2222 --authorized "$W/user.pub"
server 2223 --kbi-only
server 2224 --pw-only --rekey-bytes 65536
FAIL=0
check() {    # name, expected text in the output, command...
    local name=$1 want=$2; shift 2
    "$@" > "$W/$name.out" 2> "$W/$name.err"
    local rc=$?
    if [ $rc -eq 0 ] && grep -q -- "$want" "$W/$name.out"; then echo "ok   $name"
    else echo "FAIL $name (rc $rc)"; tail -3 "$W/$name.err"; tail -5 "$W/$name.out"; FAIL=1; fi
}
checkfail() {   # name, expected text in the error, command...
    local name=$1 want=$2; shift 2
    "$@" > "$W/$name.out" 2> "$W/$name.err"
    if grep -q -- "$want" "$W/$name.err"; then echo "ok   $name"
    else echo "FAIL $name"; tail -3 "$W/$name.err"; FAIL=1; fi
}
# the first connection: a new host key (yes), keyboard-interactive (2 prompts)
check new_host_kbi 'got: hello' $T 127.0.0.1 2223 test -K "$W/known" -a yes -a secret -a green -s 'hello\r' -s 'exit 0\r'
grep -q 'TERM=xterm-256color size=128x48' "$W/new_host_kbi.out" || { echo "FAIL pty TERM / size"; FAIL=1; }
# publickey (a new host on this port: yes)
check pubkey 'Привет' $T 127.0.0.1 2222 test -K "$W/known" -k "$W/user.key" -a yes -s 'utf8\r' -s 'exit 0\r'
# known now: no question
check pubkey_known 'got: k' $T 127.0.0.1 2222 test -K "$W/known" -k "$W/user.key" -s 'k\r' -s 'exit 0\r'
# password, a wrong one first (a new host on this port: yes)
check password 'got: x' $T 127.0.0.1 2224 test -K "$W/known" -a yes -a nope -a secret -s 'x\r' -s 'exit 0\r'
grep -q 'please try again' "$W/password.out" || { echo "FAIL password retry message"; FAIL=1; }
# user name asked
check login_as 'got: y' $T 127.0.0.1 2224 "" -K "$W/known" -a test -a secret -s 'y\r' -s 'exit 0\r'
# 3 MB: window adjust (we give 1 MB), rekeys every 64 KB of the server
check big_rekey 'BIG DONE' $T 127.0.0.1 2224 test -K "$W/known" -a secret -s 'big 3000000\r' -s 'exit 0\r' -t 60
grep -Eq 'kexes ([2-9][0-9]|[0-9]{3,}) strict 1' "$W/big_rekey.err" || { echo "FAIL no rekeys / no strict kex: $(tail -1 "$W/big_rekey.err")"; FAIL=1; }
# the exit status goes through
checkfail exit_status 'status 3' $T 127.0.0.1 2224 test -K "$W/known" -a secret -s 'exit 3\r'
# a changed host key: refused unless "replace"
grep -v ' 2224 ' "$W/known" > "$W/known2"; echo "127.0.0.1 2224 $(printf '%064d' 0)" >> "$W/known2"
checkfail changed_key 'verification failed' $T 127.0.0.1 2224 test -K "$W/known2" -a yes
grep -q 'IDENTIFICATION HAS CHANGED' "$W/changed_key.out" || { echo "FAIL changed key warning"; FAIL=1; }
check changed_replace 'got: z' $T 127.0.0.1 2224 test -K "$W/known2" -a replace -a secret -s 'z\r' -s 'exit 0\r'
# refused new host
checkfail new_host_no 'verification failed' $T 127.0.0.1 2224 test -a no
# Ф5: several sessions at once (a tab each): 4 logins, then 2 x 1 MB with rekeys every 64 KB interleaved
check four_sessions 'got: q' $T 127.0.0.1 2224 test -K "$W/known" -a secret -s 'q\r' -s 'exit 0\r' -n 4
[ "$(grep -c 'status 0)' "$W/four_sessions.err")" = 4 ] || { echo "FAIL four_sessions: $(grep -c 'status 0)' "$W/four_sessions.err") of 4 exited 0"; FAIL=1; }
check two_big 'BIG DONE' $T 127.0.0.1 2224 test -K "$W/known" -a secret -s 'big 1000000\r' -s 'exit 0\r' -n 2 -t 60
[ "$(grep -c 'BIG DONE' "$W/two_big.out")" -ge 2 ] || { echo "FAIL two_big: $(grep -c 'BIG DONE' "$W/two_big.out") of 2 done"; FAIL=1; }
# many handshakes (the mpint of K: a leading zero / high bit in some of them)
N=${N_HANDSHAKES:-100}
bad=0
for i in $(seq "$N"); do
    $T 127.0.0.1 2224 test -K "$W/known" -a secret -s 'exit 0\r' > /dev/null 2> "$W/loop.err" || { bad=$((bad + 1)); cp "$W/loop.err" "$W/loop_fail_$i.err"; }
done
[ $bad -eq 0 ] && echo "ok   $N handshakes" || { echo "FAIL $bad of $N handshakes"; FAIL=1; }
[ $FAIL -eq 0 ] && echo "ALL OK" || echo "SOME FAILED (logs in $W)"
exit $FAIL
