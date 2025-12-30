#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

static std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream iss(s);
    while (std::getline(iss, cur, ',')) {
        cur = trim(cur);
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

static std::map<std::string, std::string> readConfig(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Unable to open config: " + path);

    std::map<std::string, std::string> cfg;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == '#' || line[0] == ';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = toLower(trim(line.substr(0, eq)));
        std::string v = trim(line.substr(eq + 1));
        cfg[k] = v;
    }
    return cfg;
}

class AmiClient {
public:
    AmiClient() : sock_(-1) {}
    ~AmiClient() { if (sock_ >= 0) close(sock_); }

    void connectTo(const std::string& host, int port) {
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) throw std::runtime_error("socket() failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));

        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            hostent* he = gethostbyname(host.c_str());
            if (!he || he->h_addrtype != AF_INET) throw std::runtime_error("Unable to resolve host: " + host);
            std::memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(in_addr));
        }

        if (connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error(std::string("connect() failed: ") + std::strerror(errno));
        }

        // Banner
        readMessage();
    }

    bool login(const std::string& user, const std::string& secret) {
        std::ostringstream oss;
        oss << "Action: Login\r\n"
            << "Username: " << user << "\r\n"
            << "Secret: " << secret << "\r\n"
            << "Events: on\r\n\r\n";
        sendAll(oss.str());
        auto msg = readMessage();
        return toLower(msg["Response"]) == "success";
    }

    void logoff() {
        try { sendAll("Action: Logoff\r\n\r\n"); } catch (...) {}
    }

    // Multi-message collection helper: read until EventList: Complete for a matching ActionID,
    // or until a Response with no EventList is received (best-effort).
    struct AmiResult {
        std::map<std::string, std::string> response;
        std::vector<std::map<std::string, std::string>> events;
    };

    AmiResult sendActionCollect(const std::string& actionBlock, const std::string& actionId, int guardMax = 800) {
        sendAll(actionBlock);
        AmiResult res;

        bool sawEventList = false;
        bool done = false;

        for (int guard = 0; guard < guardMax && !done; guard++) {
            auto msg = readMessage();

            // If it is a Response, capture it (some actions return a Response plus events)
            if (msg.count("Response")) {
                // Optional ActionID match
                if (!actionId.empty() && msg.count("ActionID") && msg["ActionID"] != actionId) {
                    continue;
                }
                res.response = msg;

                if (!msg.count("EventList")) {
                    // No list, likely done
                    if (!sawEventList) done = true;
                } else {
                    sawEventList = true;
                }
            }

            if (msg.count("Event")) {
                if (!actionId.empty() && msg.count("ActionID") && msg["ActionID"] != actionId) {
                    continue;
                }
                res.events.push_back(msg);

                if (toLower(msg["Event"]).find("complete") != std::string::npos) {
                    // Many list actions end with a *Complete event
                    done = true;
                }
                if (msg.count("EventList") && toLower(msg["EventList"]) == "complete") {
                    done = true;
                }
            }
        }

        return res;
    }

    bool supportsAction(const std::string& actionName) {
        // AMI "ListCommands" returns a list of commands supported by this AMI instance.
        // We use it to check for ConfbridgePlaySound presence.
        std::string actionId = "sb_listcmd_1";
        std::ostringstream oss;
        oss << "Action: ListCommands\r\n"
            << "ActionID: " << actionId << "\r\n\r\n";

        auto res = sendActionCollect(oss.str(), actionId, 1200);

        std::string needle = toLower(actionName);
        for (const auto& ev : res.events) {
            // Depending on Asterisk version, key may be "Command"
            auto it = ev.find("Command");
            if (it != ev.end()) {
                if (toLower(it->second) == needle) return true;
            }
        }

        // As a fallback, try to find the name in any message values
        for (const auto& ev : res.events) {
            for (const auto& kv : ev) {
                if (toLower(kv.second) == needle) return true;
            }
        }
        return false;
    }

    struct RoomInfo {
        std::string conference;
        int parties = 0;
        int marked = 0;
        std::string locked;
        std::string muted;
    };

    std::vector<RoomInfo> listRooms() {
        std::string actionId = "sb_rooms_1";
        std::ostringstream oss;
        oss << "Action: ConfbridgeListRooms\r\n"
            << "ActionID: " << actionId << "\r\n\r\n";

        auto res = sendActionCollect(oss.str(), actionId);

        std::vector<RoomInfo> rooms;
        for (const auto& ev : res.events) {
            auto itEv = ev.find("Event");
            if (itEv == ev.end()) continue;
            if (toLower(itEv->second) != "confbridgelistrooms") continue;

            RoomInfo r;
            r.conference = ev.count("Conference") ? ev.at("Conference") : "";
            if (ev.count("Parties")) r.parties = std::stoi(ev.at("Parties"));
            if (ev.count("Marked")) r.marked = std::stoi(ev.at("Marked"));
            r.locked = ev.count("Locked") ? ev.at("Locked") : "";
            r.muted  = ev.count("Muted")  ? ev.at("Muted")  : "";
            if (!r.conference.empty()) rooms.push_back(r);
        }

        std::sort(rooms.begin(), rooms.end(), [](const RoomInfo& a, const RoomInfo& b) {
            return a.conference < b.conference;
        });

        return rooms;
    }

    struct ParticipantInfo {
        std::string channel;
        std::string cidNum;
        std::string cidName;
        std::string admin;
        std::string markedUser;
    };

    std::vector<ParticipantInfo> listParticipants(const std::string& conference) {
        std::string actionId = "sb_users_1";
        std::ostringstream oss;
        oss << "Action: ConfbridgeList\r\n"
            << "ActionID: " << actionId << "\r\n"
            << "Conference: " << conference << "\r\n\r\n";

        auto res = sendActionCollect(oss.str(), actionId);

        std::vector<ParticipantInfo> users;
        for (const auto& ev : res.events) {
            auto itEv = ev.find("Event");
            if (itEv == ev.end()) continue;
            if (toLower(itEv->second) != "confbridgelist") continue;

            ParticipantInfo p;
            p.channel = ev.count("Channel") ? ev.at("Channel") : "";
            p.cidNum = ev.count("CallerIDNum") ? ev.at("CallerIDNum") : "";
            p.cidName = ev.count("CallerIDName") ? ev.at("CallerIDName") : "";
            p.admin = ev.count("Admin") ? ev.at("Admin") : "";
            p.markedUser = ev.count("MarkedUser") ? ev.at("MarkedUser") : "";
            users.push_back(p);
        }
        return users;
    }

    bool confbridgePlaySound(const std::string& conference, const std::string& soundNameNoExt) {
        std::string actionId = "sb_play_1";
        std::ostringstream oss;
        oss << "Action: ConfbridgePlaySound\r\n"
            << "ActionID: " << actionId << "\r\n"
            << "Conference: " << conference << "\r\n"
            << "File: " << soundNameNoExt << "\r\n\r\n";

        auto res = sendActionCollect(oss.str(), actionId, 200);
        if (!res.response.count("Response")) return false;
        return toLower(res.response["Response"]) == "success";
    }

private:
    int sock_;
    std::string rxbuf_;

    void sendAll(const std::string& data) {
        const char* p = data.c_str();
        size_t left = data.size();
        while (left > 0) {
            ssize_t n = send(sock_, p, left, 0);
            if (n <= 0) throw std::runtime_error(std::string("send() failed: ") + std::strerror(errno));
            p += n;
            left -= static_cast<size_t>(n);
        }
    }

    std::string readLine() {
        while (true) {
            auto pos = rxbuf_.find('\n');
            if (pos != std::string::npos) {
                std::string line = rxbuf_.substr(0, pos + 1);
                rxbuf_.erase(0, pos + 1);
                return line;
            }
            char buf[4096];
            ssize_t n = recv(sock_, buf, sizeof(buf), 0);
            if (n <= 0) throw std::runtime_error("recv() failed or connection closed");
            rxbuf_.append(buf, static_cast<size_t>(n));
        }
    }

    std::map<std::string, std::string> readMessage() {
        std::map<std::string, std::string> msg;
        while (true) {
            std::string line = readLine();
            if (line == "\r\n" || line == "\n" || line.empty()) break;
            auto pos = line.find(':');
            if (pos == std::string::npos) continue;
            std::string key = trim(line.substr(0, pos));
            std::string val = trim(line.substr(pos + 1));
            if (!val.empty() && val.back() == '\r') val.pop_back();
            msg[key] = val;
        }
        return msg;
    }
};

struct SoundEntry {
    std::string name;   // Asterisk sound name (no ext), like custom/airhorn
    std::string path;   // full filesystem path
    std::string ext;    // wav, ulaw, etc
};

static bool hasAllowedExt(const std::string& ext, const std::unordered_set<std::string>& allowed) {
    std::string e = toLower(ext);
    if (!e.empty() && e.front() == '.') e.erase(e.begin());
    return allowed.count(e) > 0;
}

static std::vector<SoundEntry> indexSounds(const std::vector<std::string>& roots,
                                          const std::unordered_set<std::string>& allowedExts) {
    std::vector<SoundEntry> out;

    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;

        for (auto const& de : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) continue;
            if (!de.is_regular_file(ec)) continue;
            std::string ext = de.path().extension().string();
            if (!hasAllowedExt(ext, allowedExts)) continue;

            // name = relative path from root, with forward slashes, without extension
            std::error_code ec2;
            fs::path rel = fs::relative(de.path(), root, ec2);
            if (ec2) continue;

            std::string name = rel.generic_string();
            // strip extension
            if (name.size() >= ext.size()) name.erase(name.size() - ext.size());

            // normalize
            while (!name.empty() && name.front() == '/') name.erase(name.begin());
            name = trim(name);
            if (name.empty()) continue;

            SoundEntry se;
            se.name = name;
            se.path = de.path().string();
            se.ext  = toLower(ext.empty() ? "" : ext.substr(1));
            out.push_back(se);
        }
    }

    std::sort(out.begin(), out.end(), [](const SoundEntry& a, const SoundEntry& b) {
        return a.name < b.name;
    });

    // de-dupe by sound name (prefer first found)
    std::vector<SoundEntry> dedup;
    std::unordered_set<std::string> seen;
    for (const auto& s : out) {
        if (seen.insert(s.name).second) dedup.push_back(s);
    }
    return dedup;
}

static int promptInt(const std::string& label, int minV, int maxV) {
    while (true) {
        std::cout << label;
        std::string in;
        if (!std::getline(std::cin, in)) return minV;
        in = trim(in);
        if (in.empty()) continue;
        try {
            int v = std::stoi(in);
            if (v >= minV && v <= maxV) return v;
        } catch (...) {}
        std::cout << "Invalid selection.\n";
    }
}

static std::string promptLine(const std::string& label) {
    std::cout << label;
    std::string in;
    std::getline(std::cin, in);
    return trim(in);
}

static void printRooms(const std::vector<AmiClient::RoomInfo>& rooms) {
    if (rooms.empty()) {
        std::cout << "No active ConfBridge rooms.\n";
        return;
    }
    std::cout << "Active rooms:\n";
    for (size_t i = 0; i < rooms.size(); i++) {
        const auto& r = rooms[i];
        std::cout << "  " << (i + 1) << ") " << r.conference
                  << "  parties=" << r.parties
                  << "  marked=" << r.marked
                  << "  locked=" << (r.locked.empty() ? "-" : r.locked)
                  << "  muted=" << (r.muted.empty() ? "-" : r.muted)
                  << "\n";
    }
}

static void printSoundsPage(const std::vector<SoundEntry>& sounds, size_t start, size_t pageSize) {
    size_t end = std::min(start + pageSize, sounds.size());
    for (size_t i = start; i < end; i++) {
        const auto& s = sounds[i];
        std::cout << "  " << (i + 1) << ") " << s.name << "  (" << s.ext << ")\n";
    }
    std::cout << "Showing " << (start + 1) << "-" << end << " of " << sounds.size() << "\n";
}

static std::vector<size_t> searchSounds(const std::vector<SoundEntry>& sounds, const std::string& q) {
    std::vector<size_t> idx;
    std::string needle = toLower(q);
    for (size_t i = 0; i < sounds.size(); i++) {
        if (toLower(sounds[i].name).find(needle) != std::string::npos) idx.push_back(i);
    }
    return idx;
}

int main(int argc, char** argv) {
    const std::string cfgPath = (argc > 1) ? argv[1] : "/etc/asterisk/soundboard.conf";

    try {
        auto cfg = readConfig(cfgPath);

        std::string host = cfg.count("host") ? cfg["host"] : "127.0.0.1";
        int port = cfg.count("port") ? std::stoi(cfg["port"]) : 5038;
        std::string user = cfg.count("username") ? cfg["username"] : "";
        std::string secret = cfg.count("secret") ? cfg["secret"] : "";

        if (user.empty() || secret.empty()) {
            std::cerr << "Config must contain username and secret.\n";
            return 1;
        }

        std::vector<std::string> roots = cfg.count("sound_roots")
            ? splitCsv(cfg["sound_roots"])
            : std::vector<std::string>{"/var/lib/asterisk/sounds"};

        std::unordered_set<std::string> exts;
        for (const auto& e : (cfg.count("extensions") ? splitCsv(cfg["extensions"]) : splitCsv("wav,ulaw,gsm,alaw,sln,sln16"))) {
            exts.insert(toLower(e));
        }

        std::unordered_set<std::string> favoritesSet;
        std::vector<std::string> favorites;
        if (cfg.count("favorites")) {
            favorites = splitCsv(cfg["favorites"]);
            for (auto& f : favorites) {
                f = trim(f);
                if (!f.empty()) favoritesSet.insert(f);
            }
        }

        // Index sound library
        auto sounds = indexSounds(roots, exts);
        std::unordered_map<std::string, SoundEntry> byName;
        byName.reserve(sounds.size());
        for (const auto& s : sounds) byName[s.name] = s;

        AmiClient ami;
        ami.connectTo(host, port);
        if (!ami.login(user, secret)) {
            std::cerr << "AMI login failed.\n";
            return 1;
        }

        bool canPlay = ami.supportsAction("ConfbridgePlaySound");
        if (!canPlay) {
            std::cout << "Warning: AMI does not advertise ConfbridgePlaySound via ListCommands.\n";
            std::cout << "The tool can still browse rooms and sounds, but play will likely fail.\n";
        }

        std::string selectedRoom;
        std::string selectedSound;

        std::vector<std::string> recents; // last N played
        const size_t recentsMax = 10;

        while (true) {
            std::cout << "\nAsterisk ConfBridge Soundboard (Advanced)\n";
            std::cout << "Selected room : " << (selectedRoom.empty() ? "-" : selectedRoom) << "\n";
            std::cout << "Selected sound: " << (selectedSound.empty() ? "-" : selectedSound) << "\n\n";

            std::cout << "1) List rooms\n";
            std::cout << "2) Select room\n";
            std::cout << "3) List participants in selected room\n";
            std::cout << "4) Browse sounds\n";
            std::cout << "5) Search sounds\n";
            std::cout << "6) Select sound by exact name\n";
            std::cout << "7) Play selected sound into selected room\n";
            std::cout << "8) Favorites quick play\n";
            std::cout << "9) Batch play (sequence)\n";
            std::cout << "10) Show recents\n";
            std::cout << "11) Re-index sound library\n";
            std::cout << "12) Quit\n";

            int choice = promptInt("Select: ", 1, 12);

            if (choice == 1) {
                auto rooms = ami.listRooms();
                printRooms(rooms);

            } else if (choice == 2) {
                auto rooms = ami.listRooms();
                printRooms(rooms);
                if (rooms.empty()) {
                    selectedRoom = promptLine("Enter room manually: ");
                } else {
                    std::cout << "  " << (rooms.size() + 1) << ") Enter manually\n";
                    int pick = promptInt("Choose: ", 1, static_cast<int>(rooms.size() + 1));
                    if (pick == static_cast<int>(rooms.size() + 1)) {
                        selectedRoom = promptLine("Enter room: ");
                    } else {
                        selectedRoom = rooms[static_cast<size_t>(pick - 1)].conference;
                    }
                }

            } else if (choice == 3) {
                if (selectedRoom.empty()) {
                    std::cout << "Select a room first.\n";
                    continue;
                }
                auto users = ami.listParticipants(selectedRoom);
                if (users.empty()) {
                    std::cout << "No participants found (or permission denied).\n";
                } else {
                    std::cout << "Participants in " << selectedRoom << ":\n";
                    for (const auto& u : users) {
                        std::cout << "  channel=" << u.channel
                                  << "  cid=" << u.cidNum << "/" << u.cidName
                                  << "  admin=" << u.admin
                                  << "  marked=" << u.markedUser
                                  << "\n";
                    }
                }

            } else if (choice == 4) {
                if (sounds.empty()) {
                    std::cout << "No sounds indexed. Check sound_roots and extensions.\n";
                    continue;
                }
                size_t page = 0;
                const size_t pageSize = 25;

                while (true) {
                    size_t start = page * pageSize;
                    if (start >= sounds.size()) page = 0;

                    std::cout << "\nSounds (page " << (page + 1) << ")\n";
                    printSoundsPage(sounds, page * pageSize, pageSize);
                    std::cout << "Commands: n=next p=prev s=<#> select q=quit\n";
                    std::string cmd = promptLine("> ");
                    if (cmd == "q") break;
                    if (cmd == "n") { page++; continue; }
                    if (cmd == "p") { if (page > 0) page--; continue; }

                    // select by number
                    try {
                        int num = std::stoi(cmd);
                        if (num >= 1 && static_cast<size_t>(num) <= sounds.size()) {
                            selectedSound = sounds[static_cast<size_t>(num - 1)].name;
                            std::cout << "Selected sound: " << selectedSound << "\n";
                            break;
                        }
                    } catch (...) {}
                    std::cout << "Unknown command.\n";
                }

            } else if (choice == 5) {
                if (sounds.empty()) {
                    std::cout << "No sounds indexed.\n";
                    continue;
                }
                std::string q = promptLine("Search substring: ");
                if (q.empty()) continue;

                auto hits = searchSounds(sounds, q);
                if (hits.empty()) {
                    std::cout << "No matches.\n";
                    continue;
                }
                std::cout << "Matches:\n";
                for (size_t i = 0; i < hits.size() && i < 50; i++) {
                    const auto& s = sounds[hits[i]];
                    std::cout << "  " << (i + 1) << ") " << s.name << " (" << s.ext << ")\n";
                }
                int pick = promptInt("Select match # (0 to cancel): ", 0, static_cast<int>(std::min<size_t>(50, hits.size())));
                if (pick == 0) continue;
                selectedSound = sounds[hits[static_cast<size_t>(pick - 1)]].name;
                std::cout << "Selected sound: " << selectedSound << "\n";

            } else if (choice == 6) {
                std::string name = promptLine("Enter exact sound name (example: custom/airhorn): ");
                if (name.empty()) continue;
                if (!byName.count(name)) {
                    std::cout << "Warning: not found in indexed library, will still attempt playback by name.\n";
                }
                selectedSound = name;

            } else if (choice == 7) {
                if (selectedRoom.empty() || selectedSound.empty()) {
                    std::cout << "Select both a room and a sound first.\n";
                    continue;
                }
                if (!canPlay) {
                    std::cout << "ConfbridgePlaySound does not appear to be supported by this AMI instance.\n";
                    std::cout << "If you are sure your system supports it, you can still try, but it may fail.\n";
                }

                bool ok = ami.confbridgePlaySound(selectedRoom, selectedSound);
                if (ok) {
                    std::cout << "Played " << selectedSound << " into " << selectedRoom << "\n";
                    recents.erase(std::remove(recents.begin(), recents.end(), selectedSound), recents.end());
                    recents.insert(recents.begin(), selectedSound);
                    if (recents.size() > recentsMax) recents.resize(recentsMax);
                } else {
                    std::cout << "Playback failed.\n";
                    std::cout << "Confirm the room is active, AMI permissions allow ConfBridge actions, and the sound name is correct.\n";
                }

            } else if (choice == 8) {
                if (favorites.empty()) {
                    std::cout << "No favorites configured.\n";
                    continue;
                }
                std::cout << "Favorites:\n";
                for (size_t i = 0; i < favorites.size(); i++) {
                    std::cout << "  " << (i + 1) << ") " << favorites[i] << "\n";
                }
                int pick = promptInt("Pick favorite (0 cancel): ", 0, static_cast<int>(favorites.size()));
                if (pick == 0) continue;
                selectedSound = favorites[static_cast<size_t>(pick - 1)];
                std::cout << "Selected sound: " << selectedSound << "\n";

                if (!selectedRoom.empty()) {
                    std::string ans = promptLine("Play now into selected room? (y/n): ");
                    if (toLower(ans) == "y") {
                        bool ok = ami.confbridgePlaySound(selectedRoom, selectedSound);
                        std::cout << (ok ? "Played.\n" : "Failed.\n");
                    }
                }

            } else if (choice == 9) {
                if (selectedRoom.empty()) {
                    std::cout << "Select a room first.\n";
                    continue;
                }
                std::cout << "Enter a comma-separated list of sound names to play in order.\n";
                std::cout << "Example: custom/airhorn,custom/applause\n";
                std::string line = promptLine("Sequence: ");
                auto seq = splitCsv(line);
                if (seq.empty()) continue;

                int delayMs = promptInt("Delay between sounds in ms (0-30000): ", 0, 30000);

                for (const auto& s : seq) {
                    if (s.empty()) continue;
                    std::cout << "Playing " << s << "...\n";
                    bool ok = ami.confbridgePlaySound(selectedRoom, s);
                    std::cout << (ok ? "  OK\n" : "  FAILED\n");
                    if (delayMs > 0) usleep(static_cast<useconds_t>(delayMs) * 1000);
                }

            } else if (choice == 10) {
                if (recents.empty()) {
                    std::cout << "No recents.\n";
                } else {
                    std::cout << "Recents:\n";
                    for (size_t i = 0; i < recents.size(); i++) {
                        std::cout << "  " << (i + 1) << ") " << recents[i] << "\n";
                    }
                }

            } else if (choice == 11) {
                sounds = indexSounds(roots, exts);
                byName.clear();
                for (const auto& s : sounds) byName[s.name] = s;
                std::cout << "Re-indexed. Sounds found: " << sounds.size() << "\n";

            } else if (choice == 12) {
                break;
            }
        }

        ami.logoff();
        return 0;

    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
