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
    return R"geohtml(<!DOCTYPE html><html><head><meta charset="utf-8"><title>RTT Geo Feed</title>
<style>
body{background:#0d1117;color:#d0d7e2;font:13px/1.5 ui-monospace,Menlo,Consolas,monospace;margin:0;padding:18px}
h1{font-size:16px;margin:0 0 4px}.dot{color:#3fb950}
.grid{display:grid;grid-template-columns:300px 1fr;gap:14px;margin-top:10px}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:10px 12px;margin-bottom:12px}
.card h2{font-size:11px;text-transform:uppercase;letter-spacing:.08em;color:#8b949e;margin:0 0 8px}
.k{color:#8b949e}.v{color:#79c0ff}.ok{color:#3fb950}.bad{color:#f85149}
table{width:100%;border-collapse:collapse}td,th{padding:3px 6px;text-align:left;border-bottom:1px solid #21262d}
th{color:#8b949e;font-weight:500}
input,select,button{background:#0d1117;color:#d0d7e2;border:1px solid #30363d;border-radius:6px;padding:5px 8px;font:inherit}
button{cursor:pointer}button:hover{border-color:#3fb950}
#plot{width:100%;height:380px;border-radius:8px;border:1px solid #30363d}
.footer{color:#8b949e;font-size:11px;margin-top:6px}
</style></head><body>
<h1>RTT Engine <span class="dot">●</span> Live Geospatial Feed</h1>
<div class="footer" id="health">connecting…</div>
<div class="grid">
 <div>
  <div class="card"><h2>GPS Fix</h2><div id="fix">—</div></div>
  <div class="card"><h2>Stats</h2><div id="stats">—</div></div>
  <div class="card"><h2>Feeds</h2><div id="feeds">—</div>
   <div style="margin-top:8px"><input id="furl" placeholder="http://host/api/position" style="width:160px">
   <select id="ftype"><option>REST</option><option>WebSocket</option></select>
   <button onclick="addFeed()">Add</button></div></div>
 </div>
 <div><div class="card"><h2>Trajectory · live fix (green) · physics crates (yellow)</h2>
  <canvas id="plot" width="780" height="380"></canvas></div></div>
</div>
<script>
let last=null;
async function tick(){try{
  const r=await fetch('/feed');if(!r.ok)throw 0;const d=await r.json();
  document.getElementById('health').innerHTML='<span class="ok">● live</span> · t='+d.timestamp.toFixed(1)+' s · mode <span class="v">'+d.mode+'</span> · tracking <span class="v">'+d.tracking+'</span>';
  const fix=d.fix||{};
  document.getElementById('fix').innerHTML=(fix.valid?'<span class="ok">fix acquired</span>':'<span class="bad">no fix</span>')+
    '<br>lat <span class="v">'+fix.latitude.toFixed(7)+'</span> · lon <span class="v">'+fix.longitude.toFixed(7)+'</span>'+
    '<br>alt <span class="v">'+fix.altitude.toFixed(1)+' m</span> · speed <span class="v">'+fix.speed_mps.toFixed(2)+' m/s</span>'+
    '<br>heading <span class="v">'+fix.heading_deg.toFixed(1)+'°</span> · acc <span class="v">±'+fix.accuracy_m.toFixed(1)+' m</span> · sats <span class="v">'+fix.satellites+'</span>';
  const st=d.stats||{};
  document.getElementById('stats').innerHTML='<span class="k">points</span> <span class="v">'+st.pointsStored+'</span> · <span class="k">entities</span> <span class="v">'+st.trackedEntities+'</span> · <span class="k">ingestion</span> <span class="v">'+st.ingestionRate+' pts/s</span>';
  const feeds=d.feeds||[];let fh='';
  function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
  feeds.forEach(function(f,i){fh+='<tr><td>'+esc(f.url)+'</td><td>'+esc(f.type)+'</td><td><button onclick="delFeed('+i+')">✕</button></td></tr>';});
  document.getElementById('feeds').innerHTML=feeds.length?('<table>'+fh+'</table>'):'<span class="k">no feeds configured</span>';
  draw(d);last=d;
}catch(e){document.getElementById('health').innerHTML='<span class="bad">● offline</span>';}}
function addFeed(){fetch('/feed',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({url:document.getElementById('furl').value,type:document.getElementById('ftype').value})}).then(tick);}
function delFeed(i){fetch('/feed/remove',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:i})}).then(tick);}
function draw(d){
  const c=document.getElementById('plot'),x=c.getContext('2d');c.width=c.clientWidth||780;c.height=380;
  x.clearRect(0,0,c.width,c.height);
  const pts=(d.trajectory||[]).concat(d.physics||[]);
  if(!pts.length){x.fillStyle='#8b949e';x.fillText('no points yet',12,20);return;}
  let minLat=Infinity,maxLat=-Infinity,minLon=Infinity,maxLon=-Infinity;
  pts.forEach(function(p){minLat=Math.min(minLat,p.lat);maxLat=Math.max(maxLat,p.lat);minLon=Math.min(minLon,p.lon);maxLon=Math.max(maxLon,p.lon);});
  if(maxLat-minLat<1e-7){minLat-=1e-6;maxLat+=1e-6;}if(maxLon-minLon<1e-7){minLon-=1e-6;maxLon+=1e-6;}
  const pad=30,w=c.width-2*pad,h=c.height-2*pad;
  const X=function(lon){return pad+(lon-minLon)/(maxLon-minLon)*w;};
  const Y=function(lat){return c.height-pad-(lat-minLat)/(maxLat-minLat)*h;};
  x.strokeStyle='#21262d';x.beginPath();
  for(let g=0;g<=4;g++){const gx=pad+g*w/4;x.moveTo(gx,pad);x.lineTo(gx,pad+h);}
  for(let g=0;g<=4;g++){const gy=pad+g*h/4;x.moveTo(pad,gy);x.lineTo(pad+w,gy);}x.stroke();
  const traj=d.trajectory||[];
  if(traj.length>1){x.strokeStyle='#388bfd';x.lineWidth=2;x.beginPath();traj.forEach(function(p,i){const px=X(p.lon),py=Y(p.lat);i?x.lineTo(px,py):x.moveTo(px,py);});x.stroke();}
  (d.physics||[]).forEach(function(p){x.fillStyle='#d29922';x.beginPath();x.arc(X(p.lon),Y(p.lat),5,0,7);x.fill();});
  if(d.fix&&d.fix.valid){x.fillStyle='#3fb950';x.beginPath();x.arc(X(d.fix.longitude),Y(d.fix.latitude),6,0,7);x.fill();}
}
setInterval(tick,1000);tick();
</script></body></html>)geohtml";
}

} // namespace geo
