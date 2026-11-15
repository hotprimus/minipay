#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PORT=${PORT:-18583}
CLI="./bin/minipay-cli -p $PORT"

./bin/minipay-server -p "$PORT" -f data/cards.csv -t 4 2> server.log &
SERVER_PID=$!
trap 'kill -INT $SERVER_PID 2>/dev/null || true' EXIT
sleep 0.5

step() { printf '\n\033[1m$ %s\033[0m\n' "$*"; "$@" || true; }

DT=1115120000

step $CLI echo
step $CLI balance 4111111111111111 2812
step $CLI -s 100001 -d $DT auth 4111111111111111 2812 250000
step $CLI -s 100001 -d $DT auth 4111111111111111 2812 250000
step $CLI -s 100002 auth 5555555555554444 2706 9999999
step $CLI -s 100003 auth 4111111111111112 2812 100
step $CLI -s 100004 auth 378282246310005 2701 100
step $CLI -s 100005 auth 4012888888881881 2312 100
step $CLI balance 4111111111111111 2812
step $CLI -s 100006 reverse 4111111111111111 250000 100001 $DT
step $CLI -s 100007 reverse 4111111111111111 250000 100001 $DT
step $CLI balance 4111111111111111 2812

kill -INT $SERVER_PID
wait $SERVER_PID || true
trap - EXIT
printf '\n--- server.log ---\n'
cat server.log
