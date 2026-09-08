(() => {
  const stateEl = document.getElementById('status');
  const lightsEl = document.getElementById('lights');
  const inputEl = document.getElementById('light-input');
  const btnEl = document.getElementById('subscribe-btn');

  const wsPort =
    window.__WS_PORT__ ||
    (window.location.port || (window.location.protocol === 'https:' ? 443 : 80));
  const wsScheme = window.location.protocol === 'https:' ? 'wss' : 'ws';
  const wsUrl = `${wsScheme}://${window.location.hostname}:${wsPort}/ws`;

  const lights = new Map();
  let currentIds = new Set();
  let ws = null;
  let reconnectTimer = null;

  const setStatus = (text) => {
    stateEl.textContent = text;
  };

  const render = () => {
    if (lights.size === 0) {
      lightsEl.innerHTML = '';
      lightsEl.style.display = 'none';
      return;
    }
    lightsEl.style.display = 'grid';
    lightsEl.innerHTML = '';
    Array.from(lights.entries())
      .sort((a, b) => a[0] - b[0])
      .forEach(([id, info]) => {
        const remainSec = Math.max(0, Math.round(info.remain_ms / 1000));
        const card = document.createElement('div');
        card.className = `light-card ${info.state}`;
        card.innerHTML = `
          <div class="light-title">#${id}</div>
          <div class="light-state">${info.state.toUpperCase()}</div>
          <div class="light-remain">${remainSec} s</div>
        `;
        lightsEl.appendChild(card);
      });
  };

  const send = (payload) => {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(payload));
    }
  };

  const sendSubscribe = () => {
    const ids = (inputEl.value || '')
      .split(',')
      .map((id) => id.trim())
      .filter(Boolean)
      .map((id) => parseInt(id, 10))
      .filter((num) => !Number.isNaN(num));
    currentIds = new Set(ids);
    lights.forEach((_, id) => {
      if (!currentIds.has(id)) {
        lights.delete(id);
      }
    });
    render();
    send({ action: 'subscribe', lights: ids });
  };

  const connect = () => {
    setStatus(`连接中 ${wsUrl}`);
    ws = new WebSocket(wsUrl);

    ws.onopen = () => {
      setStatus('连接成功');
      send({ action: 'login', user_id: `web-${Date.now()}` });
      sendSubscribe();
    };

    ws.onmessage = (evt) => {
      try {
        const msg = JSON.parse(evt.data);
        if (msg.type === 'light_update') {
          if (currentIds.size > 0 && !currentIds.has(msg.light_id)) {
            return;
          }
          lights.set(msg.light_id, {
            state: msg.state,
            remain_ms: msg.remain_ms,
          });
          render();
        } else if (msg.type === 'subscribe_ack') {
          setStatus(`已订阅 ${msg.count || 0} 个路口`);
        } else if (msg.type === 'ready') {
          setStatus('连接成功，等待推送');
        } else if (msg.type === 'error') {
          setStatus(`错误：${msg.message || '未知'}`);
        }
      } catch (err) {
        console.error('bad message', err);
      }
    };

    ws.onclose = () => {
      setStatus('连接断开，3 秒后自动重试');
      if (!reconnectTimer) {
        reconnectTimer = setTimeout(() => {
          reconnectTimer = null;
          connect();
        }, 3000);
      }
    };
  };

  btnEl.addEventListener('click', () => sendSubscribe());
  connect();
})();

