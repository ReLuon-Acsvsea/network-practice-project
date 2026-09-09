#!/bin/bash
# 并发用户压测脚本
# 用法: ./benchmark.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CLIENT="$SCRIPT_DIR/build/traffic_push_client"
LOG_DIR="$SCRIPT_DIR/logs"

# 测试不同的用户数
for NUM_USERS in 10 50 100 1000 5000 13000; do
  echo ""
  echo "========================================="
  echo "测试 $NUM_USERS 个并发用户"
  echo "========================================="

  # 清理旧进程和日志
  pkill -f traffic_push_client 2>/dev/null
  rm -f "$LOG_DIR"/*.log
  mkdir -p "$LOG_DIR"

  # 启动用户
  for i in $(seq 1 $NUM_USERS); do
    "$CLIENT" --user "user_$i" --verbose > "$LOG_DIR/user_$i.log" 2>&1 &
  done

  echo "已启动 $NUM_USERS 个用户"

  # 等待连接稳定
  sleep 3

  # 检查存活进程数
  ALIVE=$(ps aux | grep traffic_push_client | grep -v grep | wc -l)
  echo "存活进程: $ALIVE / $NUM_USERS"

  # 检查是否收到数据
  if [ -f "$LOG_DIR/user_1.log" ]; then
    UPDATES=$(grep -c "LIGHT_UPDATE" "$LOG_DIR/user_1.log" 2>/dev/null || echo 0)
    echo "user_1 收到更新: $UPDATES 条"
  fi

  # 简单检查服务器响应
  echo "等待5秒观察稳定性..."
  sleep 5

  ALIVE2=$(ps aux | grep traffic_push_client | grep -v grep | wc -l)
  echo "5秒后存活: $ALIVE2 / $NUM_USERS"

  if [ "$ALIVE2" -lt "$NUM_USERS" ]; then
    echo "⚠️  有用户断开连接!"
  else
    echo "✅ 全部存活"
  fi

  # 短暂停顿再测下一轮
  sleep 2
done

echo ""
echo "========================================="
echo "测试完成"
echo "========================================="
