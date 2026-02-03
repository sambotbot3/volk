#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

class volk_modtool {
public:
    explicit volk_modtool(std::unordered_map<std::string, std::string> cfg)
        : volk("volk"),
          remove_after_underscore("_.*"),
          volk_included("INCLUDED_VOLK"),
          volk_profile(R"(^\s*(VOLK_PROFILE|VOLK_PUPPET_PROFILE).*\n)",
                       std::regex_constants::ECMAScript | std::regex_constants::multiline),
          volk_kernel_tests(R"(^\s*\((VOLK_INIT_TEST|VOLK_INIT_PUPP).*\n)",
                            std::regex_constants::ECMAScript | std::regex_constants::multiline),
          volk_null_kernel(R"(^\s*;\n)",
                           std::regex_constants::ECMAScript | std::regex_constants::multiline),
          my_dict(std::move(cfg)),
          lastline(R"(\s*char path\[1024\];.*)"),
          badassert(R"(^\s*assert\(toked\[0\] == "volk_.*\n)",
                    std::regex_constants::ECMAScript | std::regex_constants::multiline),
          baderase(R"(^\s*toked.erase\(toked.begin\(\)\);.*\n)",
                   std::regex_constants::ECMAScript | std::regex_constants::multiline)
    {
    }

    std::string get_basename(const std::string& base = "") const
    {
        std::string resolved_base = base.empty() ? at("base") : base;
        std::string candidate = fs::path(resolved_base).filename().string();
        std::size_t pos = candidate.rfind('_');
        if (pos == std::string::npos) {
            return "";
        }
        return candidate.substr(pos + 1);
    }

    std::unordered_set<std::string> get_current_kernels(const std::string& base = "") const
    {
        std::string resolved_base = base.empty() ? at("base") : base;
        std::string name = base.empty() ? get_basename() : get_basename(base);

        fs::path header_dir;
        std::string prefix;
        if (name.empty()) {
            header_dir = fs::path(resolved_base) / "kernels" / "volk";
            prefix = "volk_";
        } else {
            header_dir = fs::path(resolved_base) / "kernels" / ("volk_" + name);
            prefix = "volk_" + name + "_";
        }

        std::vector<fs::path> hdr_files;
        if (fs::exists(header_dir)) {
            for (const auto& entry : fs::directory_iterator(header_dir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                if (entry.path().extension() == ".h") {
                    hdr_files.push_back(entry.path());
                }
            }
        }
        std::sort(hdr_files.begin(), hdr_files.end());

        std::unordered_set<std::string> datatypes;
        std::unordered_set<std::string> functions;
        std::regex dtype_re("[0-9]+[A-z]+");

        for (const auto& path : hdr_files) {
            std::string filename = path.filename().string();
            if (filename.rfind(prefix, 0) != 0) {
                continue;
            }
            std::string base_name = filename.substr(prefix.size());
            if (ends_with(base_name, ".h")) {
                base_name = base_name.substr(0, base_name.size() - 2);
            }
            std::string dtype = base_name;
            std::size_t pos = dtype.find('_');
            if (pos != std::string::npos) {
                dtype = dtype.substr(0, pos);
            }
            std::smatch match;
            if (std::regex_search(dtype, match, dtype_re)) {
                datatypes.insert(match.str());
            }
        }

        for (const auto& path : hdr_files) {
            std::string filename = path.string();
            for (const auto& dt : datatypes) {
                if (filename.find(dt) == std::string::npos) {
                    continue;
                }
                std::string base_name = path.filename().string();
                if (base_name.rfind(prefix, 0) != 0) {
                    continue;
                }
                base_name = base_name.substr(prefix.size());
                if (ends_with(base_name, ".h")) {
                    base_name = base_name.substr(0, base_name.size() - 2);
                }
                functions.insert(base_name);
            }
        }

        return functions;
    }

    void make_module_skeleton()
    {
        fs::path dest = fs::path(at("destination")) / ("volk_" + at("name"));
        if (fs::exists(dest)) {
            throw std::runtime_error("Destination " + dest.string() + " already exits!");
        }

        fs::path kernel_dir = dest / "kernels" / ("volk_" + at("name"));
        if (!fs::exists(kernel_dir)) {
            fs::create_directories(kernel_dir);
        }

        std::unordered_set<std::string> current_kernel_names = get_current_kernels();
        std::vector<std::string> need_ifdef_updates = { "constant.h",       "volk_complex.h",
                                                        "volk_malloc.h",   "volk_prefs.h",
                                                        "volk_common.h",   "volk_cpu.tmpl.h",
                                                        "volk_config_fixed.tmpl.h",
                                                        "volk_typedefs.h", "volk.tmpl.h" };

        fs::path base_path = at("base");
        for (const auto& entry : fs::recursive_directory_iterator(base_path)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            std::string name = entry.path().filename().string();
            bool matches_kernel = false;
            for (const auto& kernel : current_kernel_names) {
                if (name.find(kernel) != std::string::npos) {
                    matches_kernel = true;
                    break;
                }
            }

            if (!matches_kernel || name == "volk_32f_null_32f.h") {
                fs::path infile = entry.path();
                std::string instring = read_file(infile);
                std::string outstring = std::regex_replace(instring, volk,
                                                           "volk_" + at("name"));

                if (contains(need_ifdef_updates, name)) {
                    outstring = std::regex_replace(outstring, volk_included,
                                                   "INCLUDED_VOLK_" + upper(at("name")));
                }

                std::string newname = std::regex_replace(name, volk, "volk_" + at("name"));
                if (name == "VolkConfig.cmake.in") {
                    outstring = std::regex_replace(outstring, std::regex("VOLK"),
                                                   "VOLK_" + upper(at("name")));
                    newname = "Volk" + at("name") + "Config.cmake.in";
                }

                fs::path relpath = fs::relative(infile, base_path);
                std::string newrelpath = std::regex_replace(relpath.string(), volk,
                                                           "volk_" + at("name"));
                fs::path destpath = fs::path(at("destination")) / ("volk_" + at("name")) /
                                    fs::path(newrelpath).parent_path() / newname;

                fs::create_directories(destpath.parent_path());
                write_file(destpath, outstring);
            }
        }

        fs::path kernel_tests = dest / "lib" / "kernel_tests.h";
        std::string test_string = read_file(kernel_tests);
        std::string outstring = std::regex_replace(test_string, volk_kernel_tests, "");
        outstring = std::regex_replace(outstring, volk_null_kernel,
                                       "        (VOLK_INIT_TEST(volk_" + at("name") +
                                           "_32f_null_32f, test_params))\n        ;");
        write_file(kernel_tests, outstring);

        fs::path qa_utils = dest / "lib" / "qa_utils.cc";
        std::string qa_string = read_file(qa_utils);
        qa_string = std::regex_replace(qa_string, badassert,
                                       "    assert(toked[0] == \"volk\");\n");
        qa_string = std::regex_replace(qa_string, baderase,
                                       "    toked.erase(toked.begin());\n    toked.erase(toked.begin());\n");
        write_file(qa_utils, qa_string);
    }

    void write_default_cfg(const std::string& cfg_contents)
    {
        fs::path outfile = fs::path(at("destination")) / ("volk_" + at("name")) /
                           "volk_modtool.cfg";
        write_file(outfile, cfg_contents);
    }

    void convert_kernel(const std::regex& oldvolk,
                        const std::string& name,
                        const std::string& base,
                        const std::string& inpath,
                        const std::string& top)
    {
        fs::path infile = fs::path(inpath) / "kernels" / top.substr(0, top.size() - 1) /
                          (top + name + ".h");
        std::string instring = read_file(infile);
        std::string outstring = std::regex_replace(instring, oldvolk, "volk_" + at("name"));
        std::string newname = "volk_" + at("name") + "_" + name + ".h";
        fs::path relpath = fs::relative(infile, base);
        std::string newrelpath = std::regex_replace(relpath.string(), oldvolk,
                                                   "volk_" + at("name"));
        fs::path dest = fs::path(at("destination")) / ("volk_" + at("name")) /
                        fs::path(newrelpath).parent_path() / newname;

        fs::create_directories(dest.parent_path());
        write_file(dest, outstring);

        fs::path orc_dir = fs::path(inpath) / "kernels" / "volk" / "asm" / "orc";
        if (fs::exists(orc_dir)) {
            std::vector<fs::path> orc_files;
            for (const auto& entry : fs::directory_iterator(orc_dir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                std::string filename = entry.path().filename().string();
                if (starts_with(filename, top + name) && ends_with(filename, ".orc")) {
                    orc_files.push_back(entry.path());
                }
            }
            std::sort(orc_files.begin(), orc_files.end());
            for (const auto& orcfile : orc_files) {
                std::string orc_string = read_file(orcfile);
                std::string out_orc = std::regex_replace(orc_string, oldvolk,
                                                         "volk_" + at("name"));
                std::string orc_name = "volk_" + at("name") + "_" + name + ".orc";
                fs::path rel_orc = fs::relative(orcfile, base);
                std::string newrel_orc = std::regex_replace(rel_orc.string(), oldvolk,
                                                           "volk_" + at("name"));
                fs::path dest_orc = fs::path(at("destination")) / ("volk_" + at("name")) /
                                   fs::path(newrel_orc).parent_path() / orc_name;
                fs::create_directories(dest_orc.parent_path());
                write_file(dest_orc, out_orc);
            }
        }
    }

    void remove_kernel(const std::string& name)
    {
        std::string basename = at("name");
        std::string top = basename.empty() ? "volk_" : "volk_" + basename + "_";
        fs::path base = fs::path(at("destination")) / top.substr(0, top.size() - 1);

        if (get_current_kernels().count(name) == 0) {
            throw std::runtime_error("Requested kernel " + name + " is not in module " +
                                     base.string());
        }

        fs::path inpath = fs::absolute(base);
        std::vector<std::string> search_kernels;
        search_kernels.push_back(name);
        std::regex puppet(R"(^\s*VOLK_PUPPET)");

        fs::path profile_path = inpath / "apps" / (top.substr(0, top.size() - 1) +
                                                   "_profile.cc");
        std::vector<std::string> profile_lines = split_lines_keep_ends(read_file(profile_path));
        std::ofstream profile_out(profile_path, std::ios::trunc);

        for (const auto& line : profile_lines) {
            bool write_okay = true;
            if (std::regex_search(line, std::regex(name))) {
                write_okay = false;
                if (std::regex_search(line, puppet)) {
                    std::smatch args_match;
                    if (std::regex_search(line, args_match,
                                          std::regex(R"((?<=VOLK_PUPPET_PROFILE).*)"))) {
                        std::string m_func = args_match.str();
                        std::string func_part = split(m_func, ',').front();
                        std::size_t pos = func_part.find(top);
                        if (pos != std::string::npos) {
                            search_kernels.push_back(func_part.substr(pos + top.size()));
                        }
                    }
                }
            }
            if (write_okay) {
                profile_out << line;
            }
        }

        fs::path testqa_path = inpath / "lib" / "testqa.cc";
        std::vector<std::string> testqa_lines = split_lines_keep_ends(read_file(testqa_path));
        std::ofstream testqa_out(testqa_path, std::ios::trunc);
        for (const auto& line : testqa_lines) {
            bool write_okay = true;
            for (const auto& kernel : search_kernels) {
                if (std::regex_search(line, std::regex(kernel))) {
                    write_okay = false;
                    break;
                }
            }
            if (write_okay) {
                testqa_out << line;
            }
        }

        for (const auto& kernel : search_kernels) {
            fs::path kernel_path = inpath / "kernels" / top.substr(0, top.size() - 1) /
                                   (top + kernel + ".h");
            std::cout << "Removing kernel " << kernel << std::endl;
            if (fs::exists(kernel_path)) {
                fs::remove(kernel_path);
            }
        }

        fs::path orc_dir = inpath / "kernel" / "volk" / "asm" / "orc";
        if (fs::exists(orc_dir)) {
            for (const auto& entry : fs::directory_iterator(orc_dir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                std::string filename = entry.path().filename().string();
                if (starts_with(filename, top + name) && ends_with(filename, ".orc")) {
                    std::cout << entry.path().string() << std::endl;
                    fs::remove(entry.path());
                }
            }
        }
    }

    void import_kernel(const std::string& name, const std::string& base = "")
    {
        std::string resolved_base = base.empty() ? at("base") : base;
        std::string basename = base.empty() ? get_basename() : get_basename(base);

        if (get_current_kernels(resolved_base).count(name) == 0) {
            throw std::runtime_error("Requested kernel " + name + " is not in module " +
                                     resolved_base);
        }

        fs::path inpath = fs::absolute(resolved_base);
        std::string top = basename.empty() ? "volk_" : "volk_" + basename + "_";
        std::string oldvolk_pattern = top.substr(0, top.size() - 1);
        std::regex oldvolk(oldvolk_pattern);

        convert_kernel(oldvolk, name, resolved_base, inpath.string(), top);

        std::vector<std::string> search_kernels;
        search_kernels.push_back(name);

        std::regex profile(R"(^\s*VOLK_PROFILE)");
        std::regex puppet(R"(^\s*VOLK_PUPPET)");

        fs::path src_profile = inpath / "apps" / (oldvolk_pattern + "_profile.cc");
        fs::path dst_profile = fs::path(at("destination")) / ("volk_" + at("name")) /
                               "apps" / ("volk_" + at("name") + "_profile.cc");

        std::vector<std::string> lines = split_lines_keep_ends(read_file(src_profile));
        std::vector<std::string> otherlines = split_lines_keep_ends(read_file(dst_profile));
        std::ofstream profile_out(dst_profile, std::ios::trunc);

        bool insert = false;
        bool inserted = false;
        for (const auto& otherline : otherlines) {
            if (std::regex_match(otherline, lastline)) {
                insert = true;
            }
            if (insert && !inserted) {
                inserted = true;
                for (const auto& line : lines) {
                    if (std::regex_search(line, std::regex(name))) {
                        if (std::regex_search(line, profile) || std::regex_search(line, puppet)) {
                            std::string outline = std::regex_replace(
                                line, oldvolk, "volk_" + at("name"));
                            profile_out << outline;
                            if (std::regex_search(line, puppet)) {
                                std::smatch args_match;
                                if (std::regex_search(
                                        line, args_match,
                                        std::regex(R"((?<=VOLK_PUPPET_PROFILE).*)"))) {
                                    std::string m_func = args_match.str();
                                    std::string func_part = split(m_func, ',').front();
                                    std::size_t pos = func_part.find(top);
                                    if (pos != std::string::npos) {
                                        std::string func = func_part.substr(pos + top.size());
                                        search_kernels.push_back(func);
                                        convert_kernel(oldvolk,
                                                       func,
                                                       resolved_base,
                                                       inpath.string(),
                                                       top);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            bool write_okay = true;
            for (const auto& kernel : search_kernels) {
                if (std::regex_search(otherline, std::regex(kernel))) {
                    write_okay = false;
                    break;
                }
            }
            if (write_okay) {
                profile_out << otherline;
            }
        }

        for (const auto& kernel : search_kernels) {
            std::cout << "Adding kernel " << kernel
                      << " from module " << resolved_base << std::endl;
        }

        fs::path src_testqa = inpath / "lib" / "testqa.cc";
        fs::path dst_testqa = fs::path(at("destination")) / ("volk_" + at("name")) /
                              "lib" / "testqa.cc";
        std::vector<std::string> src_lines = split_lines_keep_ends(read_file(src_testqa));
        std::vector<std::string> dst_lines = split_lines_keep_ends(read_file(dst_testqa));
        std::ofstream testqa_out(dst_testqa, std::ios::trunc);

        bool inserted_testqa = false;
        bool insert_testqa = false;
        for (const auto& otherline : dst_lines) {
            if (!std::regex_match(otherline, std::regex(R"(\s*)")) ||
                !std::regex_match(otherline, std::regex(R"(\s*#.*)"))) {
                insert_testqa = true;
            }
            if (insert_testqa && !inserted_testqa) {
                inserted_testqa = true;
                for (const auto& line : src_lines) {
                    for (const auto& kernel : search_kernels) {
                        if (std::regex_search(line, std::regex(kernel))) {
                            if (std::regex_search(line, volk_run_tests)) {
                                std::string outline = std::regex_replace(
                                    line, oldvolk, "volk_" + at("name"));
                                testqa_out << outline;
                            }
                        }
                    }
                }
            }

            bool write_okay = true;
            for (const auto& kernel : search_kernels) {
                if (std::regex_search(otherline, std::regex(kernel))) {
                    write_okay = false;
                    break;
                }
            }
            if (write_okay) {
                testqa_out << otherline;
            }
        }
    }

private:
    std::regex volk;
    std::regex remove_after_underscore;
    std::regex volk_included;
    std::regex volk_profile;
    std::regex volk_kernel_tests;
    std::regex volk_null_kernel;
    std::regex lastline;
    std::regex badassert;
    std::regex baderase;
    std::regex volk_run_tests{ "run_volk_tests" };
    std::unordered_map<std::string, std::string> my_dict;

    const std::string& at(const std::string& key) const
    {
        auto it = my_dict.find(key);
        if (it == my_dict.end()) {
            throw std::runtime_error("Missing config key: " + key);
        }
        return it->second;
    }

    static bool starts_with(const std::string& value, const std::string& prefix)
    {
        return value.rfind(prefix, 0) == 0;
    }

    static bool ends_with(const std::string& value, const std::string& suffix)
    {
        if (suffix.size() > value.size()) {
            return false;
        }
        return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
    }

    static std::string read_file(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("Unable to open file: " + path.string());
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    static void write_file(const fs::path& path, const std::string& data)
    {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("Unable to write file: " + path.string());
        }
        out << data;
    }

    static std::vector<std::string> split(const std::string& value, char sep)
    {
        std::vector<std::string> parts;
        std::string current;
        std::istringstream stream(value);
        while (std::getline(stream, current, sep)) {
            parts.push_back(current);
        }
        return parts;
    }

    static std::vector<std::string> split_lines_keep_ends(const std::string& data)
    {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start < data.size()) {
            std::size_t pos = data.find('\n', start);
            if (pos == std::string::npos) {
                lines.push_back(data.substr(start));
                break;
            }
            lines.push_back(data.substr(start, pos - start + 1));
            start = pos + 1;
        }
        if (data.empty()) {
            return lines;
        }
        if (!data.empty() && data.back() == '\n' && (lines.empty() || lines.back() != "\n")) {
            return lines;
        }
        return lines;
    }

    static bool contains(const std::vector<std::string>& values, const std::string& target)
    {
        return std::find(values.begin(), values.end(), target) != values.end();
    }

    static std::string upper(const std::string& value)
    {
        std::string out = value;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return out;
    }

};

struct volk_modtool_config {
    std::string config_name = "config";
    std::string config_path;
    std::unordered_map<std::string, std::string> values;

    explicit volk_modtool_config(const std::string& cfg_path)
    {
        if (!cfg_path.empty()) {
            config_path = cfg_path;
        } else {
            config_path = (fs::current_path() / "volk_modtool.cfg").string();
        }

        if (fs::exists(config_path)) {
            read_config();
        } else {
            initialize_config();
        }

        remap();
        verify();
    }

    std::unordered_map<std::string, std::string> get_map() const
    {
        return values;
    }

    std::string serialize() const
    {
        std::ostringstream out;
        out << "[" << config_name << "]\n";
        for (const auto& key : {"name", "destination", "base"}) {
            auto it = values.find(key);
            if (it != values.end()) {
                out << key << " = " << it->second << "\n";
            }
        }
        return out.str();
    }

private:
    void read_config()
    {
        std::ifstream in(config_path);
        if (!in) {
            throw std::runtime_error("Unable to read config file: " + config_path);
        }

        std::string line;
        bool in_section = false;
        while (std::getline(in, line)) {
            std::string trimmed = trim(line);
            if (trimmed.empty() || trimmed[0] == '#') {
                continue;
            }
            if (trimmed == "[" + config_name + "]") {
                in_section = true;
                continue;
            }
            if (!in_section) {
                continue;
            }
            if (!trimmed.empty() && trimmed[0] == '[') {
                break;
            }
            std::size_t pos = trimmed.find('=');
            if (pos == std::string::npos) {
                continue;
            }
            std::string key = trim(trimmed.substr(0, pos));
            std::string value = trim(trimmed.substr(pos + 1));
            values[key] = value;
        }
    }

    void initialize_config()
    {
        std::cout << "Initializing config file..." << std::endl;
        for (const auto& key : {"name", "destination", "base"}) {
            std::cout << key << ": ";
            std::string value;
            std::getline(std::cin, value);
            values[key] = value;
        }
    }

    void remap()
    {
        values["destination"] = canonicalize_path(values["destination"]);
        values["base"] = canonicalize_path(values["base"]);
    }

    void verify() const
    {
        std::regex name_re("[a-zA-Z0-9]+$");
        auto name_it = values.find("name");
        if (name_it == values.end() || !std::regex_match(name_it->second, name_re)) {
            throw std::runtime_error("Invalid name in config: " +
                                     (name_it == values.end() ? "" : name_it->second));
        }

        auto dest_it = values.find("destination");
        if (dest_it == values.end() || !fs::exists(dest_it->second)) {
            throw std::runtime_error("Invalid destination in config: " +
                                     (dest_it == values.end() ? "" : dest_it->second));
        }

        auto base_it = values.find("base");
        if (base_it == values.end() || !fs::exists(base_it->second)) {
            throw std::runtime_error("Invalid base in config: " +
                                     (base_it == values.end() ? "" : base_it->second));
        }
    }

    static std::string canonicalize_path(const std::string& value)
    {
        std::string expanded = expand_user(value);
        fs::path path = expanded;
        try {
            return fs::weakly_canonical(path).string();
        } catch (const std::exception&) {
            return fs::absolute(path).string();
        }
    }

    static std::string expand_user(const std::string& value)
    {
        if (!value.empty() && value[0] == '~') {
            const char* home = std::getenv("HOME");
            if (home && value.size() == 1) {
                return std::string(home);
            }
            if (home && value.size() > 1 && value[1] == '/') {
                return std::string(home) + value.substr(1);
            }
        }
        return value;
    }

    static std::string trim(const std::string& value)
    {
        std::size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
            ++start;
        }
        std::size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
            --end;
        }
        return value.substr(start, end - start);
    }
};

struct cli_options {
    bool install = false;
    bool add_kernel = false;
    bool add_all_kernels = false;
    bool remove_kernel = false;
    bool list = false;
    bool kernels = false;
    bool remote_list = false;
    bool moo = false;
    std::string base_path;
    std::string kernel_name;
    std::string config_file;
};

static cli_options parse_args(int argc, char* argv[])
{
    cli_options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-i" || arg == "--install") {
            options.install = true;
        } else if (arg == "-a" || arg == "--add_kernel") {
            options.add_kernel = true;
        } else if (arg == "-A" || arg == "--add_all_kernels") {
            options.add_all_kernels = true;
        } else if (arg == "-x" || arg == "--remove_kernel") {
            options.remove_kernel = true;
        } else if (arg == "-l" || arg == "--list") {
            options.list = true;
        } else if (arg == "-k" || arg == "--kernels") {
            options.kernels = true;
        } else if (arg == "-r" || arg == "--remote_list") {
            options.remote_list = true;
        } else if (arg == "-m" || arg == "--moo") {
            options.moo = true;
        } else if (arg == "-b" || arg == "--base_path") {
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing value for --base_path");
            }
            options.base_path = argv[++i];
        } else if (arg == "-n" || arg == "--kernel_name") {
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing value for --kernel_name");
            }
            options.kernel_name = argv[++i];
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing value for --config");
            }
            options.config_file = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: volk_modtool [options]\n";
            std::cout << "  -i, --install            Create a new volk module.\n";
            std::cout << "  -a, --add_kernel         Add kernel from existing module. Requires -n.\n";
            std::cout << "  -A, --add_all_kernels     Add all kernels from existing module.\n";
            std::cout << "  -x, --remove_kernel       Remove kernel from module. Requires -n.\n";
            std::cout << "  -l, --list                List all kernels in the base.\n";
            std::cout << "  -k, --kernels             List all kernels in the module.\n";
            std::cout << "  -r, --remote_list         List all kernels in remote module. Requires -b.\n";
            std::cout << "  -m, --moo                 Print a cow.\n";
            std::cout << "  -b, --base_path <path>    Base path for action.\n";
            std::cout << "  -n, --kernel_name <name>  Kernel name for action.\n";
            std::cout << "  -c, --config <path>       Config file path.\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    return options;
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cout << "Usage: volk_modtool [options]\n";
        return 0;
    }

    cli_options options = parse_args(argc, argv);

    if (options.moo) {
        std::cout << "         (__)    \n";
        std::cout << "         (oo)    \n";
        std::cout << "   /------\\/     \n";
        std::cout << "  / |    ||      \n";
        std::cout << " *  /\\---/\\      \n";
        std::cout << "    ~~   ~~      \n";
        return 0;
    }

    volk_modtool_config config(options.config_file);
    volk_modtool tool(config.get_map());

    if (options.install) {
        tool.make_module_skeleton();
        tool.write_default_cfg(config.serialize());
    }

    if (options.add_kernel) {
        if (options.kernel_name.empty()) {
            throw std::runtime_error("This action requires the -n option.");
        }
        std::string base = options.base_path.empty() ? config.get_map().at("base")
                                                     : options.base_path;
        tool.import_kernel(options.kernel_name, base);
    }

    if (options.remove_kernel) {
        if (options.kernel_name.empty()) {
            throw std::runtime_error("This action requires the -n option.");
        }
        tool.remove_kernel(options.kernel_name);
    }

    if (options.add_all_kernels) {
        std::string base = options.base_path.empty() ? config.get_map().at("base")
                                                     : options.base_path;
        auto kernelset = tool.get_current_kernels(base);
        for (const auto& kernel : kernelset) {
            tool.import_kernel(kernel, base);
        }
    }

    if (options.remote_list) {
        if (options.base_path.empty()) {
            throw std::runtime_error(
                "This action requires the -b option. Try -l or -k for listing kernels in the base or the module.");
        }
        auto kernelset = tool.get_current_kernels(options.base_path);
        for (const auto& kernel : kernelset) {
            std::cout << kernel << std::endl;
        }
    }

    if (options.list) {
        auto kernelset = tool.get_current_kernels();
        for (const auto& kernel : kernelset) {
            std::cout << kernel << std::endl;
        }
    }

    if (options.kernels) {
        std::string dest = config.get_map().at("destination");
        std::string name = config.get_map().at("name");
        fs::path base = fs::path(dest) / ("volk_" + name);
        auto kernelset = tool.get_current_kernels(base.string());
        for (const auto& kernel : kernelset) {
            std::cout << kernel << std::endl;
        }
    }

    return 0;
}
