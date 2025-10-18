/*
 * bmw_mqtt_bridge.cpp
 *
 * Bridge BMW CarData MQTT → Local Mosquitto
 * Copyright (c) 2025 Kurt, DJ0ABR
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


// bmw_mqtt_bridge.cpp
//
// Purpose:
//   Bridge BMW CarData Streaming MQTT → local Mosquitto (republish as bmw/<VIN>/...)
//   Uses: libmosquitto (MQTT v5), libcurl (not used at runtime here), nlohmann/json (header-only)
//
// Features:
//   - MQTT v5 with reason codes
//   - Token expiry tracking via JWT "exp" claim
//   - Soft/Hard token refresh via HTTP refresh (no external script required at runtime)
//   - Connect watchdog + client rebuild
//   - Backoff (incl. jitter) to avoid quota/rate-limit storms
//   - LWT on local broker + status topic
//
// Build (Debian/Ubuntu):
//   g++ -std=c++17 -O2 -Wall -Wextra -pthread bmw_mqtt_bridge.cpp \
//       $(pkg-config --cflags --libs libmosquitto) -lcurl
//
// Runtime configuration (env overrides):
//   CLIENT_ID        : BMW CarData client ID (GUID)              (required; no default)
//   GCID             : BMW GCID / username for the MQTT broker   (required; no default)
//   BMW_HOST         : customer.streaming-cardata.bmwgroup.com   (default: set)
//   BMW_PORT         : 9000                                      (default: 9000)
//   LOCAL_HOST       : 127.0.0.1                                 (default: 127.0.0.1)
//   LOCAL_PORT       : 1883                                      (default: 1883)
//   LOCAL_PREFIX     : bmw/                                      (default: bmw/)
//   LOCAL_USER       : (optional)
//   LOCAL_PASSWORD   : (optional)
//
// Token / .env location (fixed):
//   XDG:  $XDG_STATE_HOME/bmw-mqtt-bridge/.env
//   Fallback: $HOME/.local/state/bmw-mqtt-bridge/.env
//   Token files are expected in the same directory:
//     id_token.txt, refresh_token.txt, access_token.txt
//
// Notes:
//   - id_token (a JWT) is used as the MQTT password; we parse its 'exp' to know validity.
//   - Files written by this program use permissions 0644.
//
// ------------------------------------------------------------------------

#include <mosquitto.h>
#include <curl/curl.h>
#include <random>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <unistd.h>     // access()
#include <sys/wait.h>   // WIFEXITED, WEXITSTATUS
#include <ctime>
#include <filesystem>
#include <sys/stat.h>
#include <fcntl.h>
#include <regex>

#ifndef NLOHMANN_JSON_HPP
  #include "json.hpp" // nlohmann/json header (json.hpp next to this file)
#endif
using json = nlohmann::json;

static bool refresh_tokens();
static mosquitto* create_bmw_client();

// ---------------------- tiny helpers for env config ----------------------
static std::string env_str(const char* key, const char* defv){
    const char* v = std::getenv(key);
    return v && *v ? std::string(v) : std::string(defv);
}
static int env_int(const char* key, int defv){
    const char* v = std::getenv(key);
    if(!v || !*v) return defv;
    try{
        return std::stoi(v);
    }catch(...){
        return defv;
    }
}
static std::string trim_copy(const std::string& s){
    auto isws = [](unsigned char c){ return c=='\n'||c=='\r'||c=='\t'||c==' '; };
    size_t a=0,b=s.size();
    while(a<b && isws((unsigned char)s[a])) ++a;
    while(b>a && isws((unsigned char)s[b-1])) --b;
    return s.substr(a,b-a);
}

static void load_env_file(const std::string& path=".env"){
    std::ifstream f(path);
    if(!f) return;
    std::string line;
    while(std::getline(f, line)){
        if(line.empty() || line[0]=='#') continue;
        auto pos = line.find('=');
        if(pos==std::string::npos) continue;
        std::string key = trim_copy(line.substr(0,pos));
        std::string val = trim_copy(line.substr(pos+1));
        // einfache Quote-Handhabung
        if(val.size()>=2 && ((val.front()=='"' && val.back()=='"') || (val.front()=='\'' && val.back()=='\''))){
            val = val.substr(1, val.size()-2);
        }
        if(!key.empty()) setenv(key.c_str(), val.c_str(), 1);
    }
}

// ===================== Configuration =====================
static std::string CLIENT_ID;
static std::string GCID;
static std::string BMW_HOST;
static int         BMW_PORT;
static std::string LOCAL_HOST;
static int         LOCAL_PORT;
static std::string LOCAL_PREFIX;
static std::string LOCAL_USER;
static std::string LOCAL_PASSWORD;
static std::string ID_TOKEN_FILE;
static std::string REFRESH_TOKEN_FILE;

// ===================== Globals =====================
static std::atomic<bool> g_stop{false};
static std::string g_id_token;
static std::string g_refresh_token;
static std::atomic<long> g_id_token_exp{0};

static mosquitto* g_bmw = nullptr;
static mosquitto* g_local = nullptr;

static std::atomic<bool> g_connected{false};
static std::atomic<long> g_last_connect_attempt{0};
static std::atomic<long> g_next_connect_after{0}; // backoff fence for (re)connects

static std::mt19937 rng{std::random_device{}()};
static long jitter_ms(long base_ms){ std::uniform_int_distribution<int> d(-250,250); return base_ms + d(rng); }

// ===================== Helpers =====================
// Helper: dirname
static std::string dirname_of(const std::string& p){
    std::filesystem::path pp(p);
    auto d = pp.parent_path();
    return d.empty() ? std::string(".") : d.string();
}

// XDG-style token directory for current user
static std::string token_dir() {
    const char* xdg = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    if (xdg && *xdg) return std::string(xdg) + "/bmw-mqtt-bridge";
    if (home && *home) return std::string(home) + "/.local/state/bmw-mqtt-bridge";
    // very rare fallback (no HOME): stay relative but consistent
    return std::string("./.local/state/bmw-mqtt-bridge");
}

// helper: simple placeholder check for 1111-IDs
static bool is_placeholder_uuid(const std::string& v){
    static const std::regex all_ones("^1{8}-1{4}-1{4}-1{4}-1{12}$");
    return v.empty() || std::regex_match(v, all_ones);
}

static void bmw_full_reconnect(){
    // alten Client sauber neu aufbauen
    if (g_bmw) {
        mosquitto_loop_stop(g_bmw, true);
        mosquitto_destroy(g_bmw);
        g_bmw = nullptr;
    }
    g_bmw = create_bmw_client();
    if (!g_bmw) {
        std::cerr << "[bridge] rebuild failed (mosquitto_new)\n";
        return;
    }
    mosquitto_loop_start(g_bmw);

    int rc = mosquitto_connect_async(g_bmw, BMW_HOST.c_str(), BMW_PORT, 30);
    g_last_connect_attempt = time(nullptr);
    std::cerr << "[bridge] rebuild+connect rc=" << rc << "\n";
}

static void publish_status(bool connected) {
    if (!g_local) return;
    json j;
    j["connected"] = connected;
    j["timestamp"] = static_cast<long>(time(nullptr));
    std::string payload = j.dump();
    mosquitto_publish(g_local, nullptr, "bmw/status",
                      payload.size(), payload.data(), 0, true);
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

static std::string trim(std::string s) {
    auto isws = [](unsigned char c){ return c=='\n'||c=='\r'||c=='\t'||c==' '; };
    while (!s.empty() && isws(s.back())) s.pop_back();
    size_t i=0; while (i<s.size() && isws(s[i])) ++i;
    return s.substr(i);
}

static bool write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << data;
    return true;
}

// ---- Base64url decode (no OpenSSL; safe handling of '=' padding) ----
static inline uint8_t b64tbl(char c){
    if(c>='A'&&c<='Z') return c-'A';
    if(c>='a'&&c<='z') return c-'a'+26;
    if(c>='0'&&c<='9') return c-'0'+52;
    if(c=='+') return 62;
    if(c=='/') return 63;
    return 0xFF; // INVALID (do not map '=' here!)
}
static std::string b64url_to_b64(std::string s){
    std::replace(s.begin(), s.end(), '-', '+');
    std::replace(s.begin(), s.end(), '_', '/');
    while (s.size() % 4) s.push_back('=');
    return s;
}
static std::string base64url_decode(std::string s){
    s = b64url_to_b64(std::move(s));
    std::string out; out.reserve((s.size()*3)/4);
    for (size_t i=0; i<s.size(); i+=4){
        uint8_t a=b64tbl(s[i]), b=b64tbl(s[i+1]);
        uint8_t c=(s[i+2]=='=')?0xFF:b64tbl(s[i+2]);
        uint8_t d = (s[i+3] == '=') ? 0xFF : b64tbl(s[i+3]);
        if(a==0xFF||b==0xFF) break;
        out.push_back(char((a<<2)|(b>>4)));
        if(c!=0xFF){
            out.push_back(char(((b&0x0F)<<4)|(c>>2)));
            if(d!=0xFF)
                out.push_back(char(((c&0x03)<<6)|d));
        }
    }
    return out;
}

static long jwt_exp_unix(const std::string& jwt){
    // JWT: header.payload.sig  → we want the payload part
    auto p1 = jwt.find('.'); if(p1==std::string::npos) return 0;
    auto p2 = jwt.find('.', p1+1); if(p2==std::string::npos) return 0;
    auto payload = base64url_decode(jwt.substr(p1+1, p2-(p1+1)));
    json j = json::parse(payload, nullptr, false);
    if(j.is_discarded()) return 0;
    return j.value("exp", 0L);
}

static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata){
    auto* s = static_cast<std::string*>(userdata);
    s->append(static_cast<const char*>(ptr), size*nmemb);
    return size*nmemb;
}

// ===================== MQTT Callbacks =====================

// v5 connect callback (no property iteration, Debian header only forward-declares properties)
static void on_bmw_connect_v5(struct mosquitto*, void*, int rc, int flags, const mosquitto_property* /*props*/){
    const char* reason = mosquitto_reason_string(rc);
    std::cout << "[bridge] BMW on_connect_v5 rc=" << rc
              << " (" << (reason ? reason : "unknown") << ")"
              << " sp=" << ((flags & 0x01) ? 1 : 0)
              << "\n";

    if(rc == 0){
        g_connected = true;
        std::string sub = GCID + std::string("/+");
        int mid = 0;
        int s_rc = mosquitto_subscribe(g_bmw, &mid, sub.c_str(), 1);
        std::cerr << "[bridge] subscribe '" << sub << "' rc=" << s_rc << " mid=" << mid << "\n";
        publish_status(true);
        g_last_connect_attempt = 0;
        return;
    }

    // failed → set backoff
    long now = time(nullptr);
    long delay = 5; // default
    if (rc == 151) delay = 60;              // Quota exceeded
    if (rc == 128 || rc == 133) delay = 20; // Unspecified / Server busy
    if (rc == 135) delay = 30;              // Not authorized

    g_next_connect_after = now + delay + (jitter_ms(0)/1000);
    g_connected = false;
    publish_status(false);
}

static void on_bmw_disconnect(struct mosquitto*, void*, int rc){
    std::cout << "[bridge] BMW disconnect rc=" << rc << "\n";
    g_connected = false;
    publish_status(false);
}

static void on_bmw_disconnect_v5(struct mosquitto*, void*, int rc,
                                 const mosquitto_property* /*props*/){
    const char* reason = mosquitto_reason_string(rc);
    std::cerr << "[bridge] BMW disconnect_v5 rc=" << rc
              << " (" << (reason ? reason : "unknown") << ")\n";
    g_connected = false;
    publish_status(false);
}

static void on_bmw_message(struct mosquitto*, void*, const struct mosquitto_message* m){
    // Original topic in format GCID/<VIN>/....
    std::string in_topic = m->topic ? m->topic : "";

    // Republishing the exact message in ..../raw/<VIN>/....
    auto pos = in_topic.find('/');
    std::string out_topic = LOCAL_PREFIX + "raw" + (pos!=std::string::npos ? in_topic.substr(pos) : "");
    int rc = mosquitto_publish(g_local, nullptr, out_topic.c_str(), m->payloadlen, m->payload, 0, false);

    std::cerr << "[bridge] fwd rc=" << rc
              << " in='"  << in_topic
              << "' out='"<< out_topic
              << "' bytes="<< (m ? m->payloadlen : 0)
              << " qos="   << (m ? m->qos : -1)
              << " retain="<< (m ? (int)m->retain : -1)
              << "\n";

    // Parsing the message payload and publishing in dedicated appropiate topic
    try {
        // Parse incoming JSON payload
        std::string payload_str(static_cast<char*>(m->payload), m->payloadlen);
        auto j = json::parse(payload_str);

        // Extract VIN from payload or topic
        std::string vin;

        // VIN from payload
        if (j.contains("vin") && j["vin"].is_string()) {
            vin = j["vin"].get<std::string>();
        }

        // Fallback: VIN from topic (GCID/<VIN>/...)
        if (vin.empty()) {
            auto pos = in_topic.find('/');
            if (pos != std::string::npos) {
                auto next = in_topic.find('/', pos+1);

                if (next != std::string::npos) {
                    vin = in_topic.substr(pos+1, next - (pos+1));
                } 
                else {
                    // Take everything from pos+1 to the end
                    vin = in_topic.substr(pos+1);
                }

                if (vin.size() != 17) {
                    throw std::runtime_error("Invalid VIN length extracted from topic: " + vin);
                }
            }
        }

        if (vin.empty()) {
            throw std::runtime_error("VIN not found in payload or topic");
        }

        if (j.contains("data") && j["data"].is_object()) {
            for (auto& [propName, propObj] : j["data"].items()) {
                if (propObj.contains("value")) {
                    // Extend topic with propertyName
                    std::string prop_topic = LOCAL_PREFIX + "vehicles/" + vin + "/" + propName;

                    // Dump the entire object { value, timestamp, unit }
                    // .dump() ensures correct stringification (works for numbers, strings, etc.)
                    std::string value_str = propObj.dump(); 

                    int rc = mosquitto_publish(
                        g_local,
                        nullptr,
                        prop_topic.c_str(),
                        value_str.size(),
                        value_str.data(),
                        0,
                        false
                    );

                    std::cerr << "[bridge] fwd rc=" << rc
                              << " in='"  << in_topic
                              << "' out='"<< prop_topic
                              << "' value="<< value_str
                              << " bytes="<< value_str.size()
                              << " qos="   << (m ? m->qos : -1)
                              << " retain="<< (m ? (int)m->retain : -1)
                              << "\n";
                }
            }
        }
        else {
            throw std::runtime_error("No valid data in payloyad " + payload_str);
        }
    } 
    catch (const std::exception& e) {
        std::cerr << "[bridge] JSON parse error: " << e.what() << "\n";
    }
}

// log callback: set g_last_connect_attempt when "sending CONNECT" appears; filter ping spam
static void on_bmw_log(struct mosquitto* /*mosq*/, void* /*userdata*/,
                       int level, const char* str)
{
    if(!str) return;
    if (std::strstr(str, "PINGREQ") || std::strstr(str, "PINGRESP")) return;

    if (std::strstr(str, "sending CONNECT")) {
        g_last_connect_attempt = time(nullptr);
    }

    if (std::strstr(str, "OpenSSL Error")
        || std::strstr(str, "SSL")
        || std::strstr(str, "unexpected eof"))
    {
        g_connected = false;
        publish_status(false);
        long now = time(nullptr);
        g_next_connect_after = now + 5 + (jitter_ms(0)/1000);
    }

    std::cerr << "[bmw/log] level=" << level << " " << str << "\n";
}

static void on_bmw_suback(struct mosquitto* /*mosq*/, void* /*userdata*/,
                          int mid, int qos_count, const int* granted_qos)
{
    std::cerr << "[bmw] SUBACK mid=" << mid
              << " qos_count=" << qos_count;
    if (qos_count > 0 && granted_qos) std::cerr << " granted0=" << granted_qos[0];
    std::cerr << "\n";
}

// ===================== BMW client factory =====================

static mosquitto* create_bmw_client() {
    mosquitto* m = mosquitto_new(CLIENT_ID.c_str(), true, nullptr);
    if(!m) return nullptr;

    // enable MQTT v5
    mosquitto_int_option(m, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V5);
    mosquitto_reconnect_delay_set(m, 1, 10, true);

    // callbacks (v5)
    mosquitto_connect_v5_callback_set(m, on_bmw_connect_v5);
    mosquitto_disconnect_callback_set(m, on_bmw_disconnect);
    mosquitto_disconnect_v5_callback_set(m, on_bmw_disconnect_v5);
    mosquitto_message_callback_set(m, on_bmw_message);
    mosquitto_log_callback_set(m, on_bmw_log);
    mosquitto_subscribe_callback_set(m, on_bmw_suback);

    // TLS with system CA
    mosquitto_tls_set(
        m,
        "/etc/ssl/certs/ca-certificates.crt",
        NULL, NULL, NULL, NULL
    );

    // auth
    mosquitto_username_pw_set(m, GCID.c_str(), g_id_token.c_str());

    return m;
}

// ===================== Main =====================

static void sigint_handler(int){ g_stop = true; }

int main(){
    std::signal(SIGINT,  sigint_handler);
    std::signal(SIGTERM, sigint_handler);

    // load .env from fixed token directory (created by bmw_flow.sh)
    const std::string TDIR = token_dir();
    const std::string ENV_PATH = (std::filesystem::path(TDIR) / ".env").string();
    load_env_file(ENV_PATH);

    // initialize
    CLIENT_ID        = env_str("CLIENT_ID",        "");
    GCID             = env_str("GCID",             "");
    BMW_HOST         = env_str("BMW_HOST",         "customer.streaming-cardata.bmwgroup.com");
    BMW_PORT         = env_int("BMW_PORT",         9000);
    LOCAL_HOST       = env_str("LOCAL_HOST",       "127.0.0.1");
    LOCAL_PORT       = env_int("LOCAL_PORT",       1883);
    LOCAL_PREFIX     = env_str("LOCAL_PREFIX",     "bmw/");
    LOCAL_USER       = env_str("LOCAL_USER",       "");
    LOCAL_PASSWORD   = env_str("LOCAL_PASSWORD",   "");
    // fixed token files (no env overrides)
    ID_TOKEN_FILE       = (std::filesystem::path(TDIR) / "id_token.txt").string();
    REFRESH_TOKEN_FILE  = (std::filesystem::path(TDIR) / "refresh_token.txt").string();

    // Ensure LOCAL_PREFIX ends with '/'
    if (!LOCAL_PREFIX.empty() && LOCAL_PREFIX.back() != '/') {
        LOCAL_PREFIX.push_back('/');
    }

    // ensure token directory exists
    if (!std::filesystem::exists(TDIR)) {
        std::cerr << "✖ Token directory missing: " << TDIR << "\n"
                  << "   Run scripts/bmw_flow.sh first.\n";
        return 1;
    }

    // validate required IDs (no defaults; reject placeholders)
    if (is_placeholder_uuid(CLIENT_ID)) {
        std::cerr << "✖ CLIENT_ID missing or placeholder in " << ENV_PATH << "\n";
        return 1;
    }
    if (is_placeholder_uuid(GCID)) {
        std::cerr << "✖ GCID missing or placeholder in " << ENV_PATH << "\n";
        return 1;
    }

    // refresh logic constants
    constexpr long CLOCK_SKEW_SECS   = 60;    // 1 min safety for clock drift

    std::ios::sync_with_stdio(false);
    std::cout.setf(std::ios::unitbuf); // auto-flush stdout

    // initial tokens
    g_id_token = trim(read_file(ID_TOKEN_FILE));
    g_refresh_token = trim(read_file(REFRESH_TOKEN_FILE));
    if(g_id_token.empty() || g_refresh_token.empty()){
        std::cerr << "✖ id_token.txt or refresh_token.txt missing/empty in " << TDIR << "\n";
        return 1;
    }
    g_id_token_exp = jwt_exp_unix(g_id_token);
    if (g_id_token_exp.load() == 0) {
        std::cerr << "✖ invalid id_token (no exp) → trying refresh\n";
        if (!refresh_tokens()) {
            std::cerr << "✖ cannot obtain valid token, exiting\n";
            return 1;
        }
    }

    // libs init
    curl_global_init(CURL_GLOBAL_DEFAULT);
    mosquitto_lib_init();

    // local broker
    g_local = mosquitto_new("bmw-local-forwarder", true, nullptr);
    if(!g_local){ std::cerr << "mosquitto_new local failed\n"; return 2; }

    mosquitto_reconnect_delay_set(g_local, 1, 10, true);
    const char* lwt = "{\"connected\":false}";
    mosquitto_will_set(g_local, "bmw/status", strlen(lwt), lwt, 0, true);

    // Set credentials if provided
    if (!LOCAL_USER.empty() && !LOCAL_PASSWORD.empty()) {
        mosquitto_username_pw_set(g_local, LOCAL_USER.c_str(), LOCAL_PASSWORD.c_str());
    }

    if(mosquitto_connect(g_local, LOCAL_HOST.c_str(), LOCAL_PORT, 30) != MOSQ_ERR_SUCCESS){
        std::cerr << "connect local failed\n"; return 3;
    }
    mosquitto_loop_start(g_local);
    publish_status(false);

    // BMW broker
    g_bmw = create_bmw_client();
    if(!g_bmw){ std::cerr << "mosquitto_new bmw failed\n"; return 4; }
    mosquitto_loop_start(g_bmw);

    // initial connect (respect backoff)
    if (time(nullptr) >= g_next_connect_after.load()) {
        int rc = mosquitto_connect_async(g_bmw, BMW_HOST.c_str(), BMW_PORT, 30);
        if(rc != MOSQ_ERR_SUCCESS){
            std::cerr << "connect BMW failed (host/port/TLS?) rc=" << rc << "\n";
            // do not exit; watchdog will retry later
        }
    } else {
        std::cerr << "[bridge] initial connect delayed due to backoff\n";
    }

    std::cout << "[bridge] running… (Ctrl+C / SIGTERM to stop)\n";

    // === Token refresh + CONNECT watchdog + backoff ===
    const long CONNECT_TIMEOUT = 30; // seconds until we assume "CONNECT hung"
    long last_refresh_attempt = 0;
    long last_successful_refresh = time(nullptr);
    constexpr long SOFT_MARGIN_SECS = 10*60;   // refresh 10 min before exp
    constexpr long HARD_REFRESH_SECS = 45*60;  // refresh at least every 45 min

    auto needs_soft_refresh = [&](long now){
        return (g_id_token_exp.load() - now) <= (SOFT_MARGIN_SECS + CLOCK_SKEW_SECS);
    };
    auto needs_hard_refresh = [&](long now){
        return (now - last_successful_refresh) >= HARD_REFRESH_SECS;
    };

    while(!g_stop){
        std::this_thread::sleep_for(std::chrono::seconds(1));
        long now = time(nullptr);

        // 0) Backoff window active? → do not trigger new actions
        if (now < g_next_connect_after.load()) continue;

        bool due_soft = needs_soft_refresh(now);
        bool due_hard = needs_hard_refresh(now);
        bool should_try = (due_soft || due_hard) && (now - last_refresh_attempt > 10);

        if (should_try){
            // small jitter to avoid sync with other processes
            std::this_thread::sleep_for(std::chrono::milliseconds(100 + (rng() % 200)));

            std::cout << "[bridge] token refresh (" << (due_soft ? "soft" : "hard") << ")\n";

            if (refresh_tokens()){
                last_refresh_attempt    = now;
                last_successful_refresh = now;

                int upw_rc = mosquitto_username_pw_set(g_bmw, GCID.c_str(), g_id_token.c_str());
                if (upw_rc != MOSQ_ERR_SUCCESS) {
                    std::cerr << "[bridge] username_pw_set rc=" << upw_rc << "\n";
                }

                g_connected = false;
                publish_status(false);

                // leichtes Backoff + Jitter wie beim Script-Flow
                long delay_ms = 1500 + (rng()%500);
                g_next_connect_after = time(nullptr) + 1; // 1s Sperre, nur zur Sicherheit
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

                // kompletter Rebuild → verhindert TLS/State-Races
                bmw_full_reconnect();
            } else {
                last_refresh_attempt = now;
                g_next_connect_after = now + 15;
                std::cerr << "[bridge] refresh failed, retry soon\n";
            }
        }

        // CONNECT watchdog: CONNECT sent but no CONNACK in time
        long last_attempt = g_last_connect_attempt.load();
        bool connect_hung = (last_attempt != 0) && ((now - last_attempt) > CONNECT_TIMEOUT);
        if (connect_hung) {
            if (now < g_next_connect_after.load()) continue;

            std::cerr << "[bridge] CONNECT timed out or handshake failed -> full mosquitto client rebuild\n";
            g_connected = false;
            publish_status(false);

            if (g_bmw) {
                mosquitto_loop_stop(g_bmw, true);
                mosquitto_destroy(g_bmw);
                g_bmw = nullptr;
            }

            g_bmw = create_bmw_client();
            if(!g_bmw){
                std::cerr << "[bridge] rebuild failed (mosquitto_new)\n";
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            mosquitto_loop_start(g_bmw);

            if (time(nullptr) >= g_next_connect_after.load()) {
                int rc = mosquitto_connect_async(g_bmw, BMW_HOST.c_str(), BMW_PORT, 30);
                std::cerr << "[bridge] rebuild+connect rc=" << rc << "\n";
                g_last_connect_attempt = time(nullptr);
            } else {
                std::cerr << "[bridge] rebuild done, connect delayed due to backoff\n";
            }
        }
    }

    // Cleanup
    if (g_bmw) {
        mosquitto_loop_stop(g_bmw, true);
        mosquitto_disconnect(g_bmw);
        mosquitto_destroy(g_bmw);
    }
    if (g_local) {
        mosquitto_loop_stop(g_local, true);
        mosquitto_disconnect(g_local);
        mosquitto_destroy(g_local);
    }
    mosquitto_lib_cleanup();
    curl_global_cleanup();
    std::cout << "[bridge] bye\n";
    return 0;
}


// ============= refresh tokens =============

// small utility: safely writes a file (0644 default)
static bool write_file_mode(const std::string& path, const std::string& data, mode_t mode=0644){
    int fd = ::open(path.c_str(), O_CREAT|O_TRUNC|O_WRONLY, mode);
    if (fd < 0) return false;
    ssize_t want = (ssize_t)data.size();
    const char* p = data.data();
    while (want > 0){
        ssize_t n = ::write(fd, p, want);
        if (n <= 0) { ::close(fd); return false; }
        want -= n; p += n;
    }
    ::fsync(fd);
    ::close(fd);
    return true;
}

// form-urlencode for a key/value with libcurl (uses its own CURL easy handle)
static std::string urlencode_component(const std::string& s){
    CURL* h = curl_easy_init();
    if(!h) return s; // worst case: unencoded
    char* esc = curl_easy_escape(h, s.c_str(), (int)s.size());
    std::string out = esc ? esc : "";
    if (esc) curl_free(esc);
    curl_easy_cleanup(h);
    return out;
}

static std::string build_form_body(const std::vector<std::pair<std::string,std::string>>& kv){
    std::ostringstream oss;
    bool first = true;
    for (auto& [k,v] : kv){
        if(!first) oss << "&";
        first = false;
        oss << urlencode_component(k) << "=" << urlencode_component(v);
    }
    return oss.str();
}

static bool write_file_atomic(const std::string& final_path,
                              const std::string& data,
                              mode_t mode = 0644)
{
    namespace fs = std::filesystem;

    fs::path fpath(final_path);
    fs::path dir = fpath.parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec); // ignorierbar, wir loggen nur bei echten Fehlern später

    // tmp-Datei im gleichen Verzeichnis erzeugen
    std::string tmpl = (dir / (fpath.filename().string() + ".tmp.XXXXXX")).string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');

    int tfd = ::mkstemp(buf.data()); // erzeugt .../filename.tmp.ABC123
    if (tfd < 0) {
        std::cerr << "[bridge] mkstemp failed: " << std::strerror(errno) << "\n";
        return false;
    }

    // Rechte setzen (unabhängig von umask)
    if (::fchmod(tfd, mode) != 0) {
        std::cerr << "[bridge] fchmod failed: " << std::strerror(errno) << "\n";
        ::close(tfd);
        ::unlink(buf.data());
        return false;
    }

    // schreiben (vollständig)
    const char* p = data.data();
    ssize_t left = (ssize_t)data.size();
    while (left > 0) {
        ssize_t n = ::write(tfd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[bridge] write failed: " << std::strerror(errno) << "\n";
            ::close(tfd);
            ::unlink(buf.data());
            return false;
        }
        left -= n; p += n;
    }

    // flushen
    if (::fsync(tfd) != 0) {
        std::cerr << "[bridge] fsync(tmp) failed: " << std::strerror(errno) << "\n";
        ::close(tfd);
        ::unlink(buf.data());
        return false;
    }
    ::close(tfd);

    // atomar ersetzen (gleiches FS ⇒ kein EXDEV)
    if (::rename(buf.data(), final_path.c_str()) != 0) {
        std::cerr << "[bridge] rename failed: " << std::strerror(errno)
                  << " (errno=" << errno << ")\n";
        ::unlink(buf.data());
        return false;
    }

    // Verzeichnis-Flush (damit das Rename selbst crash-sicher ist)
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }

    return true;
}

static bool refresh_tokens() {

    std::cout << "[bridge] refresh started\n";

    // load current refresh token (as in the script)
    std::string cur_refresh = trim(read_file(REFRESH_TOKEN_FILE));
    if (cur_refresh.empty()) {
        std::cerr << "[bridge] refresh: refresh_token.txt missing/empty\n";
        return false;
    }

    // form body
    const std::string url = "https://customer.bmwgroup.com/gcdm/oauth/token";
    const std::string body = build_form_body({
        {"grant_type",   "refresh_token"},
        {"refresh_token",cur_refresh},
        {"client_id",    CLIENT_ID}
    });

    // HTTP Request via libcurl
    std::string resp;
    long http_code = 0;

    CURL* c = curl_easy_init();
    if(!c){
        std::cerr << "[bridge] curl_easy_init failed\n";
        return false;
    }

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/x-www-form-urlencoded");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());

    // Timeout/SSL
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); // threadsafe timeouts
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "bmw-mqtt-bridge/1.0");

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        std::cerr << "[bridge] curl perform failed: " << curl_easy_strerror(rc) << "\n";
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        return false;
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    // determine target paths based on configured files
    std::string id_path  = ID_TOKEN_FILE;
    std::string rt_path  = REFRESH_TOKEN_FILE;
    std::string dir      = dirname_of(id_path);
    std::string at_path  = (std::filesystem::path(dir) / "access_token.txt").string();

    // save entire response (debug) in same directory as token files
    try {
        json dbg = json::parse(resp);
        write_file_mode((std::filesystem::path(dir) / "token_refresh_response.json").string(),
                        dbg.dump(2) + "\n", 0644);
    } catch (...) {
        write_file_mode((std::filesystem::path(dir) / "token_refresh_response.json").string(),
                        resp, 0644);
    }

    if (http_code != 200) {
        std::cerr << "✖ Refresh HTTP " << http_code << ":\n" << resp << "\n";
        return false;
    }

    // parse JSON & check error
    json j = json::parse(resp, nullptr, false);
    if (j.is_discarded()) {
        std::cerr << "✖ Refresh: invalid JSON\n";
        return false;
    }
    if (j.contains("error") && !j["error"].is_null()) {
        std::cerr << "✖ Refresh failed:\n" << j.dump(2) << "\n";
        return false;
    }

    // extract tokens
    std::string new_id  = j.value("id_token",      "");
    std::string new_rt  = j.value("refresh_token", "");
    std::string new_acc = j.value("access_token",  "");

    // remove \r\n / trim
    new_id  = trim(new_id);
    new_rt  = trim(new_rt);
    new_acc = trim(new_acc);

    if (new_id.empty() || new_rt.empty() || new_acc.empty()) {
        std::cerr << "✖ Refresh: missing data in response\n";
        return false;
    }

    // --- Atomar direkt ins Zielverzeichnis schreiben (kein /tmp mehr) ---
    bool ok = true;
    ok &= write_file_atomic(id_path, new_id, 0644);
    ok &= write_file_atomic(rt_path, new_rt, 0644);
    ok &= write_file_atomic(at_path, new_acc, 0644);

    if (!ok) {
        std::cerr << "[bridge] writing tokens atomically failed\n";
        return false;
    }

    // update in-memory
    g_id_token      = new_id;
    g_refresh_token = new_rt;
    g_id_token_exp  = jwt_exp_unix(g_id_token);

    std::cout << "✔ New Tokens saved:\n"
              << "   id_token.txt, refresh_token.txt, access_token.txt\n";
    std::cout << "[bridge] token refreshed via HTTP, exp=" << g_id_token_exp
              << " (in " << (g_id_token_exp.load() - time(nullptr)) << "s)\n";

    return true;
}

/*
static bool refresh_tokens() {

    std::cout << "[bridge] refresh started\n";

    // load current refresh token (as in the script)
    std::string cur_refresh = trim(read_file(REFRESH_TOKEN_FILE));
    if (cur_refresh.empty()) {
        std::cerr << "[bridge] refresh: refresh_token.txt missing/empty\n";
        return false;
    }

    // form body
    const std::string url = "https://customer.bmwgroup.com/gcdm/oauth/token";
    const std::string body = build_form_body({
        {"grant_type",   "refresh_token"},
        {"refresh_token",cur_refresh},
        {"client_id",    CLIENT_ID}
    });

    // HTTP Request via libcurl
    std::string resp;
    long http_code = 0;

    CURL* c = curl_easy_init();
    if(!c){
        std::cerr << "[bridge] curl_easy_init failed\n";
        return false;
    }

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/x-www-form-urlencoded");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());

    // Timeout/SSL
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); // threadsafe timeouts
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "bmw-mqtt-bridge/1.0");

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        std::cerr << "[bridge] curl perform failed: " << curl_easy_strerror(rc) << "\n";
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        return false;
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    // determine target paths based on configured files
    std::string id_path  = ID_TOKEN_FILE;
    std::string rt_path  = REFRESH_TOKEN_FILE;
    std::string dir      = dirname_of(id_path);
    std::string at_path  = (std::filesystem::path(dir) / "access_token.txt").string();

    // save entire response (debug) in same directory as token files
    // (pretty-printed, falls back to raw on parse error)
    try {
        json dbg = json::parse(resp);
        write_file_mode((std::filesystem::path(dir) / "token_refresh_response.json").string(),
                        dbg.dump(2) + "\n", 0644);
    } catch (...) {
        // if not JSON: save raw response
        write_file_mode((std::filesystem::path(dir) / "token_refresh_response.json").string(),
                        resp, 0644);
    }

    if (http_code != 200) {
        std::cerr << "✖ Refresh HTTP " << http_code << ":\n" << resp << "\n";
        return false;
    }

    // parse JSON & check error
    json j = json::parse(resp, nullptr, false);
    if (j.is_discarded()) {
        std::cerr << "✖ Refresh: invalid JSON\n";
        return false;
    }
    if (j.contains("error") && !j["error"].is_null()) {
        std::cerr << "✖ Refresh failed:\n" << j.dump(2) << "\n";
        return false;
    }

    // extract tokens
    std::string new_id  = j.value("id_token",      "");
    std::string new_rt  = j.value("refresh_token", "");
    std::string new_acc = j.value("access_token",  "");

    // remove \r\n / trim
    new_id  = trim(new_id);
    new_rt  = trim(new_rt);
    new_acc = trim(new_acc);

    if (new_id.empty() || new_rt.empty() || new_acc.empty()) {
        std::cerr << "✖ Refresh: missing data in response\n";
        return false;
    }

    // store atomically – tmpdir → move
    char tmpl[] = "/tmp/bmwtokens.XXXXXX";
    char* tmp = ::mkdtemp(tmpl);
    if (!tmp) {
        std::cerr << "[bridge] mkdtemp failed\n";
        return false;
    }
    std::string tmpdir = tmp; // e.g. /tmp/bmwtokens.ABCDEF

    bool ok = true;
    ok &= write_file_mode(tmpdir + "/id_token.txt",      new_id,  0644);
    ok &= write_file_mode(tmpdir + "/refresh_token.txt", new_rt,  0644);
    ok &= write_file_mode(tmpdir + "/access_token.txt",  new_acc, 0644);

    if (!ok) {
        std::cerr << "[bridge] write tmp token files failed\n";
        // Cleanup best-effort
        std::error_code ec;
        std::filesystem::remove_all(tmpdir, ec);
        return false;
    }

    // move (rename is atomic on the same FS) to configured target paths
    if (std::rename((tmpdir + "/id_token.txt").c_str(),      id_path.c_str()) != 0 ||
        std::rename((tmpdir + "/refresh_token.txt").c_str(), rt_path.c_str()) != 0 ||
        std::rename((tmpdir + "/access_token.txt").c_str(),  at_path.c_str()) != 0) {
        std::cerr << "[bridge] rename() token files failed\n";
        std::error_code ec;
        std::filesystem::remove_all(tmpdir, ec);
        return false;
    }

    // remove tmpdir
    std::error_code ec;
    std::filesystem::remove_all(tmpdir, ec);

    // update in-memory
    g_id_token      = new_id;
    g_refresh_token = new_rt;
    g_id_token_exp  = jwt_exp_unix(g_id_token);

    std::cout << "✔ New Tokens saved:\n"
              << "   id_token.txt, refresh_token.txt, access_token.txt\n";
    std::cout << "[bridge] token refreshed via HTTP, exp=" << g_id_token_exp
              << " (in " << (g_id_token_exp.load() - time(nullptr)) << "s)\n";

    return true;
}

*/

