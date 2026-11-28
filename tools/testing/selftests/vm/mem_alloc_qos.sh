#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu

qos=${QOS_DEBUGFS:-/sys/kernel/debug/mem_alloc_qos}
tracefs=${QOS_TRACEFS:-/sys/kernel/tracing}
worker="$(dirname "$0")/mem_alloc_qos_worker"

if [ ! -w "$qos/control" ] || [ ! -d "$tracefs/instances" ] ||
   ! grep -q 'state=OFF ' "$qos/status"; then
	echo "SKIP: need a stopped QoS kernel, debugfs, tracefs and root"
	exit 4
fi

tmp=$(mktemp -d)
trace="$tracefs/instances/mem-alloc-qos-$$"

control()
{
	printf '%s\n' "$*" > "$qos/control"
}

cleanup()
{
	control stop
	control test fail_after -1 2>/dev/null || :
	[ -z "${top:-}" ] || kill "$top" 2>/dev/null || :
	[ -z "${fgs:-}" ] || kill "$fgs" 2>/dev/null || :
	rmdir "$trace" 2>/dev/null || :
	rm -rf "$tmp"
}
trap cleanup EXIT

fail()
{
	echo "FAIL: $*"
	cat "$qos/status"
	exit 1
}

expect()
{
	grep -q "$1" "$qos/status" || fail "$1"
}

await_line()
{
	for count in $(seq 1 300); do
		if grep -q "$2" "$1"; then
			return
		fi
		sleep 0.01
	done
	fail "missing $2 in $1"
}

submit()
{
	if [ "$1" = 0 ]; then
		printf '%s %s\n' "$2" "$3" >&3
	else
		printf '%s %s\n' "$2" "$3" >&4
	fi
}

stop()
{
	control stop
	expect 'state=OFF '
	expect 'F=0 G=0 R=0 retiring=0 .* balanced=1'
	expect 'queue top=0 fgs=0 live=0'
	expect 'permits_pending=0'
}

# mode domain weights pool settings queue settings trace run_id role_tgids
setup()
{
	control config "$@" 2 "$run" "$top" "$fgs"
	control start
}

grants()
{
	awk -v run="$run" '
		$0 ~ ("run=" run " ") && /phase=grant/ {
			for (i = 1; i <= NF; i++)
				if ($i ~ /^role=/) printf "%s", substr($i, 6)
		}' "$trace/trace"
}

mkdir "$trace"
echo mono > "$trace/trace_clock"
echo 1 > "$trace/events/mem_alloc_qos/enable"
mkfifo "$tmp/in0" "$tmp/in1"
exec 3<> "$tmp/in0"
exec 4<> "$tmp/in1"
( exec "$worker" "$qos/alloc" < "$tmp/in0" > "$tmp/out0" 3>&- 4>&- ) &
top=$!
( exec "$worker" "$qos/alloc" < "$tmp/in1" > "$tmp/out1" 3>&- 4>&- ) &
fgs=$!

run=10
control test fail_after 2
if setup 3 0 3 1 8 8 4 4 4 1000 3 16 1000000 1 2>/dev/null; then
	fail 'partial initialization succeeded'
fi
expect 'initialized=2 .* start_error=-12'
stop
control test fail_after -1
if control config 3 0 3 1 8 8 8 4 4 1000 3 16 1000000 1 2 99 "$top" "$fgs" \
	2>/dev/null; then
	fail 'accepted a low watermark equal to target'
fi
expect 'config_error=low'
expect 'staged_config .* run_id=10 '

run=11
setup 0 0 0 0 0 0 0 0 0 0 0 0 0 0
if control config 2>/dev/null; then
	fail 'changed an active session'
fi
expect 'config_error=busy'
submit 0 auto 11
submit 1 auto 12
await_line "$tmp/out0" '^11=0$'
await_line "$tmp/out1" '^12=0$'
expect 'role=0 .* fast_ok=1'
stop

run=12
setup 1 0 0 0 4 4 2 2 2 1000 3 0 0 0
submit 0 force 13
await_line "$tmp/out0" '^13=0$'
expect 'role=0 .* pool_ok=1'
submit 1 auto 14
await_line "$tmp/out1" '^14=0$'
expect 'role=1 .* claimed=0 .*fast_ok=1 .*pool_ok=0'
stop

run=15
setup 1 0 0 0 8 1024 4 4 4 1000 3 0 0 0
control target 256
await_line "$qos/status" 'pool F=256 G=0 R=0'
expect 'pool_target pages=256 max=1024'
control target 2
await_line "$qos/status" 'pool F=2 G=0 R=0 retiring=0'
stop

run=16
setup 1 0 0 0 2 8 1 4 4 1000 3 0 0 0
control test fail_after 0
for i in 161 162; do
	submit 0 force "$i"
	await_line "$tmp/out0" "^$i=0$"
done
control test reclaim_direct 1
expect 'pool F=0 G=0 R=0 retiring=0'
expect 'kswapd=0 direct=0 memcg=0'
control test reclaim_direct 2
expect 'pool F=1 G=0 R=0 retiring=0'
expect 'kswapd=0 direct=1 memcg=0'
control test reclaim_memcg 2
expect 'pool F=2 G=0 R=0 retiring=0'
expect 'kswapd=0 direct=1 memcg=1'
for i in 163 164; do
	submit 0 force "$i"
	await_line "$tmp/out0" "^$i=0$"
done
stop
control test fail_after -1

run=17
setup 1 0 0 0 2 16 1 4 4 1000 3 0 0 0
control auto 1 16
if control target 8 2>/dev/null; then
	fail 'manual resize was accepted with auto enabled'
fi
control test fail_after 0
for i in 171 172 173; do
	submit 0 force "$i"
	await_line "$tmp/out0" "^$i=0$"
done
await_line "$qos/status" 'pool_target pages=4'
await_line "$qos/status" 'pool_target pages=2'
expect 'auto enabled=1 frozen=0 min=1 max=16 grows=1 shrinks=1'
control auto off
control target 8
expect 'auto enabled=0'
expect 'pool_target pages=8'
stop
control test fail_after -1

run=18
setup 3 0 3 1 4 8 2 4 4 1000 3 16 1000000 1
control test manual 1
control test hold 0
submit 0 force 181
await_line "$qos/status" 'queue top=1 fgs=0'
submit 0 force 182
await_line "$qos/status" 'queue top=2 fgs=0'
control test step
control test step
expect 'pool F=2 G=2 R=0 '
control target 1
await_line "$qos/status" 'pool F=0 G=2 R=0 retiring=0'
generation=$(sed -n 's/.* state=ACTIVE session=\([0-9]*\)$/\1/p' "$qos/status")
id=$(sed -n 's/.*run=18 .* id=\([0-9]*\) parent=181 .*phase=grant .*/\1/p' "$trace/trace")
[ -n "$generation" ] && [ -n "$id" ] || fail 'missing page grant during resize'
control cancel "$generation" "$id"
await_line "$tmp/out0" '^181=125$'
await_line "$qos/status" 'pool F=0 G=1 R=0 retiring=0'
control test hold -1
await_line "$tmp/out0" '^182=0$'
await_line "$qos/status" 'pool F=1 G=0 R=0 retiring=0'
stop

run=20
setup 2 0 0 0 8 8 4 4 4 1000 3 16 1000000 1
control test manual 1
submit 1 auto 21
await_line "$qos/status" 'queue top=0 fgs=1'
submit 0 force 22
await_line "$qos/status" 'queue top=1 fgs=1'
control test step
control test step
[ "$(grants)" = 10 ] || fail 'global FIFO order'
await_line "$tmp/out1" '^21=0$'
await_line "$tmp/out0" '^22=0$'
expect 'role=1 .* claimed=0 .*permitted_ok=1'
stop

for weight in 1 2 3 4; do
	run=$((30 + weight))
	setup 3 0 "$weight" 1 8 8 4 4 4 1000 3 16 1000000 1
	control test manual 1
	nr=$((weight + 1))
	for i in $(seq 1 "$nr"); do
		submit 0 force "$((100 * weight + i))"
		await_line "$qos/status" "queue top=$i fgs=0"
	done
	submit 1 auto "$((100 * weight + 90))"
	await_line "$qos/status" "queue top=$nr fgs=1"
	expected=
	for i in $(seq 1 "$weight"); do
		control test step
		expected=${expected}0
	done
	control test step
	[ "$(grants)" = "${expected}1" ] || fail "WRR weight $weight"
	control test manual 0
	await_line "$tmp/out0" "^$((100 * weight + nr))=0$"
	await_line "$tmp/out1" "^$((100 * weight + 90))=0$"
	expect 'cursor=0'
	stop
done

run=40
setup 3 0 3 1 2 2 1 2 2 1000 3 16 1000000 1
control test manual 1
control test fail_after 0
for i in 1 2 3 4; do
	submit 0 force "$((500 + i))"
	await_line "$qos/status" "queue top=$i fgs=0"
done
submit 1 auto 590
await_line "$qos/status" 'queue top=4 fgs=1'
control test step
control test step
control test step
[ "$(grants)" = 001 ] || fail 'empty pool did not allow a background permit'
await_line "$tmp/out1" '^590=0$'
await_line "$qos/status" 'paused=1 pending=0'
expect 'cursor=2'
control test reclaim 3
await_line "$qos/status" 'pool F=2 G=0 R=0'
expect 'refill_sources reclaim=2 ordinary=0'
submit 1 auto 591
await_line "$qos/status" 'queue top=2 fgs=1'
control test step
control test step
[ "$(grants)" = 00101 ] || fail 'reclaim refill reset the WRR cursor'
await_line "$tmp/out1" '^591=0$'
control test manual 0
await_line "$tmp/out0" '^504=0$'
stop
control test fail_after -1

run=50
setup 3 0 3 1 8 8 4 4 4 1000 3 16 1000000 1
control test manual 1
control test hold 0
submit 0 force 601
await_line "$qos/status" 'queue top=1 fgs=0'
submit 1 force 691
await_line "$qos/status" 'queue top=1 fgs=1'
control test step
control test step
await_line "$tmp/out1" '^691=0$'
expect 'pool F=.* G=1 '
generation=$(sed -n 's/.* state=ACTIVE session=\([0-9]*\)$/\1/p' "$qos/status")
id=$(sed -n 's/.*run=50 .* id=\([0-9]*\) parent=601 .*phase=grant .*/\1/p' "$trace/trace")
[ -n "$generation" ] && [ -n "$id" ] || fail 'missing granted request'
control cancel "$generation" "$id"
await_line "$tmp/out0" '^601=125$'
expect 'role=0 .* revoked=1'
submit 0 force 602
await_line "$qos/status" 'queue top=1 fgs=0'
control test step
submit 1 force 692
await_line "$qos/status" 'queue top=0 fgs=1'
stop
await_line "$tmp/out0" '^602=0$'
await_line "$tmp/out1" '^692=0$'
expect 'stop=2'

run=51
setup 3 0 3 1 8 8 4 4 4 1000 3 16 10000 1
control test manual 1
submit 0 force 701
await_line "$tmp/out0" '^701=0$'
expect 'expired=1'
expect 'role=0 .* slow_ok=1'
submit 1 auto 791
await_line "$tmp/out1" '^791=0$'
expect '^fgs_bypass .* expired=1 '
expect 'role=1 .*fgs_bypass_ok=1'
stop

run=52
setup 3 0 3 1 8 8 4 4 4 1000 3 1 1000000 1
control test manual 1
submit 1 auto 711
await_line "$qos/status" 'queue top=0 fgs=1'
submit 1 auto 712
await_line "$tmp/out1" '^712=0$'
expect '^fgs_bypass .* full=1 '
expect 'role=1 .*fgs_bypass_ok=1'
control test step
await_line "$tmp/out1" '^711=0$'
expect 'role=1 .*permitted_ok=1 .*fgs_bypass_ok=1'
expect 'pool F=8 G=0 R=0 .* delivered=0 '
stop

run=53
setup 3 0 3 1 8 8 4 4 4 1000 3 16 1000000 1
control test manual 1
control test hold 1
submit 1 force 721
await_line "$qos/status" 'queue top=0 fgs=1'
control test step
expect 'permits_pending=1 permits_used=0'
generation=$(sed -n 's/.* state=ACTIVE session=\([0-9]*\)$/\1/p' "$qos/status")
id=$(sed -n 's/.*run=53 .* id=\([0-9]*\) parent=721 .*phase=grant_permit .*/\1/p' "$trace/trace")
[ -n "$generation" ] && [ -n "$id" ] || fail 'missing background permit'
control cancel "$generation" "$id"
await_line "$tmp/out1" '^721=125$'
expect 'permits_pending=0 permits_used=0'
expect '^fgs_bypass .* explicit=0 '
submit 1 force 722
await_line "$qos/status" 'queue top=0 fgs=1'
control test step
expect 'permits_pending=1 permits_used=0'
stop
await_line "$tmp/out1" '^722=0$'
expect '^fgs_bypass .* stop=1 '
expect 'role=1 .*fgs_bypass_ok=1'

run=60
setup 3 1 3 1 8 8 4 4 4 1000 3 16 250 1
submit 0 anon 801
submit 1 anon 891
await_line "$tmp/out0" '^801=0$'
await_line "$tmp/out1" '^891=0$'
expect 'role=0 .* fast_ok=[1-9]'
expect 'role=1 .*permitted_ok=[1-9]'
stop

# Verify exclusive page ownership for TOP and permit ownership for FGS.
awk '
	/phase=(grant|claim|revoke)/ {
		for (i = 1; i <= NF; i++) {
			split($i, kv, "=")
			v[kv[1]] = kv[2]
		}
		page = v["session"] ":" v["page"]
		if (v["phase"] ~ /_permit$/) {
			key = v["session"] ":" v["id"]
			if (v["role"] != 1 || v["page"] != 0)
				exit 1
			if (v["phase"] == "grant_permit") {
				if (permit[key])
					exit 1
				permit[key] = v["ordinal"]
			} else {
				if (permit[key] != v["ordinal"])
					exit 1
				delete permit[key]
				permits++
			}
			next
		}
		if (v["role"] != 0 || v["page"] == 0)
			exit 1
		if (v["phase"] == "grant") {
			if (owner[page] || consumed[page])
				exit 1
			owner[page] = v["id"]
			grants++
		} else {
			if (!owner[page] || owner[page] != v["id"])
				exit 1
			delete owner[page]
			if (v["phase"] == "claim") {
				consumed[page] = 1
				claims++
			}
		}
	}
	END {
		if (!grants || !claims || !permits)
			exit 1
		for (page in owner)
			exit 1
		for (key in permit)
			exit 1
	}
' "$trace/trace" || fail 'trace page ownership'
echo 'PASS: TOP pool, FGS permits, resize, WRR/FIFO, reclaim, bypass, cancel, stop, anon'
