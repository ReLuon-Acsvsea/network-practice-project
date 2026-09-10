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

BATCH=50
for i in $(seq 1 $NUM_USERS); do
  # 每个客户端加 0~50ms 随机延迟，错开 connect 时间
  ( sleep "$(awk "BEGIN{srand($i); printf \"%.2f\", rand()*0.05}")"; exec "$CLIENT" --user "user_$i" --port $PORT --verbose ) > "$LOG_DIR/user_$i.log" 2>&1 &
  if (( i % BATCH == 0 )); then
    echo "  已启动 $i 个用户"
    sleep 0.2
  fi
done
echo "  全部 $NUM_USERS 个用户已启动"

echo ""
echo "等待 60 秒收集数据..."
sleep 60

echo "统计结果..."
echo ""

# 连接成功率
total=$(ls "$LOG_DIR"/*.log 2>/dev/null | wc -l)
success=$(for f in "$LOG_DIR"/*.log; do sed -n '2p' "$f"; done | grep -c "Connected.")
fail=$((total - success))
echo "=== 连接成功率 ==="
echo "总用户: $total"
echo "成功: $success ($(awk "BEGIN{printf \"%.1f\", $success/$total*100}")%)"
echo "失败: $fail"

# 推送到达率
received=0
for f in "$LOG_DIR"/*.log; do
  grep -q "LIGHT_UPDATE" "$f" 2>/dev/null && received=$((received+1))
done
echo ""
echo "=== 推送到达率 ==="
echo "收到推送: $received / $success ($(awk "BEGIN{printf \"%.1f\", $received/($success>0?$success:1)*100}")%)"

# 清理
pkill -f traffic_push_client 2>/dev/null
echo ""
echo "测试完成，客户端已清理"
