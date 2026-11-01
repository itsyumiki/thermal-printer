const $ = id => document.getElementById(id);
const state = { ws:null, pins:[], pinStatus:{}, connected:false, pending:null, toastTimer:null, powerOn:false, startupRunning:false };
const DANGEROUS = new Set(["PA0","PA1","PA9","PA10","PB8","PB9","PB10","PB11","PC2","PC3"]);
const modeLabels = {hiz:"High impedance",in:"Input",up:"Input · pull-up",dn:"Input · pull-down",lo:"Output LOW",hi:"Output HIGH",off:"Not configured"};
function log(message, type="info") {
  const box=$("log"), row=document.createElement("div"); row.className="log-line "+type;
  const time=document.createElement("span"); time.className="time"; time.textContent=new Date().toLocaleTimeString([], {hour12:false});
  const tag=document.createElement("span"); tag.className="tag"; tag.textContent=type==="command"?"SEND":type==="error"?"ERROR":type==="event"?"EVENT":"INFO";
  row.append(time,tag,document.createTextNode(message)); box.append(row);
  if($("autoScroll").checked) box.scrollTop=box.scrollHeight;
}
function toast(message,error=false){const el=$("toast");el.textContent=message;el.className="toast show"+(error?" error":"");clearTimeout(state.toastTimer);state.toastTimer=setTimeout(()=>el.className="toast",3000)}
function send(obj){if(!state.ws||state.ws.readyState!==WebSocket.OPEN){toast("WebSocket disconnected",true);return}state.ws.send(JSON.stringify(obj))}
function connectSocket(){
  const scheme=location.protocol==="https:"?"wss":"ws"; state.ws=new WebSocket(scheme+"://"+location.host+"/ws");
  $("socketStatus").classList.remove("online");$("socketStatus").lastChild.textContent=" Connecting";
  state.ws.onopen=()=>{$("socketStatus").classList.add("online");$("socketStatus").lastChild.textContent=" Connected";$("railDot").style.background="var(--mint)";$("footerDot").style.background="var(--mint)";log("WebSocket connected")};
  state.ws.onclose=()=>{$("socketStatus").classList.remove("online");$("socketStatus").lastChild.textContent=" Reconnecting";$("railDot").style.background="var(--red)";$("footerDot").style.background="var(--red)";setTimeout(connectSocket,1500)};
  state.ws.onerror=()=>{};
  state.ws.onmessage=e=>{let msg;try{msg=JSON.parse(e.data)}catch{return}handleMessage(msg)};
}
function handleMessage(m){
  if(m.type==="hello"){state.pins=m.pins||[];$("hash").textContent=m.cfg_hash||"—";$("pinCount").textContent=state.pins.filter(p=>!p.denied).length;$("pinCountSub").textContent=state.pins.length+" documented pin entries";renderPins();renderPorts(m.ports||[]);if(m.status)applyStatus(m.status);if(m.mock)log("Mock serial mode enabled","event");return}
  if(m.type==="ports"){renderPorts(m.ports||[]);return}
  if(m.type==="connected"){state.connected=true;applyStatus(m.status);$("connectBtn").classList.add("hidden");$("disconnectBtn").classList.remove("hidden");$("powerBtn").disabled=false;log("Device connected: "+(m.status.port||"serial device"),"event");toast("Connected — running startup power macro…");state.startupRunning=true;send({action:"startup"});return}
  if(m.type==="disconnected"){state.connected=false;state.powerOn=false;applyStatus({connected:false});$("connectBtn").classList.remove("hidden");$("disconnectBtn").classList.add("hidden");$("powerBtn").disabled=true;$("powerBtn").textContent="Power on";$("powerBtn").classList.remove("is-on");log("Device disconnected","event");toast("Disconnected");return}
  if(m.type==="status"){applyStatus(m.status);return}
  if(m.type==="confirm_required"){askConfirm(m,m.mode==="set");return}
  if(m.type==="result"){if(m.reply)log(m.reply.join(" ")||"ok","info");if(m.status)applyStatus(m.status);if(m.action==="startup"||m.action==="power"){state.powerOn=!!(m.status&&m.status.pins||[]).some(p=>p.name==="PA1"&&p.value===1);$("powerBtn").textContent=state.powerOn?"Power off":"Power on";$("powerBtn").classList.toggle("is-on",state.powerOn);if(m.action==="startup"){state.startupRunning=false;toast("PA1 is HIGH — you can let go of the power button.");log("Startup macro complete: PA1 HIGH; PB1 pull-up enabled","event")}else toast(state.powerOn?"Printer power latched ON":"Printer power set LOW");}return}
  if(m.type==="raw"){log(m.command,"command");log((m.reply||[]).join(" ")||"ok");if(m.command.toUpperCase().startsWith("HEL"))send({action:"status"});return}
  if(m.type==="error"){log(m.message||"Unknown backend error","error");toast(m.message||"Operation failed",true);return}
  if(m.type==="event"){log((m.event||"event")+": "+(m.detail||""),"event");return}
}
function renderPorts(ports){const sel=$("portSelect"),old=sel.value;sel.replaceChildren(new Option("Choose serial port…",""));for(const p of ports)sel.add(new Option(p.device+" — "+p.description,p.device));if(old&&[...sel.options].some(o=>o.value===old))sel.value=old}
function applyStatus(s){state.connected=!!s.connected;state.pinStatus={};$("deviceState").textContent=s.connected?"Device connected":"No device connected";$("deviceMeta").textContent=s.connected?(s.port||"Serial")+(s.error?" · "+s.error:""):"Connect a Pinbridge-enabled board to begin";$("connectBtn").classList.toggle("hidden",state.connected);$("disconnectBtn").classList.toggle("hidden",!state.connected);$("uptime").textContent=s.uptime==null?"—":s.uptime+" ticks";(s.pins||[]).forEach(p=>state.pinStatus[p.name]={mode:p.mode,value:p.value});renderPins()}
function modesFor(pin){const c=pin.caps||[],a=[["hiz","High impedance"]];if(c.includes("in"))a.push(["in","Input"]);if(c.includes("pu"))a.push(["up","Pull-up"]);if(c.includes("pd"))a.push(["dn","Pull-down"]);if(c.includes("out"))a.push(["lo","Output LOW"],["hi","Output HIGH"]);return a}
function renderPins(){
 const grid=$("pinGrid"),query=$("search").value.toLowerCase(),group=$("groupFilter").value;
 const pins=state.pins.filter(p=>(!query||(p.name+" "+p.note+" "+(p.alias||"")).toLowerCase().includes(query))&&(group==="ALL"||p.port===group));
 grid.replaceChildren();if(!pins.length){grid.innerHTML='<div class="empty-state">No pins match this filter.</div>';return}
 pins.forEach(p=>{
  const denied=!!p.denied,st=state.pinStatus[p.name]||{},card=document.createElement("article");card.className="pin-card"+(denied?" reserved":p.caps.includes("out")?"":" input-only");card.dataset.pin=p.name;
  const top=document.createElement("div");top.className="pin-top";
  const title=document.createElement("div");title.className="pin-title";
  const avatar=document.createElement("div");avatar.className="pin-avatar";avatar.textContent=p.port==="SYS"?"⊘":p.port+p.bit;
  const titleText=document.createElement("div");const id=document.createElement("div");id.className="pin-id";id.textContent=p.name;const desc=document.createElement("div");desc.className="pin-desc";desc.textContent=p.alias?"Alias · "+p.alias+" · "+p.note:p.note;titleText.append(id,desc);title.append(avatar,titleText);
  const badge=document.createElement("span");badge.className="pin-badge";badge.textContent=denied?"RESERVED":p.caps.includes("out")?"GPIO":"INPUT";top.append(title,badge);card.append(top);
  const meta=document.createElement("div");meta.className="pin-meta";if(p.bit!==null)meta.append(makeSpan("Port "+p.port),makeSpan("Bit "+p.bit));(p.caps||[]).forEach(c=>meta.append(makeSpan(c.toUpperCase())));card.append(meta);
  if(!denied&&state.connected){
   const controls=document.createElement("div");controls.className="pin-controls";const select=document.createElement("select");select.setAttribute("aria-label",p.name+" mode");modesFor(p).forEach(([v,l])=>select.add(new Option(l,v)));select.value=modesFor(p).some(x=>x[0]===st.mode)?st.mode:"hiz";select.onchange=()=>send({action:"mode",pin:p.name,mode:select.value});controls.append(select);
   if(p.caps.includes("out")){const btn=document.createElement("button");btn.className="level"+(st.value===0?" low":"");btn.textContent=st.value===1?"HIGH":st.value===0?"LOW":"SET";btn.title="Toggle output level";btn.onclick=()=>{const val=st.value===1?0:1;if(DANGEROUS.has(p.name))askConfirm({pin:p.name,mode:"set",value:val,message:"Change output level on "+p.name+"?\n"+p.note},true);else send({action:"set",pin:p.name,value:val})};controls.append(btn)}
   const read=document.createElement("button");read.className="button subtle";read.textContent="Read";read.onclick=()=>send({action:"get",pin:p.name});controls.append(read);card.append(controls);
  }else if(!denied){const controls=document.createElement("div");controls.className="pin-controls";const note=document.createElement("span");note.className="muted";note.textContent="Connect to control";controls.append(note);card.append(controls)}
  const stateLine=document.createElement("div");stateLine.className="pin-state";const left=document.createElement("span");left.textContent="CURRENT MODE";const right=document.createElement("b");right.textContent=denied?"UNAVAILABLE":modeLabels[st.mode]||"Not configured";stateLine.append(left,right);card.append(stateLine);
  if(!denied){const valueLine=document.createElement("div");valueLine.className="pin-state pin-value-line";const valueLabel=document.createElement("span");valueLabel.textContent="PIN VALUE";const valueText=document.createElement("b");const value=st.value;valueText.textContent=value===1?"HIGH (1)":value===0?"LOW (0)":"Not read";valueText.classList.toggle("value-low",value===0);valueText.classList.toggle("value-unknown",value!==0&&value!==1);valueLine.append(valueLabel,valueText);card.append(valueLine)}
  if(denied){const d=document.createElement("div");d.className="pin-state";d.textContent=p.note;card.append(d)}
  grid.append(card);
 });
}
function makeSpan(text){const s=document.createElement("span");s.textContent=text;return s}
function askConfirm(m,sendOnApprove=false){state.pending={...m,sendOnApprove};$("confirmTitle").textContent=m.mode==="set"?"Change output on "+m.pin+"?":"Enable output on "+m.pin+"?";$("confirmText").textContent=(m.message||"This pin may affect hardware.")+" Proceed only with the attached loads disconnected."; $("confirmModal").classList.remove("hidden")}
function closeConfirm(){$("confirmModal").classList.add("hidden");state.pending=null}
$("cancelConfirm").onclick=closeConfirm;$("confirmModal").onclick=e=>{if(e.target===$("confirmModal"))closeConfirm()};
$("approveConfirm").onclick=()=>{const m=state.pending;if(!m)return;closeConfirm();if(m.sendOnApprove)send({action:"set",pin:m.pin,value:m.value,confirm:m.pin});else send({action:"mode",pin:m.pin,mode:m.mode,confirm:m.pin})};
$("connectBtn").onclick=()=>send({action:"connect",port:$("portSelect").value});$("disconnectBtn").onclick=()=>send({action:"disconnect"});$("powerBtn").onclick=()=>{if(!state.connected)return;send({action:"power",value:state.powerOn?0:1})};$("refreshPorts").onclick=()=>send({action:"ports"});$("refreshStatus").onclick=()=>send({action:"status"});$("resetPins").onclick=()=>{if(confirm("Put all configured pins into the firmware safe state?"))send({action:"reset"})};$("search").oninput=renderPins;$("groupFilter").onchange=renderPins;$("clearLog").onclick=()=>$("log").replaceChildren();$("logsNav").onclick=()=>$("consolePanel").scrollIntoView({behavior:"smooth"});
$("rawForm").onsubmit=e=>{e.preventDefault();const command=$("rawCommand").value.trim();if(command){send({action:"raw",command});$("rawCommand").value=""}};
connectSocket();fetch("/api/info").then(r=>r.json()).then(d=>{state.pins=d.pins||state.pins;$("hash").textContent=d.cfg_hash||"—";$("pinCount").textContent=state.pins.filter(p=>!p.denied).length;$("pinCountSub").textContent=state.pins.length+" documented pin entries";renderPins();renderPorts(d.ports||[])}).catch(()=>{});
