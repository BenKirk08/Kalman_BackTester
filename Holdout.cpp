#include "Holdout.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static bool unsealed = false;

bool holdoutUnsealed() { return unsealed; }

//where the file really is: symlinks followed, ".." removed, lower case (the project disk ignores case, so
//Data/Input/Stooq and Data/Input/stooq are the same folder and must be treated the same)
static std::string realLocation(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path full = std::filesystem::weakly_canonical(p, ec);
    if (ec) full = std::filesystem::absolute(p, ec);
    std::string s = full.lexically_normal().string();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

int holdoutCutoff(const std::string& file) {
    if (unsealed) return 0;
    //exempt only by real location. the program is always run from the project folder (its data paths are relative)
    static const std::string handFiles = realLocation("Data/Input");
    static const std::string synthetic = realLocation("Data/Input/synthetic");
    static const std::string simulated = realLocation("Data/Output/montecarlo/paths_tmp");
    const std::filesystem::path p(file);
    const std::string folder = realLocation(p.parent_path().empty() ? std::filesystem::path(".") : p.parent_path());
    if (folder == handFiles || folder == synthetic) return 0;
    if (folder == simulated || folder.compare(0, simulated.size() + 1, simulated + "/") == 0) return 0;
    return HOLDOUT_LAST_DEV_DATE;
}

//runs a command, returns what it printed (trailing newline removed). ok = it ran and exited 0
static std::string capture(const std::string& command, bool& ok) {
    std::string out;
    ok = false;
    std::FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
    ok = pclose(pipe) == 0;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

static std::string quoted(const std::string& s) {
    std::string q = "'";
    for (char c : s) { if (c == '\'') q += "'\\''"; else q += c; }
    return q + "'";
}

void unsealHoldout(const std::string& commandLine, const std::string& pairList) {
    bool ok = false;

    //a fingerprint of the pair list -> pins the universe it was run on
    std::string fingerprint = "-";
    if (!pairList.empty()) {
        const std::string sha = capture("shasum -a 256 " + quoted(pairList) + " 2>/dev/null", ok);
        if (ok && sha.size() >= 16) fingerprint = sha.substr(0, 16);
    }

    std::time_t now = std::time(nullptr);
    char when[32];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    std::ofstream log("HOLDOUT_UNSEAL.log", std::ios::app);
    if (!log) throw std::runtime_error("--unseal-holdout: couldnt write HOLDOUT_UNSEAL.log");
    log << when << "  pairs " << fingerprint << "  " << commandLine << "\n";

    std::cerr << "\n*** HOLD-OUT UNSEALED: this run reads prices after " << HOLDOUT_LAST_DEV_DATE_TEXT
              << " (logged in HOLDOUT_UNSEAL.log) ***\n\n";
    unsealed = true;
}
