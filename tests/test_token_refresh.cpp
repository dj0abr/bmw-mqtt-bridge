// Exercise the production refresh path with a mocked HTTP transport.
#define main bridge_original_main
#include "../src/bmw_mqtt_bridge.cpp"
#undef main
#include <cstdarg>
#include <stdexcept>

static std::string response_body;
static std::string request_body;
static std::string* response_target = nullptr;
static long response_status = 200;
static CURLcode transport_result = CURLE_OK;
static int request_count = 0;
static std::string fail_rename_path;

extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* from, const char* to) {
    if (!fail_rename_path.empty() && to == fail_rename_path) {
        errno = EACCES;
        return -1;
    }
    return __real_rename(from, to);
}

extern "C" CURLcode __wrap_curl_easy_setopt(CURL*, CURLoption option, ...) {
    va_list args;
    va_start(args, option);
    if (option == CURLOPT_WRITEDATA) response_target = va_arg(args, std::string*);
    if (option == CURLOPT_POSTFIELDS) request_body = va_arg(args, const char*);
    va_end(args);
    return CURLE_OK;
}
extern "C" CURLcode __wrap_curl_easy_perform(CURL*) {
    ++request_count;
    if (transport_result == CURLE_OK) *response_target = response_body;
    return transport_result;
}
extern "C" CURLcode __wrap_curl_easy_getinfo(CURL*, CURLINFO info, ...) {
    va_list args;
    va_start(args, info);
    if (info == CURLINFO_RESPONSE_CODE) *va_arg(args, long*) = response_status;
    va_end(args);
    return CURLE_OK;
}

static void require(bool condition, const std::string& description) {
    if (!condition) throw std::runtime_error(description);
}
static std::string encode_payload(const std::string& value) {
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string encoded;
    unsigned buffer = 0;
    int bits = 0;
    for (unsigned char c : value) {
        buffer = (buffer << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            encoded += chars[(buffer >> bits) & 63];
        }
    }
    if (bits) encoded += chars[(buffer << (6-bits)) & 63];
    return encoded;
}
static std::string jwt(const json& payload) {
    return "e30." + encode_payload(payload.dump()) + ".c2ln";
}
static mode_t permissions(const std::string& path) {
    struct stat info{};
    require(::stat(path.c_str(), &info) == 0, "stat failed: " + path);
    return info.st_mode & 0777;
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "supply an isolated state directory");
        const std::filesystem::path dir(argv[1]);
        std::filesystem::create_directories(dir);
        ID_TOKEN_FILE = (dir / "id_token.txt").string();
        REFRESH_TOKEN_FILE = (dir / "refresh_token.txt").string();
        const std::string access_path = (dir / "access_token.txt").string();
        const std::string diagnostic_path = (dir / "token_refresh_response.json").string();
        const long old_exp = time(nullptr) + 1800;
        const long new_exp = time(nullptr) + 3600;
        const std::string old_id = jwt({{"exp", old_exp}});
        const std::string new_id = jwt({{"exp", new_exp}});
        const json valid = {{"id_token", new_id}, {"refresh_token", "NEW_REFRESH_SECRET"},
                            {"access_token", "NEW_ACCESS_SECRET"}, {"expires_in", 3600}};
        curl_global_init(CURL_GLOBAL_DEFAULT);
        CLIENT_ID = "test-client";

        auto reset = [&] {
            require(write_file(ID_TOKEN_FILE, old_id), "write old ID token");
            require(write_file(REFRESH_TOKEN_FILE, "OLD_REFRESH_SECRET"), "write old refresh token");
            require(write_file(access_path, "OLD_ACCESS_SECRET"), "write old access token");
            g_id_token = old_id;
            g_refresh_token = "OLD_REFRESH_SECRET";
            g_id_token_exp = old_exp;
            response_status = 200;
            transport_result = CURLE_OK;
        };
        auto unchanged = [&] {
            require(trim(read_file(ID_TOKEN_FILE)) == old_id, "ID token was overwritten on failure");
            require(trim(read_file(REFRESH_TOKEN_FILE)) == "OLD_REFRESH_SECRET", "refresh token was overwritten on failure");
            require(trim(read_file(access_path)) == "OLD_ACCESS_SECRET", "access token was overwritten on failure");
            require(g_id_token == old_id && g_refresh_token == "OLD_REFRESH_SECRET" &&
                    g_id_token_exp == old_exp, "in-memory tokens changed on failure");
        };
        auto redacted = [&] {
            const auto text = read_file(diagnostic_path);
            for (const auto& secret : {new_id, old_id, std::string("NEW_REFRESH_SECRET"),
                     std::string("NEW_ACCESS_SECRET"), std::string("OLD_REFRESH_SECRET")})
                require(text.find(secret) == std::string::npos, "secret in diagnostic file");
            require(permissions(diagnostic_path) == 0600, "diagnostics must be private");
        };

        std::vector<std::pair<std::string, std::string>> invalid = {
            {"empty response", ""}, {"HTML", "<html>error</html>"},
            {"JSON array", "[]"}, {"JSON null", "null"}, {"missing tokens", "{}"},
            {"gateway error", R"({"fault":{"detail":{"errorcode":"keymanagement.service.invalid_access_token"}}})"}
        };
        for (const char* key : {"id_token", "refresh_token", "access_token"}) {
            for (const json& value : {json(nullptr), json("null"), json(""), json(42), json(true), json("bad token")}) {
                json body = valid;
                body[key] = value;
                invalid.emplace_back(std::string("invalid ") + key + ": " + value.dump(), body.dump());
            }
            json body = valid;
            body.erase(key);
            invalid.emplace_back(std::string("missing ") + key, body.dump());
        }
        for (const json& value : {json(nullptr), json("3600"), json(false), json(0), json(-1), json(1.5)}) {
            json body = valid;
            body["expires_in"] = value;
            invalid.emplace_back("invalid expires_in", body.dump());
        }
        for (const json& payload : {json::object(), json::array(), json{{"exp", nullptr}},
                json{{"exp", "tomorrow"}}, json{{"exp", true}}, json{{"exp", -1}},
                json{{"exp", 1.5}}, json{{"exp", time(nullptr)-1}},
                json{{"exp", std::numeric_limits<unsigned long long>::max()}}}) {
            json body = valid;
            body["id_token"] = jwt(payload);
            invalid.emplace_back("invalid JWT expiry", body.dump());
        }
        for (const auto& token : {"not-a-jwt", ".e30.sig", "e30.e30.", "e30.e30.sig.extra"}) {
            json body = valid;
            body["id_token"] = token;
            invalid.emplace_back("malformed JWT", body.dump());
        }
        json error_with_tokens = valid;
        error_with_tokens["error"] = "invalid_grant";
        invalid.emplace_back("OAuth error with tokens", error_with_tokens.dump());
        error_with_tokens = valid;
        error_with_tokens["fault"] = {{"faultstring", "Invalid Access Token"}};
        invalid.emplace_back("gateway error with tokens", error_with_tokens.dump());

        for (const auto& test : invalid) {
            reset();
            response_body = test.second;
            require(!refresh_tokens(), test.first + " was accepted");
            unchanged();
            redacted();
        }
        reset();
        response_body = valid.dump();
        response_status = 400;
        require(!refresh_tokens(), "HTTP error was accepted");
        unchanged();
        redacted();

        reset();
        transport_result = CURLE_OPERATION_TIMEDOUT;
        require(!refresh_tokens(), "timeout was accepted");
        unchanged();
        transport_result = CURLE_OK;

        reset();
        write_file(REFRESH_TOKEN_FILE, "null");
        const int before = request_count;
        require(!refresh_tokens() && request_count == before, "invalid stored refresh token reached HTTP endpoint");

        reset();
        fail_rename_path = REFRESH_TOKEN_FILE;
        response_body = valid.dump();
        const int before_write_failure = request_count;
        require(!refresh_tokens(), "failed token write reported success");
        require(request_count == before_write_failure+1, "persistence failure did not exercise HTTP success");
        unchanged();
        fail_rename_path.clear();

        reset();
        // Also prove that old permissive permissions are tightened on replacement.
        for (const auto& path : {ID_TOKEN_FILE, REFRESH_TOKEN_FILE, access_path, diagnostic_path})
            ::chmod(path.c_str(), 0644);
        response_body = valid.dump();
        require(refresh_tokens(), "valid token refresh failed");
        require(request_body.find("refresh_token=OLD_REFRESH_SECRET") != std::string::npos,
                "refresh did not read the current token from disk");
        require(read_file(ID_TOKEN_FILE) == new_id && g_id_token == new_id && g_id_token_exp == new_exp,
                "valid ID token was not installed");
        require(read_file(REFRESH_TOKEN_FILE) == "NEW_REFRESH_SECRET" &&
                g_refresh_token == "NEW_REFRESH_SECRET", "rotated refresh token was not installed");
        require(read_file(access_path) == "NEW_ACCESS_SECRET", "access token was not installed");
        for (const auto& path : {ID_TOKEN_FILE, REFRESH_TOKEN_FILE, access_path})
            require(permissions(path) == 0600, "token must be private: " + path);
        redacted();
        curl_global_cleanup();
        std::cout << "PASS: " << invalid.size() + 5 << " refresh cases\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
