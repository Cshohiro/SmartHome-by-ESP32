function toast(msg){
  const el = document.getElementById('toast');
  if (!el) return;
  el.innerText = msg;
  setTimeout(()=>{ if(el.innerText === msg) el.innerText=''; }, 2200);
}

async function httpText(url){
  const r = await fetch(url, { cache: 'no-store' });
  const t = await r.text();
  return { ok: r.ok, text: t, status: r.status };
}

async function httpJson(url){
  const r = await fetch(url, { cache: 'no-store' });
  const j = await r.json();
  return { ok: r.ok, json: j, status: r.status };
}

// ===== AC =====
async function ac_api(params){
  const url = '/api/ac3?' + new URLSearchParams(params).toString();
  const msg = document.getElementById('msg');
  if (msg) msg.innerText = '状态：发送中...';

  const r = await httpText(url);
  if (msg) msg.innerText = '状态：' + r.text;

  await ac_refresh();
}

async function ac_refresh(){
  const stateEl = document.getElementById('state');
  const tempEl  = document.getElementById('temp');
  const leftEl  = document.getElementById('left');

  if (!stateEl && !tempEl && !leftEl) return; // 不在空调页就不刷

  try{
    const r = await httpJson('/api/ac3_state');
    const j = r.json;

    if (tempEl) tempEl.innerText = j.temp;
    if (stateEl){
      stateEl.innerText =
        '当前：' + (j.power ? '开机':'关机') +
        ' / ' + (j.mode === 'heat' ? '暖房':'冷房') +
        ' / 风量:' + j.fan;
    }
    if (leftEl) leftEl.innerText = j.timer_left;
  }catch(e){
    if (stateEl) stateEl.innerText = '状态读取失败';
  }
}

function ac_mode(m){ ac_api({mode:m}); }
function ac_tempStep(d){ ac_api({tempStep:d}); }
function ac_fan(f){ ac_api({fan:f}); }
function ac_powerOff(){ ac_api({power:0}); }
function ac_setTimer(){
  const v = document.getElementById('mins')?.value ?? '';
  ac_api({timerMin:v});
}
function ac_cancelTimer(){ ac_api({timerCancel:1}); }

// 空调页定时刷新
setInterval(ac_refresh, 1000);
ac_refresh();

// ===== Light =====
async function light_cmd(cmd){
  const msg = document.getElementById('msg');
  if (msg) msg.innerText = '状态：发送中...';

  const r = await httpText('/api/light?cmd=' + encodeURIComponent(cmd));
  if (msg) msg.innerText = '状态：' + r.text;
}

// export for onclick
window.toast = toast;

window.ac_mode = ac_mode;
window.ac_tempStep = ac_tempStep;
window.ac_fan = ac_fan;
window.ac_powerOff = ac_powerOff;
window.ac_setTimer = ac_setTimer;
window.ac_cancelTimer = ac_cancelTimer;

window.light_cmd = light_cmd;
