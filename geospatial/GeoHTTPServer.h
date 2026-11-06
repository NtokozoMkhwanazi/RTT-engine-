#pragma once

/**
 * GeoHTTPServer - minimal localhost status server for the geospatial pipeline.
 *
 * Lets you inspect the LIVE feed from a browser while the engine runs:
 *   http://localhost:8123/       pretty HTML viewer (canvas trajectory plot,
 *                                live fix, stats, feed add/remove)
 *   http://localhost:8123/feed   raw status JSON (what the viewer polls)
 *   http://localhost:8123/health {"status":"ok"}
 *   POST /feed        {"url":"...","type":"REST"}          -> add a feed
 *   POST /feed/remove {"index":N}                          -> remove a feed
 *
 * Implementation notes:
 * - Single-threaded accept loop on a dedicated thread; each request is served
 *   sequentially (fine for a developer-inspection endpoint).
 * - The JSON PayloadProvider / HTML PageProvider are invoked per request on
 *   the server thread. The defaults wired by the app read mutex-protected
 *   caches plus the live GPS fix (plain struct write) - acceptable for a
 *   diagnostic endpoint.
 * - Feed add/remove handlers run on the server thread too; the app's handlers
 *   delegate to mutex-protected feed management.
 * - Binds to 127.0.0.1 only (never exposed on the network). Accepted sockets
 *   get a 3 s recv/send timeout so a silent client can never hang shutdown.
 */

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace geo {

class GeoHTTPServer {
public:
    // Builds the JSON body returned for /feed (and /status). Called on the
    // server thread for each request.
    using PayloadProvider = std::function<std::string()>;

    // Builds the HTML page returned for GET /. Falls back to the built-in
    // viewer when no provider is set.
    using PageProvider = std::function<std::string()>;

    // Handlers for POST /feed (add) and POST /feed/remove. Each returns the
    // JSON response body (e.g. {"ok":true} or {"ok":false,"error":"..."}).
    using FeedAddHandler = std::function<std::string(const std::string& url,
                                                     const std::string& type)>;
    using FeedRemoveHandler = std::function<std::string(size_t index)>;

    // Default port the engine's live status server listens on.
    static constexpr int kDefaultPort = 8123;

    GeoHTTPServer() = default;
    ~GeoHTTPServer() { stop(); }

    GeoHTTPServer(const GeoHTTPServer&) = delete;
    GeoHTTPServer& operator=(const GeoHTTPServer&) = delete;

    /**
     * Bind + listen on 127.0.0.1:port and start the accept thread.
     * Returns false when the port is already in use / bind fails.
     */
    bool start(int port = kDefaultPort, PayloadProvider provider = nullptr) {
        stop();
        port_ = port;
        provider_ = std::move(provider);

        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;

        int yes = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // localhost only
        addr.sin_port = htons(static_cast<uint16_t>(port_));
        if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            listen(listenFd_, 8) < 0) {
            ::close(listenFd_);
            listenFd_ = -1;
            return false;
        }

        running_ = true;
        try {
            thread_ = std::thread([this]() { acceptLoop(); });
        } catch (...) {
            // Thread creation failed - don't leak the listener or leave
            // running_ set (stop() would then try to join a dead thread).
            running_ = false;
            ::close(listenFd_);
            listenFd_ = -1;
            return false;
        }
        return true;
    }

    /** Stop the accept thread and close the socket (idempotent). */
    void stop() {
        if (!running_) return;
        running_ = false;
        if (listenFd_ >= 0) {
            shutdown(listenFd_, SHUT_RDWR);  // wake accept()
            ::close(listenFd_);
            listenFd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    bool isRunning() const { return running_; }
    int port() const { return port_; }

    // --- Optional providers / handlers (wire in the app) ---
    void setPageProvider(PageProvider p) { pageProvider_ = std::move(p); }
    void setFeedHandlers(FeedAddHandler add, FeedRemoveHandler remove) {
        feedAdd_ = std::move(add);
        feedRemove_ = std::move(remove);
    }

    /** Built-in HTML viewer (used when no PageProvider is set). */
    static const char* DefaultPage();

private:
    void acceptLoop() {
        while (running_) {
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            const int fd = accept(listenFd_, reinterpret_cast<sockaddr*>(&client), &len);
            if (fd < 0) {
                if (!running_) break;
                continue;
            }
            handleClient(fd);
            ::close(fd);
        }
    }

    static void SendResponse(int fd, const std::string& body) {
        std::string resp = "HTTP/1.1 200 OK\r\n";
        resp += "Content-Type: application/json\r\n";
        resp += "Access-Control-Allow-Origin: *\r\n";
        resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        resp += "Connection: close\r\n\r\n";
        resp += body;
        send(fd, resp.data(), resp.size(), 0);
    }

    void handleClient(int fd) {
        // Never pin the accept loop (and therefore stop()/the destructor)
        // forever: a client that connects without sending or closing would
        // otherwise hang recv() (and the shutdown join) indefinitely.
        timeval tv{};
        tv.tv_sec = 3;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        char buf[8192];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return;
        buf[n] = '\0';

        // Split headers from body (if any).
        std::string req(buf);
        std::string body;
        const size_t hdrEnd = req.find("\r\n\r\n");
        if (hdrEnd != std::string::npos) {
            body = req.substr(hdrEnd + 4);
            req = req.substr(0, hdrEnd);

            // If a Content-Length is declared and the body didn't arrive in
            // the same recv, pull the remaining bytes.
            const size_t cl = req.find("Content-Length:");
            if (cl != std::string::npos) {
                const long want = std::strtol(req.c_str() + cl + 15, nullptr, 10);
                while (want > 0 && static_cast<long>(body.size()) < want) {
                    const ssize_t more = recv(fd, buf, sizeof(buf) - 1, 0);
                    if (more <= 0) break;
                    buf[more] = '\0';
                    body += buf;
                }
            }
        }

        // Request line: "METHOD /path HTTP/1.1".
        std::string method = "GET";
        std::string path = "/";
        {
            const size_t sp1 = req.find(' ');
            if (sp1 != std::string::npos) {
                method = req.substr(0, sp1);
                const size_t sp2 = req.find(' ', sp1 + 1);
                path = req.substr(sp1 + 1, (sp2 == std::string::npos) ? std::string::npos : sp2 - sp1 - 1);
            }
        }

        if (method == "POST") {
            handlePost(fd, path, body);
            return;
        }

        // GET routes.
        std::string out;
        if (path == "/") {
            out = pageProvider_ ? pageProvider_() : DefaultPage();
            std::string resp = "HTTP/1.1 200 OK\r\n";
            resp += "Content-Type: text/html; charset=utf-8\r\n";
            resp += "Access-Control-Allow-Origin: *\r\n";
            resp += "Content-Length: " + std::to_string(out.size()) + "\r\n";
            resp += "Connection: close\r\n\r\n";
            resp += out;
            send(fd, resp.data(), resp.size(), 0);
            return;
        }
        if (path == "/feed" || path == "/status") {
            out = provider_ ? provider_() : "{}";
        } else if (path == "/health") {
            out = "{\"status\":\"ok\"}";
        } else {
            out = "{\"error\":\"not found\"}";
        }
        SendResponse(fd, out);
    }

    void handlePost(int fd, const std::string& path, const std::string& body) {
        if (path == "/feed") {
            const std::string url = JsonString(body, "url");
            if (url.empty()) {
                SendResponse(fd, "{\"ok\":false,\"error\":\"missing url\"}");
                return;
            }
            const std::string type = JsonString(body, "type");
            const std::string typeResolved = (type == "WebSocket") ? type : "REST";
            if (feedAdd_) {
                SendResponse(fd, feedAdd_(url, typeResolved));
            } else {
                SendResponse(fd, "{\"ok\":false,\"error\":\"no add handler\"}");
            }
            return;
        }
        if (path == "/feed/remove") {
            long index = -1;
            if (!JsonNumber(body, "index", index) || index < 0) {
                SendResponse(fd, "{\"ok\":false,\"error\":\"missing or invalid index\"}");
                return;
            }
            if (feedRemove_) {
                SendResponse(fd, feedRemove_(static_cast<size_t>(index)));
            } else {
                SendResponse(fd, "{\"ok\":false,\"error\":\"no remove handler\"}");
            }
            return;
        }
        SendResponse(fd, "{\"error\":\"not found\"}");
    }

    // --- Minimal JSON field extraction (no parser dependency) ---
    static std::string JsonString(const std::string& body, const char* key) {
        const std::string k = std::string("\"") + key + "\"";
        size_t p = body.find(k);
        if (p == std::string::npos) return "";
        p = body.find('"', p + k.size());
        if (p == std::string::npos) return "";
        const size_t q = body.find('"', p + 1);
        if (q == std::string::npos) return "";
        return body.substr(p + 1, q - p - 1);
    }

    static bool JsonNumber(const std::string& body, const char* key, long& out) {
        const std::string k = std::string("\"") + key + "\"";
        size_t p = body.find(k);
        if (p == std::string::npos) return false;
        p = body.find(':', p + k.size());
        if (p == std::string::npos) return false;
        try {
            out = std::stol(body.substr(p + 1));
            return true;
        } catch (...) {
            return false;
        }
    }

    int listenFd_ = -1;
    int port_ = kDefaultPort;
    std::thread thread_;
    std::atomic<bool> running_{false};
    PayloadProvider provider_;
    PageProvider pageProvider_;
    FeedAddHandler feedAdd_;
    FeedRemoveHandler feedRemove_;
};

inline const char* GeoHTTPServer::DefaultPage() {
    return R"geohtml(<!DOCTYPE html><html><head><meta charset="utf-8"><title>RTT Geo Twin Telemetry</title>
<style>
:root {
    --bg: #060913; --panel: #0d1222; --border: #1e293b;
    --text: #f1f5f9; --text-dim: #94a3b8; --accent: #388bfd;
    --green: #22c55e; --orange: #f97316; --yellow: #eab308; --red: #ef4444;
}
body { background: var(--bg); color: var(--text);
     font-family: system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
     margin: 0; padding: 24px; }
h1 { font-size: 18px; font-weight: 600; margin: 0 0 16px; display: flex; align-items: center;
     gap: 8px; letter-spacing: -0.02em; }
.dot { width: 8px; height: 8px; background: var(--green); border-radius: 50%;
       box-shadow: 0 0 8px var(--green); animation: pulse 2s infinite; }
@keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.4; } }
.grid { display: grid; grid-template-columns: 340px 1fr; gap: 20px; }
.card { background: var(--panel); border: 1px solid var(--border); border-radius: 12px;
        padding: 16px; margin-bottom: 16px; box-shadow: 0 4px 6px -1px rgb(0 0 0 / 0.2); }
.card h2 { font-size: 11px; font-weight: 700; text-transform: uppercase;
           letter-spacing: .08em; color: var(--text-dim); margin: 0 0 12px; }
.metric-row { display: flex; justify-content: space-between; margin-bottom: 8px;
               font-size: 13px; border-bottom: 1px solid #1e293b; padding-bottom: 4px; }
.k { color: var(--text-dim); } .v { color: var(--text); font-weight: 600; }
.badge { display: inline-block; padding: 2px 6px; border-radius: 4px; font-size: 11px; font-weight: 700; }
.badge.ok { background: rgba(34,197,94,0.15); color: var(--green); }
.badge.bad { background: rgba(239,68,68,0.15); color: var(--red); }
table { width: 100%; border-collapse: collapse; margin-top: 4px; }
td { padding: 6px 4px; font-size: 12px; border-bottom: 1px solid var(--border); }
input, select, button { background: var(--bg); color: var(--text); border: 1px solid var(--border);
                          border-radius: 6px; padding: 6px 10px; font-size: 12px; outline: none; }
button { font-weight: 600; background: var(--accent); border: none; cursor: pointer;
         transition: background 0.2s; }
button:hover { background: #2563eb; }
button.danger { background: rgba(239,68,68,0.15); color: var(--red); border: 1px solid rgba(239,68,68,0.3); }
button.danger:hover { background: var(--red); color: var(--text); }
#plot { width: 100%; height: 420px; border-radius: 8px; background: #090d16; }
.footer { font-size: 12px; color: var(--text-dim); margin-bottom: 16px; background: var(--panel);
          display: inline-block; padding: 6px 16px; border-radius: 20px; border: 1px solid var(--border); }
.toast { position: fixed; top: 22px; right: 24px; min-width: 220px; max-width: 360px;
         background: var(--panel); border: 1px solid var(--red); border-radius: 10px;
         padding: 12px 14px; color: var(--text); font-size: 12px;
         box-shadow: 0 6px 18px -4px rgba(239,68,68,.35); opacity: 0;
         transition: opacity .3s; pointer-events: none; z-index: 10; }
.toast.show { opacity: 1; }
.toast .x { color: var(--red); font-weight: 700; }
.alert-row { border-bottom: 1px solid var(--border); padding: 6px 0; font-size: 12px; }
</style></head><body>
<h1><div class="dot"></div> RTT Geo Twin Telemetry &middot; Live Geospatial Feed</h1>
<div class="footer" id="health">Connecting to engine pipeline…</div>
<div id="toast" class="toast"></div>
<div class="grid">
 <div>
  <div class="card"><h2>GPS Core Telemetry</h2><div id="fix">—</div></div>
  <div class="card"><h2>Predicted Next Fix</h2><div id="prediction">—</div></div>
  <div class="card"><h2>Database Stats</h2><div id="stats">—</div></div>
  <div class="card"><h2>Proximity Safety Alerts</h2><div id="alerts">—</div></div>
  <div class="card"><h2>Ingestion Pipelines</h2><div id="feeds">—</div>
   <div style="margin-top:12px; display:flex; gap:6px;">
     <input id="furl" placeholder="Telemetry endpoint URL" style="flex:1;">
     <select id="ftype"><option>REST</option><option>WebSocket</option></select>
     <button onclick="addFeed()">Deploy</button></div></div>
 </div>
 <div><div class="card"><h2>Real-Time Spatial Track (Cars: Blue &middot; Machinery: Yellow &middot; Personnel: Orange)</h2>
  <canvas id="plot" width="780" height="420"></canvas></div></div>
</div>
<script>
// Normalize the (possibly alias-heavy) asset type string coming from a feed
// down to the canonical category the canvas color-mapping keys off.
function classifyType(t){
  const s=(t||'').toLowerCase();
  if(!s||s==='unknown'||s==='digital_twin_telemetry') return 'unknown';
  if(s==='machinery'||s==='heavy'||s==='excavator'||s==='truck'||s==='dragline'||
     s==='loader'||s==='drill') return 'machinery';
  if(s==='human'||s==='personnel'||s==='miner'||s==='worker'||s==='pedestrian'||
     s==='foot') return 'personnel';
  if(s==='car'||s==='vehicle'||s==='light_vehicle'||s==='utility'||s==='bakkie'||
     s==='van') return 'vehicle';
  if(s==='sensor'||s==='static'||s==='monitor'||s==='gas'||s==='geophone'||
     s==='fan') return 'sensor';
  return 'unknown';
}
function assetColor(t){
  const c=classifyType(t);
  if(c==='machinery') return '#eab308';     // Yellow: heavy equipment
  if(c==='personnel') return '#f97316';     // Orange: on-foot crew
  return '#388bfd';                          // Blue: light vehicles / default
}
// Map each canonical asset category to a high-fidelity canvas glyph so the
// 2D dots map to the same iconography used by the 3D viewport assets (Honor
// Pass: telemetry dots -> rich visual icons matching the editor twin mesh).
function assetGlyph(t){
  const c=classifyType(t);
  if(c==='machinery') return '⛟';            // Haul truck / excavator
  if(c==='personnel') return '웃';            // Ground crew
  if(c==='sensor')    return '📡';           // Static IoT node
  if(c==='vehicle')   return '🚗';           // Light utility vehicle
  return '•';                                // Unknown fallback
}
// Convert a '#rrggbb' hex to an 'rgba(r,g,b,a)' string for translucent fills.
function hexRGBA(h, a){
  const r=parseInt(h.slice(1,3),16), g=parseInt(h.slice(3,5),16),
        b=parseInt(h.slice(5,7),16);
  return 'rgba('+r+','+g+','+b+','+a+')';
}
let lastAlertCount=0;
async function tick(){try{
  const r=await fetch('/feed'); if(!r.ok) throw 0; const d=await r.json();
  document.getElementById('health').innerHTML='Status: <span style="color:var(--green)">Online</span> &middot; Mode: <b>'+d.mode+'</b> &middot; Tracked: <b>'+d.tracking+'</b> &middot; Epoch: '+d.timestamp.toFixed(2)+'s';
  const fix=d.fix||{};
  document.getElementById('fix').innerHTML=
    '<div style="margin-bottom:10px;">'+(fix.valid?'<span class="badge ok">FIX ACQUIRED</span>':'<span class="badge bad">NO FIX</span>')+'</div>'+
    '<div class="metric-row"><span class="k">Latitude</span><span class="v">'+fix.latitude.toFixed(7)+'°</span></div>'+
    '<div class="metric-row"><span class="k">Longitude</span><span class="v">'+fix.longitude.toFixed(7)+'°</span></div>'+
    '<div class="metric-row"><span class="k">Altitude</span><span class="v">'+fix.altitude.toFixed(1)+' m</span></div>'+
    '<div class="metric-row"><span class="k">Ground Speed</span><span class="v">'+fix.speed_mps.toFixed(2)+' m/s</span></div>'+
    '<div class="metric-row"><span class="k">True Heading</span><span class="v">'+fix.heading_deg.toFixed(1)+'°</span></div>'+
    '<div class="metric-row"><span class="k">Satellites</span><span class="v">'+fix.satellites+'</span></div>';
  // Predicted next fix (Todo 9/10: 3D Kalman) - a readable summary so a
  // non-technical viewer sees WHAT is predicted (lat/lon/alt/heading/speed +
  // confidence), not just a dot on the canvas.
  const preds=d.predictions||[];
  if(preds.length){
    const nxt=preds[preds.length-1];
    const tPlus=(nxt.timestamp-(d.fix?d.timestamp:0)).toFixed(0);
    document.getElementById('prediction').innerHTML=
      '<div style="margin-bottom:10px;"><span class="badge ok">PREDICTING</span></div>'+
      '<div class="metric-row"><span class="k">Time ahead</span><span class="v">t + '+tPlus+'s</span></div>'+
      '<div class="metric-row"><span class="k">Latitude</span><span class="v">'+nxt.latitude.toFixed(6)+'°</span></div>'+
      '<div class="metric-row"><span class="k">Longitude</span><span class="v">'+nxt.longitude.toFixed(6)+'°</span></div>'+
      '<div class="metric-row"><span class="k">Altitude</span><span class="v">'+nxt.altitude.toFixed(1)+' m</span></div>'+
      '<div class="metric-row"><span class="k">Heading</span><span class="v">'+nxt.heading_deg.toFixed(1)+'°</span></div>'+
      '<div class="metric-row"><span class="k">3D Speed</span><span class="v">'+nxt.speed_mps.toFixed(2)+' m/s</span></div>'+
      '<div class="metric-row"><span class="k">Confidence</span><span class="v">'+(nxt.confidence*100).toFixed(0)+'%</span></div>';
  }else{
    document.getElementById('prediction').innerHTML='<div style="color:var(--text-dim);font-size:12px;">Feed track data to seed a path prediction.</div>';
  }
  const st=d.stats||{};
  document.getElementById('stats').innerHTML=
    '<div class="metric-row"><span class="k">Points Stored</span><span class="v">'+st.pointsStored+'</span></div>'+
    '<div class="metric-row"><span class="k">Tracked Entities</span><span class="v">'+st.trackedEntities+'</span></div>'+
    '<div class="metric-row"><span class="k">Ingestion Velocity</span><span class="v">'+st.ingestionRate+' pts/s</span></div>';
  // Proximity safety alerts (engine-side geofence, surfaced via /feed).
  const al=(d.alerts||[]); let ah='';
  if(al.length){
    al.slice().reverse().forEach(function(a){
      const lv=a.level>=2?'<span class="badge bad">CRITICAL</span>':'<span class="badge ok">WARNING</span>';
      ah+='<div class="alert-row">'+lv+' <b>'+a.message+'</b> <span style="color:var(--text-dim)">'+a.distanceMeters.toFixed(1)+'m</span> <span style="color:var(--yellow)">↗ escape '+a.escapeBearingDeg.toFixed(0)+'°</span></div>';
    });
  }
  document.getElementById('alerts').innerHTML = al.length?ah:'<div style="color:var(--text-dim);font-size:12px;">No active proximity alerts.</div>';
  // Toast on a newly-arrived alert (only grows, never shrinks, in a session).
  if(al.length>lastAlertCount){
    lastAlertCount=al.length;
    const a=al[al.length-1];
    const t=document.getElementById('toast');
    t.innerHTML=(a.level>=2?'<span class="x">CRITICAL</span> ':'')+'Safety alert: '+a.message;
    t.classList.add('show'); setTimeout(function(){t.classList.remove('show');},4500);
  }
  const feeds=d.feeds||[]; let fh='';
  feeds.forEach(function(f,i){
    fh+='<tr><td><span style="color:var(--accent)">'+f.type+'</span></td><td style="word-break:break-all;">'+f.url+'</td><td style="text-align:right;"><button class="danger" onclick="delFeed('+i+')">Remove</button></td></tr>';
  });
  document.getElementById('feeds').innerHTML=feeds.length?('<table>'+fh+'</table>'):'<div style="color:var(--text-dim);font-size:12px;">No background feeds deployed.</div>';
  draw(d);
}catch(e){
  lastAlertCount=0;
  document.getElementById('toast').classList.remove('show');
  document.getElementById('health').innerHTML='<span style="color:var(--red)">● Offline (Engine Disconnected)</span>'
  ;}}
function addFeed(){
  fetch('/feed',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({url:document.getElementById('furl').value,type:document.getElementById('ftype').value})})
  .then(tick);
}
function delFeed(i){
  fetch('/feed/remove',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({index:i})}).then(tick);
}
function draw(d){
  const c=document.getElementById('plot'),x=c.getContext('2d'); c.width=c.clientWidth; c.height=420;
  x.clearRect(0,0,c.width,c.height);
  const traj=d.trajectory||[]; const phys=d.physics||[]; const preds=d.predictions||[];
  const pts=traj.concat(phys);
  if(!pts.length && !preds.length){
    x.fillStyle='#94a3b8'; x.font='12px system-ui'; x.fillText('Waiting for sensor data…',12,20); return;
  }
  let minLat=Infinity,maxLat=-Infinity,minLon=Infinity,maxLon=-Infinity;
  pts.forEach(function(p){minLat=Math.min(minLat,p.lat);maxLat=Math.max(maxLat,p.lat);
                          minLon=Math.min(minLon,p.lon);maxLon=Math.max(maxLon,p.lon);});
  preds.forEach(function(p){minLat=Math.min(minLat,p.latitude);maxLat=Math.max(maxLat,p.latitude);
                          minLon=Math.min(minLon,p.longitude);maxLon=Math.max(maxLon,p.longitude);});
  if(maxLat-minLat<1e-7){minLat-=1e-5;maxLat+=1e-5;} if(maxLon-minLon<1e-7){minLon-=1e-5;maxLon+=1e-5;}
  const pad=40,w=c.width-2*pad,h=c.height-2*pad;
  const X=function(lon){return pad+(lon-minLon)/(maxLon-minLon)*w;};
  const Y=function(lat){return c.height-pad-(lat-minLat)/(maxLat-minLat)*h;};
  const degToM=111000.0;
  // Anti-aliased grid
  x.strokeStyle='#1e293b'; x.lineWidth=1; x.beginPath();
  for(let g=0;g<=4;g++){x.moveTo(pad+g*w/4,pad);x.lineTo(pad+g*w/4,pad+h);x.moveTo(pad,pad+g*h/4);x.lineTo(pad+w,pad+g*h/4);} x.stroke();
  // Observed walker path - labelled so it's clear this is history, not a guess.
  x.strokeStyle='#388bfd'; x.fillStyle='#388bfd'; x.lineWidth=2; x.beginPath();
  traj.forEach(function(p,i){const px=X(p.lon),py=Y(p.lat);i?x.lineTo(px,py):x.moveTo(px,py);}); x.stroke();
  if(traj.length){x.font='11px system-ui'; x.fillText('Observed path ('+traj.length+' pts)',X(traj[0].lon)+6,Y(traj[0].lat)-6);}
  // Other tracked twins, colour-coded by kind.
  phys.forEach(function(p){
    const g=assetGlyph(p.type);
    x.fillStyle=assetColor(p.type);
    // Rich glyph icon centred on the twin's fix (Honor Pass) instead of a flat
    // dot, so the web plot mirrors the editor viewport asset icons.
    x.font="bold 14px system-ui"; x.textAlign="center"; x.textBaseline="middle";
    x.fillText(g, X(p.lon), Y(p.lat));
    // Per-twin prediction cone (each twin predicts its OWN future). Static/unfed
    // twins have no observations -> no cone. Moving twins get a leading cone.
    if(p.hasPrediction && (p.predictions||[]).length){
      const tc=assetColor(p.type);
      x.strokeStyle=tc; x.fillStyle=hexRGBA(tc,0.18); x.lineWidth=2;
      const pp=p.predictions;
      x.beginPath();
      x.moveTo(X(pp[0].lon),Y(pp[0].lat));
      for(let i=0;i<pp.length;i++){x.lineTo(X(pp[i].lon),Y(pp[i].lat));}
      for(let i=pp.length-1;i>=0;i--){
        const r=(pp[i].uncertaintyRadius||0)/degToM; const asp=Math.cos(pp[i].lat*Math.PI/180);
        x.lineTo(X(pp[i].lon-r/asp),Y(pp[i].lat-r));
      }
      x.closePath(); x.fill(); x.stroke();
      const nxt=pp[pp.length-1];
      x.fillStyle=tc; x.font='10px system-ui';
      x.fillText((p.type||'twin')+' predicting (conf '+(nxt.confidence*100).toFixed(0)+'%)',X(nxt.lon)+6,Y(nxt.lat)-8);
    }
  });
  // 3D Kalman prediction cone (Todo 9/10): a shaded uncertainty band + predicted
  // path + a labelled next-fix marker, so the "dot" is explained, not mystery.
  if(preds.length){
    x.strokeStyle='#a78bfa'; x.fillStyle='rgba(167,139,250,0.22)'; x.lineWidth=2;
    // Uncertainty band: trace the path forward, then return along -uncertainty.
    x.beginPath();
    x.moveTo(X(preds[0].longitude),Y(preds[0].latitude));
    for(let i=0;i<preds.length;i++){x.lineTo(X(preds[i].longitude),Y(preds[i].latitude));}
    for(let i=preds.length-1;i>=0;i--){
      const r=(preds[i].uncertaintyRadius||0)/degToM; const asp=Math.cos(preds[i].latitude*Math.PI/180);
      x.lineTo(X(preds[i].longitude-r/asp),Y(preds[i].latitude-r));
    }
    x.closePath(); x.fill(); x.stroke();
    // Sampled predicted waypoints.
    x.fillStyle='#a78bfa';
    for(let i=0;i<preds.length;i+=Math.max(1,Math.floor(preds.length/8))){
      x.beginPath(); x.arc(X(preds[i].longitude),Y(preds[i].latitude),3.5,0,7); x.fill();
    }
    const nxt=preds[preds.length-1];
    x.fillStyle='#a78bfa'; x.font='11px system-ui';
    x.fillText('Predicted next fix (t+'+nxt.timestamp.toFixed(0)+'s, conf '+(nxt.confidence*100).toFixed(0)+'%)',X(nxt.longitude)+6,Y(nxt.latitude)-8);
  }
  // Current live position - labelled "You are here" with a speed-scaled heading.
  if(d.fix&&d.fix.valid){
    const fx=d.fix; const px=X(fx.longitude),py=Y(fx.latitude);
    x.fillStyle='#22c55e'; x.strokeStyle='white'; x.lineWidth=1.5;
    x.beginPath(); x.arc(px,py,7,0,7); x.fill(); x.stroke();
    x.fillStyle='#22c55e'; x.font='11px system-ui'; x.fillText('You are here',px+8,py-10);
    // Heading arrow - length encodes ground speed so faster = longer arrow.
    const ang=(fx.heading_deg-90)*Math.PI/180, len=7+fx.speed_mps*2.5;
    x.strokeStyle='#22c55e'; x.lineWidth=2; x.beginPath();
    x.moveTo(px,py); x.lineTo(px+len*Math.cos(ang),py+len*Math.sin(ang)); x.stroke();
    x.fillStyle='#22c55e'; x.font='10px system-ui'; x.fillText(fx.speed_mps.toFixed(1)+' m/s',px+8,py+18);
  }
  // Legend (so the colours/symbols are self-explanatory for non-technical users).
  x.fillStyle='#94a3b8'; x.font='11px system-ui';
  let lx=pad;
  x.fillText('Legend:',lx,16);
  x.fillStyle='#388bfd'; x.fillRect(lx+44,7,10,4); x.fillText('observed path',lx+58,16);
  x.strokeStyle='#a78bfa'; x.fillStyle='#a78bfa'; x.fillRect(lx+180,7,10,4); x.fillText('prediction cone',lx+194,16);
  x.fillStyle='#22c55e'; x.beginPath(); x.arc(lx+310,11,4,0,7); x.fill(); x.fillText('current position',lx+320,16);
}
setInterval(tick,1000); tick();
</script></body></html>)geohtml";
}

} // namespace geo
