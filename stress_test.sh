#!/bin/bash
# 多用户压力测试脚本
# 用法: ./stress_test.sh [用户数] [服务器端口]

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CLIENT="$SCRIPT_DIR/build/traffic_push_client"
LOG_DIR="$SCRIPT_DIR/logs"

mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR"/*.log

NUM_USERS=${1:-10}
PORT=${2:-9000}

echo "启动 $NUM_USERS 个用户连接到端口 $PORT ..."
echo "日志目录: $LOG_DIR/"

for i in $(seq 1 $NUM_USERS); do
  "$CLIENT" --user "user_$i" --port $PORT --verbose > "$LOG_DIR/user_$i.log" 2>&1 &
  echo "  启动 user_$i (PID: $!) → logs/user_$i.log"
done

echo ""
echo "所有用户已启动，按 Ctrl+C 停止"
echo "查看日志: tail -f logs/user_1.log"
echo ""

# 等待所有后台进程
wait
