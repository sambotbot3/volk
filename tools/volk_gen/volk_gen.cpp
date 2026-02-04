/*
 * Copyright 2024 Free Software Foundation, Inc.
 *
 * This file is part of VOLK
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * C++ replacement for Python code generation scripts:
 * - gen/volk_compile_utils.py
 * - gen/volk_tmpl_utils.py
 * - gen/volk_arch_defs.py
 * - gen/volk_machine_defs.py
 * - gen/volk_kernel_defs.py
 */

#include <algorithm>
#include <cassert>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ============================================================================
// Architecture Definition
// ============================================================================

struct Arch {
    std::string name;
    std::string environment;
    std::string include;
    int alignment = 1;
    std::vector<std::pair<std::string, std::vector<std::string>>> checks;
    std::map<std::string, std::vector<std::string>> flags; // compiler -> flags

    bool is_supported(const std::string& compiler) const {
        if (flags.empty()) return true;
        return flags.find(compiler) != flags.end();
    }

    std::vector<std::string> get_flags(const std::string& compiler) const {
        auto it = flags.find(compiler);
        if (it != flags.end()) return it->second;
        return {};
    }
};

// ============================================================================
// Machine Definition
// ============================================================================

struct Machine {
    std::string name;
    std::vector<std::string> arch_names;
    std::vector<const Arch*> archs;
    int alignment = 1;
};

// ============================================================================
// Kernel Implementation
// ============================================================================

struct Impl {
    std::string name;
    std::set<std::string> deps;
    std::vector<std::pair<std::string, std::string>> args; // type, name
    bool is_aligned = false;
};

// ============================================================================
// Kernel Definition
// ============================================================================

struct Kernel {
    std::string name;
    std::string pname;
    std::vector<Impl> impls;
    std::vector<std::pair<std::string, std::string>> args;
    std::string arglist_types;
    std::string arglist_full;
    std::string arglist_names;
    bool has_dispatcher = false;

    std::vector<const Impl*> get_impls(const std::set<std::string>& arch_set) const {
        std::vector<const Impl*> result;
        for (const auto& impl : impls) {
            bool all_deps_satisfied = true;
            for (const auto& dep : impl.deps) {
                if (arch_set.find(dep) == arch_set.end()) {
                    all_deps_satisfied = false;
                    break;
                }
            }
            if (all_deps_satisfied) {
                result.push_back(&impl);
            }
        }
        return result;
    }
};

// ============================================================================
// Global Data
// ============================================================================

std::vector<Arch> g_archs;
std::map<std::string, Arch*> g_arch_dict;
std::vector<Machine> g_machines;
std::map<std::string, Machine*> g_machine_dict;
std::vector<Kernel> g_kernels;

// ============================================================================
// String Utilities
// ============================================================================

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

static std::string to_upper(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> result;
    size_t start = 0;
    size_t end;
    while ((end = s.find(delim, start)) != std::string::npos) {
        result.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    result.push_back(s.substr(start));
    return result;
}

static std::vector<std::string> split_whitespace(const std::string& s) {
    std::vector<std::string> result;
    std::istringstream iss(s);
    std::string token;
    while (iss >> token) {
        result.push_back(token);
    }
    return result;
}

static std::string join(const std::vector<std::string>& v, const std::string& delim) {
    std::string result;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) result += delim;
        result += v[i];
    }
    return result;
}

static std::string read_file(const fs::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Cannot open file: " + path.string());
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static void write_file(const fs::path& path, const std::string& content) {
    std::ofstream file(path);
    if (!file) {
        throw std::runtime_error("Cannot write file: " + path.string());
    }
    file << content;
}

// ============================================================================
// Simple XML Parser (for archs.xml and machines.xml only)
// ============================================================================

// Remove XML comments <!-- ... -->
static std::string strip_xml_comments(const std::string& xml) {
    std::string result;
    size_t pos = 0;
    while (pos < xml.size()) {
        size_t comment_start = xml.find("<!--", pos);
        if (comment_start == std::string::npos) {
            result += xml.substr(pos);
            break;
        }
        result += xml.substr(pos, comment_start - pos);
        size_t comment_end = xml.find("-->", comment_start);
        if (comment_end == std::string::npos) {
            break; // Unclosed comment, skip rest
        }
        pos = comment_end + 3;
    }
    return result;
}

struct XmlElement {
    std::string tag;
    std::map<std::string, std::string> attrs;
    std::string text;
    std::vector<XmlElement> children;
};

static std::string extract_attr(const std::string& tag_content, const std::string& attr_name) {
    std::regex attr_re(attr_name + R"RE(\s*=\s*"([^"]*)")RE");
    std::smatch match;
    if (std::regex_search(tag_content, match, attr_re)) {
        return match[1].str();
    }
    return "";
}

static std::vector<XmlElement> parse_xml_elements(const std::string& xml, const std::string& tag_name) {
    std::vector<XmlElement> elements;
    std::string open_tag = "<" + tag_name;
    std::string close_tag = "</" + tag_name + ">";

    size_t pos = 0;
    size_t max_iterations = xml.size() + 1;  // Safety limit
    size_t iterations = 0;
    while ((pos = xml.find(open_tag, pos)) != std::string::npos) {
        if (++iterations > max_iterations) {
            std::cerr << "Error: XML parsing exceeded maximum iterations for tag: " << tag_name << std::endl;
            break;
        }
        size_t tag_end = xml.find('>', pos);
        if (tag_end == std::string::npos) break;

        XmlElement elem;
        elem.tag = tag_name;

        // Extract attributes from opening tag
        std::string tag_content = xml.substr(pos, tag_end - pos + 1);

        // Parse attributes
        std::regex attr_re(R"RE((\w+)\s*=\s*"([^"]*)")RE");
        std::sregex_iterator attr_it(tag_content.begin(), tag_content.end(), attr_re);
        std::sregex_iterator attr_end;
        for (; attr_it != attr_end; ++attr_it) {
            elem.attrs[(*attr_it)[1].str()] = (*attr_it)[2].str();
        }

        // Check for self-closing tag
        if (xml[tag_end - 1] == '/') {
            elements.push_back(elem);
            pos = tag_end + 1;
            continue;
        }

        // Find closing tag
        size_t close_pos = xml.find(close_tag, tag_end);
        if (close_pos == std::string::npos) {
            pos = tag_end + 1;
            continue;
        }

        std::string inner = xml.substr(tag_end + 1, close_pos - tag_end - 1);
        elem.text = trim(inner);

        // Parse child elements
        for (const auto& child_tag : {"flag", "check", "param", "alignment", "environment", "include", "archs"}) {
            auto children = parse_xml_elements(inner, child_tag);
            elem.children.insert(elem.children.end(), children.begin(), children.end());
        }

        elements.push_back(elem);
        pos = close_pos + close_tag.length();
    }

    return elements;
}

// ============================================================================
// Architecture Parsing
// ============================================================================

static void parse_archs(const fs::path& archs_xml_path) {
    std::string xml = read_file(archs_xml_path);
    xml = strip_xml_comments(xml);
    auto arch_elements = parse_xml_elements(xml, "arch");

    for (const auto& elem : arch_elements) {
        Arch arch;
        arch.name = elem.attrs.count("name") ? elem.attrs.at("name") : "";
        if (arch.name.empty()) continue;

        // Parse child elements
        for (const auto& child : elem.children) {
            if (child.tag == "flag") {
                std::string compiler = child.attrs.count("compiler") ? child.attrs.at("compiler") : "";
                if (!compiler.empty() && !child.text.empty()) {
                    arch.flags[compiler].push_back(child.text);
                }
            } else if (child.tag == "check") {
                std::string check_name = child.attrs.count("name") ? child.attrs.at("name") : "";
                std::vector<std::string> params;
                for (const auto& param : child.children) {
                    if (param.tag == "param" && !param.text.empty()) {
                        params.push_back(param.text);
                    }
                }
                if (!check_name.empty()) {
                    arch.checks.push_back({check_name, params});
                }
            } else if (child.tag == "alignment") {
                arch.alignment = std::stoi(child.text);
            } else if (child.tag == "environment") {
                arch.environment = child.text;
            } else if (child.tag == "include") {
                arch.include = child.text;
            }
        }

        g_archs.push_back(arch);
    }

    // Build arch dictionary
    for (auto& arch : g_archs) {
        g_arch_dict[arch.name] = &arch;
    }
}

// ============================================================================
// Machine Parsing
// ============================================================================

static void register_machine(const std::string& name, std::vector<std::string> archs);

static void register_machine(const std::string& name, std::vector<std::string> archs) {
    // Handle special arch names with '|'
    for (size_t i = 0; i < archs.size(); ++i) {
        if (archs[i].find('|') != std::string::npos) {
            auto parts = split(archs[i], '|');
            for (const auto& part : parts) {
                std::vector<std::string> new_archs;
                new_archs.insert(new_archs.end(), archs.begin(), archs.begin() + i);
                if (!part.empty()) {
                    new_archs.push_back(part);
                    std::vector<std::string> full_archs = new_archs;
                    full_archs.insert(full_archs.end(), archs.begin() + i + 1, archs.end());
                    register_machine(name + "_" + part, full_archs);
                } else {
                    new_archs.insert(new_archs.end(), archs.begin() + i + 1, archs.end());
                    register_machine(name, new_archs);
                }
            }
            return;
        }
    }

    // No '|' found, register the machine
    Machine machine;
    machine.name = name;

    for (const auto& arch_name : archs) {
        if (arch_name.empty()) continue;
        auto it = g_arch_dict.find(arch_name);
        if (it != g_arch_dict.end()) {
            machine.arch_names.push_back(arch_name);
            machine.archs.push_back(it->second);
        } else {
            // Unknown arch - this machine is invalid
            return;
        }
    }

    if (machine.archs.empty()) {
        return;
    }

    int max_align = 1;
    for (const auto* arch : machine.archs) {
        max_align = std::max(max_align, arch->alignment);
    }
    machine.alignment = max_align;

    g_machines.push_back(machine);
}

static void parse_machines(const fs::path& machines_xml_path) {
    std::string xml = read_file(machines_xml_path);
    xml = strip_xml_comments(xml);
    auto machine_elements = parse_xml_elements(xml, "machine");

    for (const auto& elem : machine_elements) {
        std::string name = elem.attrs.count("name") ? elem.attrs.at("name") : "";
        if (name.empty()) continue;

        std::vector<std::string> archs;
        for (const auto& child : elem.children) {
            if (child.tag == "archs") {
                archs = split_whitespace(child.text);
                break;
            }
        }

        register_machine(name, archs);
    }

    // Build machine dictionary
    for (auto& machine : g_machines) {
        g_machine_dict[machine.name] = &machine;
    }
}

// ============================================================================
// Comment Removal
// ============================================================================

static std::string remove_comments(const std::string& code) {
    std::string result;
    bool in_line_comment = false;
    bool in_block_comment = false;
    bool in_string = false;
    char string_char = 0;

    for (size_t i = 0; i < code.size(); ++i) {
        if (in_line_comment) {
            if (code[i] == '\n') {
                in_line_comment = false;
                result += code[i];
            }
        } else if (in_block_comment) {
            if (i + 1 < code.size() && code[i] == '*' && code[i + 1] == '/') {
                in_block_comment = false;
                ++i;
            }
        } else if (in_string) {
            result += code[i];
            if (code[i] == '\\' && i + 1 < code.size()) {
                result += code[++i];
            } else if (code[i] == string_char) {
                in_string = false;
            }
        } else {
            if (code[i] == '"' || code[i] == '\'') {
                in_string = true;
                string_char = code[i];
                result += code[i];
            } else if (i + 1 < code.size() && code[i] == '/' && code[i + 1] == '/') {
                in_line_comment = true;
                ++i;
            } else if (i + 1 < code.size() && code[i] == '/' && code[i + 1] == '*') {
                in_block_comment = true;
                ++i;
            } else {
                result += code[i];
            }
        }
    }

    return result;
}

// ============================================================================
// Kernel Header Parsing
// ============================================================================

struct IfdefSection {
    std::string header;
    std::string body;
    std::vector<IfdefSection> subsections;
    bool is_text = false;
};

static std::vector<IfdefSection> split_ifdef_sections(const std::string& code, int recursion_depth = 0) {
    const int max_recursion_depth = 50;  // Safety limit for nested #ifdef
    std::vector<IfdefSection> sections;

    if (recursion_depth > max_recursion_depth) {
        std::cerr << "Warning: #ifdef recursion depth exceeded, stopping" << std::endl;
        return sections;
    }

    std::string current_section;
    std::string header = "text";
    int depth = 0;

    auto lines = split(code, '\n');
    for (const auto& line : lines) {
        std::regex ifdef_re(R"(^\s*#\s*(\w+)(.*)$)");
        std::smatch match;
        std::string line_type = "normal";

        if (std::regex_match(line, match, ifdef_re)) {
            std::string directive = match[1].str();
            if (directive == "if" || directive == "ifndef" || directive == "ifdef") {
                line_type = "if";
            } else if (directive == "else" || directive == "elif") {
                line_type = "else";
            } else if (directive == "endif") {
                line_type = "end";
            }
        }

        if (line_type == "if") depth++;
        if (line_type == "end") depth--;

        if (depth == 1 && line_type == "if") {
            if (!trim(current_section).empty()) {
                IfdefSection sec;
                sec.header = header;
                sec.body = current_section;
                sec.is_text = (header == "text");
                sections.push_back(sec);
            }
            current_section = "";
            header = line;
            continue;
        }

        if (depth == 1 && line_type == "else") {
            if (!trim(current_section).empty()) {
                IfdefSection sec;
                sec.header = header;
                sec.body = current_section;
                sec.is_text = (header == "text");
                sections.push_back(sec);
            }
            current_section = "";
            header = line;
            continue;
        }

        if (depth == 0 && line_type == "end") {
            if (!trim(current_section).empty()) {
                IfdefSection sec;
                sec.header = header;
                sec.body = current_section;
                sec.is_text = (header == "text");
                sections.push_back(sec);
            }
            current_section = "";
            header = "text";
            continue;
        }

        current_section += line + "\n";
    }

    if (!trim(current_section).empty()) {
        IfdefSection sec;
        sec.header = header;
        sec.body = current_section;
        sec.is_text = (header == "text");
        sections.push_back(sec);
    }

    // Recursively process non-text sections
    for (auto& sec : sections) {
        if (!sec.is_text && !sec.body.empty()) {
            sec.subsections = split_ifdef_sections(sec.body, recursion_depth + 1);
        }
    }

    return sections;
}

static std::string flatten_sections(const std::vector<IfdefSection>& sections, int recursion_depth = 0) {
    const int max_recursion_depth = 50;  // Safety limit
    std::string result;

    if (recursion_depth > max_recursion_depth) {
        std::cerr << "Warning: flatten_sections recursion depth exceeded" << std::endl;
        return result;
    }

    for (const auto& sec : sections) {
        if (sec.is_text) {
            result += sec.body;
        } else {
            result += flatten_sections(sec.subsections, recursion_depth + 1);
        }
    }
    return result;
}

static Impl parse_impl(const std::string& kern_name, const std::string& header,
                       const std::vector<IfdefSection>& body_sections) {
    Impl impl;

    // Extract LV_HAVE_* dependencies
    std::regex lv_have_re(R"(LV_HAVE_(\w+))");
    std::sregex_iterator it(header.begin(), header.end(), lv_have_re);
    std::sregex_iterator end;
    for (; it != end; ++it) {
        impl.deps.insert(to_lower((*it)[1].str()));
    }

    // Flatten body and extract function info
    std::string body = flatten_sections(body_sections);

    // Find the function signature
    // Pattern: kern_name_impl_name(args) {
    std::regex fcn_re(R"((\w*)" + kern_name + R"(_?\w*)\s*\([^)]*\))");
    std::smatch fcn_match;

    // Get part before opening brace
    size_t brace_pos = body.find('{');
    std::string pre_brace = (brace_pos != std::string::npos) ? body.substr(0, brace_pos) : body;

    // Find function name
    std::regex name_re(kern_name + R"(_(\w+)\s*\()");
    if (std::regex_search(pre_brace, fcn_match, name_re)) {
        impl.name = fcn_match[1].str();
    } else {
        // Fallback: use first dep as name
        if (!impl.deps.empty()) {
            impl.name = *impl.deps.begin();
        }
    }

    impl.is_aligned = impl.name.find("a_") == 0;

    // Parse arguments (we'll get these from the first impl later)
    std::regex args_re(kern_name + R"(\w*\s*\(([^)]*)\))");
    if (std::regex_search(pre_brace, fcn_match, args_re)) {
        std::string args_str = fcn_match[1].str();
        auto arg_parts = split(args_str, ',');
        for (const auto& arg : arg_parts) {
            std::string trimmed = trim(arg);
            if (trimmed.empty()) continue;

            // Find last word (argument name) - avoid regex with .* that can cause backtracking
            // Find the end of trailing whitespace
            size_t end_pos = trimmed.size();
            while (end_pos > 0 && std::isspace(static_cast<unsigned char>(trimmed[end_pos - 1]))) {
                --end_pos;
            }
            // Find start of last identifier (argument name) - identifiers include alphanumeric and underscore
            size_t name_end = end_pos;
            while (end_pos > 0) {
                char c = trimmed[end_pos - 1];
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                    --end_pos;
                } else {
                    break;
                }
            }
            if (end_pos < name_end) {
                std::string arg_name = trimmed.substr(end_pos, name_end - end_pos);
                std::string arg_type = trim(trimmed.substr(0, end_pos));
                if (!arg_type.empty()) {
                    impl.args.push_back({arg_type, arg_name});
                }
            }
        }
    }

    return impl;
}

static void parse_kernels(const fs::path& kernels_dir) {
    std::vector<fs::path> kernel_files;
    for (const auto& entry : fs::directory_iterator(kernels_dir)) {
        if (entry.path().extension() == ".h") {
            kernel_files.push_back(entry.path());
        }
    }
    std::sort(kernel_files.begin(), kernel_files.end());

    for (const auto& kernel_file : kernel_files) {
        Kernel kern;
        kern.name = kernel_file.stem().string();
        kern.pname = std::regex_replace(kern.name, std::regex("^volk_"), "p_");

        std::string code = read_file(kernel_file);
        code = remove_comments(code);
        auto sections = split_ifdef_sections(code);

        for (const auto& sec : sections) {
            if (to_lower(sec.header).find("ifndef") == std::string::npos) continue;

            for (const auto& sub : sec.subsections) {
                if (to_lower(sub.header).find("if") == std::string::npos) continue;
                if (sub.header.find("LV_HAVE_") == std::string::npos) continue;

                try {
                    Impl impl = parse_impl(kern.name, sub.header, sub.subsections);
                    if (!impl.name.empty()) {
                        kern.impls.push_back(impl);
                    }
                } catch (...) {
                    // Skip unparseable implementations
                }
            }
        }

        if (kern.impls.empty()) continue;

        // Check for generic implementation
        bool has_generic = false;
        for (const auto& impl : kern.impls) {
            if (impl.name == "generic") {
                has_generic = true;
                break;
            }
        }
        if (!has_generic) {
            std::cerr << "Warning: " << kern.name << " does not have a generic protokernel, skipping." << std::endl;
            continue;
        }

        // Check for dispatcher
        for (auto it = kern.impls.begin(); it != kern.impls.end(); ++it) {
            if (it->name == "dispatcher") {
                kern.has_dispatcher = true;
                kern.impls.erase(it);
                break;
            }
        }

        // Get args from first impl
        if (!kern.impls.empty() && !kern.impls[0].args.empty()) {
            kern.args = kern.impls[0].args;

            std::vector<std::string> types, full, names;
            for (const auto& [type, name] : kern.args) {
                types.push_back(type);
                full.push_back(type + " " + name);
                names.push_back(name);
            }
            kern.arglist_types = join(types, ", ");
            kern.arglist_full = join(full, ", ");
            kern.arglist_names = join(names, ", ");
        }

        g_kernels.push_back(kern);
    }
}

// ============================================================================
// Template Engine
// ============================================================================

class TemplateEngine {
public:
    void set_var(const std::string& name, const std::string& value) {
        vars_[name] = value;
    }

    std::string render(const std::string& tmpl, const std::vector<std::string>& extra_args = {}) {
        std::string result;
        result += "\n/* this file was generated by volk template utils, do not edit! */\n\n";

        std::istringstream stream(tmpl);
        std::string line;

        struct LoopState {
            std::string var_name;
            std::string index_var_name;  // For enumerate loops
            std::string collection;
            size_t index = 0;
            std::string body;
            bool collecting = false;
            bool is_enumerate = false;
            int depth = 0;
        };

        struct IfState {
            bool condition_met = false;
            bool in_else = false;
            int depth = 0;
        };

        std::vector<LoopState> loop_stack;
        std::vector<IfState> if_stack;
        std::string pending_lines;

        // Multi-line code block tracking
        bool in_multiline_block = false;
        std::string multiline_block_content;

        auto process_line = [&](const std::string& input_line) -> std::string {
            std::string out;

            // Handle multi-line <% ... %> blocks
            if (in_multiline_block) {
                size_t end_pos = input_line.find("%>");
                if (end_pos != std::string::npos) {
                    // End of multi-line block
                    multiline_block_content += input_line.substr(0, end_pos);
                    std::string replacement = execute_code_block(multiline_block_content, extra_args);
                    in_multiline_block = false;
                    multiline_block_content.clear();
                    // Rest of line after %> is usually empty or whitespace, ignore it
                    return replacement;
                } else {
                    // Still collecting
                    multiline_block_content += input_line + "\n";
                    return "";
                }
            }

            // Check for start of multi-line <% block (no %> on same line)
            size_t block_start = input_line.find("<%");
            if (block_start != std::string::npos) {
                size_t block_end = input_line.find("%>", block_start + 2);
                if (block_end == std::string::npos) {
                    // Multi-line block starting
                    in_multiline_block = true;
                    multiline_block_content = input_line.substr(block_start + 2) + "\n";
                    // Return anything before the <%
                    return input_line.substr(0, block_start);
                }
            }

            // Handle for loops (collection can be word.word or just word)
            // Also handle enumerate: %for i, arch in enumerate(archs):
            // Also handle tuple unpacking: %for arg_type, arg_name in kern.args:
            std::regex for_re(R"(^\s*%\s*for\s+(\w+)\s+in\s+([\w.]+)\s*:)");
            std::regex for_enum_re(R"(^\s*%\s*for\s+(\w+)\s*,\s*(\w+)\s+in\s+enumerate\((\w+)\)\s*:)");
            std::regex for_tuple_re(R"(^\s*%\s*for\s+(\w+)\s*,\s*(\w+)\s+in\s+([\w.]+)\s*:)");
            std::smatch for_match;

            // Only start a new loop if we're NOT already collecting for an outer loop
            // If we're collecting, the loop detection happens in the collection code below
            if (loop_stack.empty() || !loop_stack.back().collecting) {
                // Check for enumerate pattern first
                if (std::regex_match(input_line, for_match, for_enum_re)) {
                    LoopState state;
                    state.index_var_name = for_match[1].str();
                    state.var_name = for_match[2].str();
                    state.collection = for_match[3].str();
                    state.collecting = true;
                    state.is_enumerate = true;
                    state.depth = 1;
                    loop_stack.push_back(state);
                    return "";
                }

                // Check for tuple unpacking pattern (like: for arg_type, arg_name in kern.args:)
                if (std::regex_match(input_line, for_match, for_tuple_re)) {
                    LoopState state;
                    state.index_var_name = for_match[1].str();  // first tuple element
                    state.var_name = for_match[2].str();        // second tuple element
                    state.collection = for_match[3].str();
                    state.collecting = true;
                    state.is_enumerate = false;  // Not enumerate, but tuple unpacking
                    state.depth = 1;
                    loop_stack.push_back(state);
                    return "";
                }

                if (std::regex_match(input_line, for_match, for_re)) {
                    LoopState state;
                    state.var_name = for_match[1].str();
                    state.collection = for_match[2].str();
                    state.collecting = true;
                    state.depth = 1;
                    loop_stack.push_back(state);
                    return "";
                }
            }

            // Handle endfor
            std::regex endfor_re(R"(^\s*%\s*endfor)");
            if (std::regex_match(input_line, endfor_re) && !loop_stack.empty()) {
                auto& state = loop_stack.back();
                state.depth--;
                assert(state.depth >= 0 && "Mismatched endfor");
                if (state.depth == 0) {
                    // Execute the loop
                    std::string loop_output;

                    if (state.collection == "kernels") {
                        for (size_t i = 0; i < g_kernels.size(); ++i) {
                            current_kernel_ = &g_kernels[i];
                            if (state.is_enumerate) current_enum_index_ = i;
                            loop_output += render_block(state.body, extra_args);
                        }
                        current_kernel_ = nullptr;
                        current_enum_index_ = -1;
                    } else if (state.collection == "archs") {
                        for (size_t i = 0; i < g_archs.size(); ++i) {
                            current_arch_ = &g_archs[i];
                            if (state.is_enumerate) current_enum_index_ = i;
                            loop_output += render_block(state.body, extra_args);
                        }
                        current_arch_ = nullptr;
                        current_enum_index_ = -1;
                    } else if (state.collection == "machines") {
                        for (size_t i = 0; i < g_machines.size(); ++i) {
                            current_machine_ = &g_machines[i];
                            loop_output += render_block(state.body, extra_args);
                        }
                        current_machine_ = nullptr;
                    }
                    // Handle this_machine.archs loop
                    else if (state.collection == "this_machine.archs" && current_machine_) {
                        for (const auto* arch : current_machine_->archs) {
                            current_arch_ = const_cast<Arch*>(arch);
                            loop_output += render_block(state.body, extra_args);
                        }
                        current_arch_ = nullptr;
                    }
                    // Handle kern.args loop (with tuple unpacking: arg_type, arg_name)
                    else if (state.collection == "kern.args" && current_kernel_) {
                        for (size_t i = 0; i < current_kernel_->args.size(); ++i) {
                            current_arg_index_ = i;
                            loop_output += render_block(state.body, extra_args);
                        }
                        current_arg_index_ = -1;
                    }
                    // Handle arch.checks loop
                    else if (state.collection == "arch.checks" && current_arch_) {
                        for (size_t i = 0; i < current_arch_->checks.size(); ++i) {
                            current_check_index_ = i;
                            loop_output += render_block(state.body, extra_args);
                        }
                        current_check_index_ = -1;
                    }

                    loop_stack.pop_back();
                    return loop_output;
                }
            }

            // If in a loop and collecting, add lines to body (but don't handle endfor here -
            // that's handled by the endfor code above which already decremented depth)
            if (!loop_stack.empty() && loop_stack.back().collecting) {
                auto& state = loop_stack.back();
                // Track nested for loops - check ALL for loop patterns
                if (std::regex_match(input_line, for_re) ||
                    std::regex_match(input_line, for_enum_re) ||
                    std::regex_match(input_line, for_tuple_re)) {
                    state.depth++;
                }
                // Note: endfor is handled by the endfor code above, which decrements depth
                // If we're here, either this isn't an endfor, or depth > 0 after decrement
                if (!std::regex_match(input_line, endfor_re)) {
                    state.body += input_line + "\n";
                } else if (state.depth > 0) {
                    // Nested endfor - include it in the body
                    state.body += input_line + "\n";
                }
                // If depth == 0, the endfor code above already handled execution
                return "";
            }

            // Handle if/elif/else/endif
            std::regex if_re(R"(^\s*%\s*if\s+(.+?)\s*:)");
            std::regex elif_re(R"(^\s*%\s*elif\s+(.+?)\s*:)");
            std::regex else_re(R"(^\s*%\s*else\s*:)");
            std::regex endif_re(R"(^\s*%\s*endif)");
            std::smatch if_match;

            if (std::regex_match(input_line, if_match, if_re)) {
                IfState state;
                state.condition_met = evaluate_condition(if_match[1].str(), extra_args);
                state.depth = 1;
                if_stack.push_back(state);
                return "";
            }

            if (std::regex_match(input_line, if_match, elif_re) && !if_stack.empty()) {
                auto& state = if_stack.back();
                if (!state.condition_met) {
                    state.condition_met = evaluate_condition(if_match[1].str(), extra_args);
                    state.in_else = false;
                } else {
                    state.in_else = true; // Skip remaining branches
                }
                return "";
            }

            if (std::regex_match(input_line, else_re) && !if_stack.empty()) {
                auto& state = if_stack.back();
                if (!state.condition_met) {
                    // No previous branch matched, include else content
                    state.condition_met = true;
                    state.in_else = false;
                } else {
                    // A previous branch matched, skip else content
                    state.in_else = true;
                }
                return "";
            }

            if (std::regex_match(input_line, endif_re) && !if_stack.empty()) {
                if_stack.pop_back();
                return "";
            }

            // Check if we should skip this line due to if condition
            if (!if_stack.empty()) {
                const auto& state = if_stack.back();
                // Skip if: no condition matched yet, OR we're in a branch after a matched condition
                if (!state.condition_met || state.in_else) {
                    return "";
                }
            }

            // Handle inline Python code blocks <% ... %>
            // Use a non-greedy match that allows % inside as long as it's not followed by >
            std::regex code_block_re(R"(<%(.*?)%>)");
            std::string processed = input_line;
            std::smatch code_match;
            size_t code_iterations = 0;
            const size_t max_code_iterations = processed.size() + 100;
            while (std::regex_search(processed, code_match, code_block_re)) {
                if (++code_iterations > max_code_iterations) {
                    std::cerr << "Error: Code block substitution exceeded max iterations" << std::endl;
                    break;
                }
                std::string code = code_match[1].str();
                std::string replacement = execute_code_block(code, extra_args);
                processed = code_match.prefix().str() + replacement + code_match.suffix().str();
            }

            // Handle variable substitutions ${...}
            std::regex var_re(R"(\$\{([^}]+)\})");
            std::smatch var_match;
            size_t var_iterations = 0;
            const size_t max_var_iterations = processed.size() + 100;
            while (std::regex_search(processed, var_match, var_re)) {
                if (++var_iterations > max_var_iterations) {
                    std::cerr << "Error: Variable substitution exceeded max iterations" << std::endl;
                    break;
                }
                std::string expr = var_match[1].str();
                std::string value = evaluate_expression(expr, extra_args);
                processed = var_match.prefix().str() + value + var_match.suffix().str();
            }

            // Skip lines that start with ## (template comments)
            if (processed.find("##") == 0) {
                return "";
            }

            return processed + "\n";
        };

        while (std::getline(stream, line)) {
            result += process_line(line);
        }

        return result;
    }

private:
    std::string render_block(const std::string& block, const std::vector<std::string>& extra_args) {
        if (render_depth_ >= max_render_depth_) {
            std::cerr << "Error: Template render depth exceeded maximum (" << max_render_depth_ << ")" << std::endl;
            return "";
        }

        TemplateEngine sub_engine;
        sub_engine.vars_ = vars_;
        sub_engine.current_kernel_ = current_kernel_;
        sub_engine.current_arch_ = current_arch_;
        sub_engine.current_machine_ = current_machine_;
        sub_engine.current_impl_ = current_impl_;
        sub_engine.current_arg_index_ = current_arg_index_;
        sub_engine.current_check_index_ = current_check_index_;
        sub_engine.current_enum_index_ = current_enum_index_;
        sub_engine.len_archs_ = len_archs_;
        sub_engine.num_open_parens_ = num_open_parens_;
        sub_engine.end_open_parens_ = end_open_parens_;
        sub_engine.render_depth_ = render_depth_ + 1;

        // Remove the header we added in render()
        std::string result = sub_engine.render(block, extra_args);

        // Copy mutable state back to parent
        num_open_parens_ = sub_engine.num_open_parens_;
        end_open_parens_ = sub_engine.end_open_parens_;
        current_impls_ = sub_engine.current_impls_;

        // Remove the auto-added header
        std::string header = "\n/* this file was generated by volk template utils, do not edit! */\n\n";
        if (result.find(header) == 0) {
            result = result.substr(header.length());
        }
        return result;
    }

    bool evaluate_condition(const std::string& cond, const std::vector<std::string>& extra_args) {
        std::string c = trim(cond);
        std::smatch in_match;

        // Handle "or" - split and evaluate each part
        size_t or_pos = c.find(" or ");
        if (or_pos != std::string::npos) {
            std::string left = c.substr(0, or_pos);
            std::string right = c.substr(or_pos + 4);
            return evaluate_condition(left, extra_args) || evaluate_condition(right, extra_args);
        }

        // Handle "and"
        size_t and_pos = c.find(" and ");
        if (and_pos != std::string::npos) {
            std::string left = c.substr(0, and_pos);
            std::string right = c.substr(and_pos + 5);
            return evaluate_condition(left, extra_args) && evaluate_condition(right, extra_args);
        }

        // Handle slice comparison: X.Y[:N] == "Z"
        std::regex slice_eq_re(R"RE((\w+(?:\.\w+)*)\[:(\d+)\]\s*==\s*"([^"]*)")RE");
        if (std::regex_match(c, in_match, slice_eq_re)) {
            std::string var = in_match[1].str();
            int len = std::stoi(in_match[2].str());
            std::string expected = in_match[3].str();
            std::string value = evaluate_expression(var, extra_args);
            return value.substr(0, len) == expected;
        }

        // Handle '*' in arg_type (string literal 'X' in Y) - check BEFORE general in_re
        std::regex star_re(R"('([^']+)'\s+in\s+(\w+))");
        if (std::regex_match(c, in_match, star_re)) {
            std::string needle = in_match[1].str();
            std::string varname = in_match[2].str();
            std::string haystack = evaluate_expression(varname, extra_args);
            return haystack.find(needle) != std::string::npos;
        }

        // Handle string comparisons with "in" (double quotes)
        std::regex str_in_re(R"RE("([^"]+)"\s+in\s+(\S+))RE");
        if (std::regex_match(c, in_match, str_in_re)) {
            std::string needle = in_match[1].str();
            std::string var = in_match[2].str();
            std::string haystack = evaluate_expression(var, extra_args);
            return haystack.find(needle) != std::string::npos;
        }

        // Handle "X in Y" (variable in collection)
        std::regex in_re(R"((\S+)\s+in\s+(\S+))");
        if (std::regex_match(c, in_match, in_re)) {
            std::string item = evaluate_expression(in_match[1].str(), extra_args);
            std::string collection = in_match[2].str();

            if (collection == "deprecated_kernels") {
                // Hardcoded list of deprecated kernels
                static std::set<std::string> deprecated = {
                    "volk_16i_x5_add_quad_16i_x4", "volk_16i_branch_4_state_8",
                    "volk_16i_max_star_16i", "volk_16i_max_star_horizontal_16i",
                    "volk_16i_permute_and_scalar_add", "volk_16i_x4_quad_max_star_16i",
                    "volk_32fc_s32fc_multiply_32fc", "volk_32fc_s32fc_x2_rotator_32fc",
                    "volk_32fc_x2_s32fc_multiply_conjugate_add_32fc"
                };
                return deprecated.count(item) > 0;
            }
            return false;
        }

        // Handle "X.Y"
        if (c.find('.') != std::string::npos) {
            std::string value = evaluate_expression(c, extra_args);
            return !value.empty() && value != "0" && value != "false";
        }

        return false;
    }

    std::string execute_code_block(const std::string& code, const std::vector<std::string>& extra_args) {
        std::string c = trim(code);

        // Handle this_machine = machine_dict[args[0]]
        std::regex machine_re(R"(this_machine\s*=\s*machine_dict\[args\[0\]\])");
        if (std::regex_search(c, machine_re)) {
            if (!extra_args.empty()) {
                auto it = g_machine_dict.find(extra_args[0]);
                if (it != g_machine_dict.end()) {
                    current_machine_ = it->second;
                }
            }
            return "";
        }

        // Handle arch_names = this_machine.arch_names
        std::regex arch_names_re(R"(arch_names\s*=\s*this_machine\.arch_names)");
        if (std::regex_search(c, arch_names_re)) {
            // This is handled implicitly through current_machine_
            return "";
        }

        // Handle num_open_parens = 0
        std::regex num_parens_re(R"(num_open_parens\s*=\s*0)");
        if (std::regex_search(c, num_parens_re)) {
            num_open_parens_ = 0;
            return "";
        }

        // Handle num_open_parens += 1
        std::regex inc_parens_re(R"(num_open_parens\s*\+=\s*1)");
        if (std::regex_search(c, inc_parens_re)) {
            num_open_parens_++;
            return "";
        }

        // Handle end_open_parens = ')'*num_open_parens
        std::regex end_parens_re(R"(end_open_parens\s*=\s*'\)'\*num_open_parens)");
        if (std::regex_search(c, end_parens_re)) {
            end_open_parens_ = std::string(num_open_parens_, ')');
            return "";
        }

        // Handle impls = kern.get_impls(arch_names)
        std::regex impls_re(R"(impls\s*=\s*kern\.get_impls\(arch_names\))");
        if (std::regex_search(c, impls_re) && current_kernel_ && current_machine_) {
            std::set<std::string> arch_set(current_machine_->arch_names.begin(),
                                           current_machine_->arch_names.end());
            current_impls_ = current_kernel_->get_impls(arch_set);
            return "";
        }

        // Handle complex expressions that output something
        // make_arch_have_list = ...
        std::regex make_have_re(R"(make_arch_have_list\s*=)");
        if (std::regex_search(c, make_have_re) && current_machine_) {
            std::vector<std::string> parts;
            for (const auto* arch : current_machine_->archs) {
                parts.push_back("(1 << LV_" + to_upper(arch->name) + ")");
            }
            return join(parts, " | ");
        }

        // this_machine_name = ...
        std::regex machine_name_re(R"(this_machine_name\s*=)");
        if (std::regex_search(c, machine_name_re) && current_machine_) {
            return "\"" + current_machine_->name + "\"";
        }

        // kern_name = ...
        std::regex kern_name_re(R"(kern_name\s*=)");
        if (std::regex_search(c, kern_name_re) && current_kernel_) {
            return "\"" + current_kernel_->name + "\"";
        }

        // make_impl_name_list = ...
        std::regex impl_names_re(R"(make_impl_name_list\s*=)");
        if (std::regex_search(c, impl_names_re)) {
            std::vector<std::string> names;
            for (const auto* impl : current_impls_) {
                names.push_back("\"" + impl->name + "\"");
            }
            return "{" + join(names, ", ") + "}";
        }

        // make_impl_deps_list = ...
        std::regex impl_deps_re(R"(make_impl_deps_list\s*=)");
        if (std::regex_search(c, impl_deps_re)) {
            std::vector<std::string> deps_list;
            for (const auto* impl : current_impls_) {
                std::vector<std::string> dep_parts;
                for (const auto& dep : impl->deps) {
                    dep_parts.push_back("(1 << LV_" + to_upper(dep) + ")");
                }
                if (dep_parts.empty()) {
                    deps_list.push_back("0");
                } else {
                    deps_list.push_back(join(dep_parts, " | "));
                }
            }
            return "{" + join(deps_list, ", ") + "}";
        }

        // make_impl_align_list = ...
        std::regex impl_align_re(R"(make_impl_align_list\s*=)");
        if (std::regex_search(c, impl_align_re)) {
            std::vector<std::string> aligns;
            for (const auto* impl : current_impls_) {
                aligns.push_back(impl->is_aligned ? "true" : "false");
            }
            return "{" + join(aligns, ", ") + "}";
        }

        // make_impl_fcn_list = ...
        std::regex impl_fcn_re(R"(make_impl_fcn_list\s*=)");
        if (std::regex_search(c, impl_fcn_re) && current_kernel_) {
            std::vector<std::string> fcns;
            for (const auto* impl : current_impls_) {
                fcns.push_back(current_kernel_->name + "_" + impl->name);
            }
            return "{" + join(fcns, ", ") + "}";
        }

        // len_impls = ...
        std::regex len_impls_re(R"(len_impls\s*=)");
        if (std::regex_search(c, len_impls_re)) {
            return std::to_string(current_impls_.size());
        }

        // len_archs=len(archs) - computes and stores the number of archs (returns empty, variable is used via ${len_archs})
        std::regex len_archs_re(R"(len_archs\s*=\s*len\(archs\))");
        if (std::regex_search(c, len_archs_re)) {
            len_archs_ = g_archs.size();
            return "";
        }

        // Handle deprecated_kernels definition
        if (c.find("deprecated_kernels") != std::string::npos) {
            return "";
        }

        // Handle platform import/check (Windows detection)
        if (c.find("from platform import system") != std::string::npos ||
            c.find("system()") != std::string::npos) {
            return "";
        }

        return "";
    }

    std::string evaluate_expression(const std::string& expr, const std::vector<std::string>& extra_args) {
        std::string e = trim(expr);

        // Check variables first
        auto var_it = vars_.find(e);
        if (var_it != vars_.end()) {
            return var_it->second;
        }

        // end_open_parens
        if (e == "end_open_parens") {
            return end_open_parens_;
        }

        // kern.name, kern.pname, etc.
        if (current_kernel_) {
            if (e == "kern.name") return current_kernel_->name;
            if (e == "kern.pname") return current_kernel_->pname;
            if (e == "kern.arglist_full") return current_kernel_->arglist_full;
            if (e == "kern.arglist_names") return current_kernel_->arglist_names;
            if (e == "kern.arglist_types") return current_kernel_->arglist_types;
            if (e == "kern.has_dispatcher") return current_kernel_->has_dispatcher ? "1" : "";
        }

        // arch.name
        if (current_arch_) {
            if (e == "arch.name") return current_arch_->name;
            if (e == "arch.name.upper()") return to_upper(current_arch_->name);
        }

        // this_machine.alignment, this_machine.name, machine.name (loop variable)
        if (current_machine_) {
            if (e == "this_machine.alignment") return std::to_string(current_machine_->alignment);
            if (e == "this_machine.name") return current_machine_->name;
            if (e == "machine.name") return current_machine_->name;
            if (e == "machine.name.upper()") return to_upper(current_machine_->name);
        }

        // arg_type, arg_name
        if (current_kernel_ && current_arg_index_ >= 0 &&
            current_arg_index_ < (int)current_kernel_->args.size()) {
            if (e == "arg_type") return current_kernel_->args[current_arg_index_].first;
            if (e == "arg_name") return current_kernel_->args[current_arg_index_].second;
        }

        // check, params
        if (current_arch_ && current_check_index_ >= 0 &&
            current_check_index_ < (int)current_arch_->checks.size()) {
            if (e == "check") return current_arch_->checks[current_check_index_].first;
        }

        // enumerate index variable (typically 'i')
        if (e == "i" && current_enum_index_ >= 0) {
            return std::to_string(current_enum_index_);
        }

        // len_archs - stored from inline code block
        if (e == "len_archs" && len_archs_ > 0) {
            return std::to_string(len_archs_);
        }

        return "";
    }

    std::map<std::string, std::string> vars_;
    Kernel* current_kernel_ = nullptr;
    Arch* current_arch_ = nullptr;
    Machine* current_machine_ = nullptr;
    const Impl* current_impl_ = nullptr;
    int current_arg_index_ = -1;
    int current_check_index_ = -1;
    int current_enum_index_ = -1;
    int num_open_parens_ = 0;
    std::string end_open_parens_;
    std::vector<const Impl*> current_impls_;
    size_t len_archs_ = 0;
    int render_depth_ = 0;
    static const int max_render_depth_ = 20;  // Safety limit for nested template loops
};

// ============================================================================
// Command Line Interface
// ============================================================================

static void print_usage() {
    std::cerr << "Usage: volk_gen <mode> [options]\n";
    std::cerr << "\nModes:\n";
    std::cerr << "  arch_flags --compiler <name>          List arch flags for compiler\n";
    std::cerr << "  machines --archs \"arch1;arch2;...\"    List available machines\n";
    std::cerr << "  machine_flags --machine <name> --compiler <name>  Get machine flags\n";
    std::cerr << "  render --input <file> --output <file> [extra args]  Render template\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string mode = argv[1];

    // Find the source directory
    fs::path exe_path = fs::canonical(argv[0]);
    fs::path src_dir = exe_path.parent_path().parent_path().parent_path();

    // Also check if we're in a build directory
    if (!fs::exists(src_dir / "gen" / "archs.xml")) {
        // Try to find via CMAKE_SOURCE_DIR environment or relative paths
        src_dir = fs::current_path();
        int search_depth = 0;
        const int max_search_depth = 20;  // Safety limit for filesystem traversal
        while (!fs::exists(src_dir / "gen" / "archs.xml") && src_dir.has_parent_path() && search_depth < max_search_depth) {
            fs::path parent = src_dir.parent_path();
            if (parent == src_dir) break;  // Reached filesystem root
            src_dir = parent;
            ++search_depth;
        }
    }

    // Allow override via environment variable
    if (const char* env_src = std::getenv("VOLK_SOURCE_DIR")) {
        src_dir = env_src;
    }

    fs::path archs_xml = src_dir / "gen" / "archs.xml";
    fs::path machines_xml = src_dir / "gen" / "machines.xml";
    fs::path kernels_dir = src_dir / "kernels" / "volk";

    try {
        parse_archs(archs_xml);
        parse_machines(machines_xml);

        if (mode == "arch_flags") {
            // --compiler <name>
            std::string compiler;
            for (int i = 2; i < argc; ++i) {
                if (std::string(argv[i]) == "--compiler" && i + 1 < argc) {
                    compiler = to_lower(argv[++i]);
                }
            }

            std::vector<std::string> output;
            for (const auto& arch : g_archs) {
                if (!arch.is_supported(compiler)) continue;
                std::vector<std::string> fields = {arch.name};
                auto flags = arch.get_flags(compiler);
                fields.insert(fields.end(), flags.begin(), flags.end());
                output.push_back(join(fields, ","));
            }
            std::cout << join(output, ";") << std::endl;
        }
        else if (mode == "machines") {
            // --archs "arch1;arch2;..."
            std::set<std::string> arch_names;
            for (int i = 2; i < argc; ++i) {
                if (std::string(argv[i]) == "--archs" && i + 1 < argc) {
                    auto parts = split(argv[++i], ';');
                    arch_names.insert(parts.begin(), parts.end());
                }
            }

            std::vector<std::string> output;
            for (const auto& machine : g_machines) {
                std::set<std::string> machine_arch_set(machine.arch_names.begin(),
                                                        machine.arch_names.end());
                // Check if all machine archs are in available archs
                bool all_present = true;
                for (const auto& ma : machine_arch_set) {
                    if (arch_names.find(ma) == arch_names.end()) {
                        all_present = false;
                        break;
                    }
                }
                if (all_present) {
                    output.push_back(machine.name);
                }
            }
            std::cout << join(output, ";") << std::endl;
        }
        else if (mode == "machine_flags") {
            // --machine <name> --compiler <name>
            std::string machine_name, compiler;
            for (int i = 2; i < argc; ++i) {
                if (std::string(argv[i]) == "--machine" && i + 1 < argc) {
                    machine_name = argv[++i];
                } else if (std::string(argv[i]) == "--compiler" && i + 1 < argc) {
                    compiler = to_lower(argv[++i]);
                }
            }

            auto it = g_machine_dict.find(machine_name);
            if (it == g_machine_dict.end()) {
                std::cerr << "Unknown machine: " << machine_name << std::endl;
                return 1;
            }

            std::vector<std::string> output;
            for (const auto* arch : it->second->archs) {
                auto flags = arch->get_flags(compiler);
                output.insert(output.end(), flags.begin(), flags.end());
            }
            std::cout << join(output, " ") << std::endl;
        }
        else if (mode == "render") {
            // --input <file> --output <file> [extra args]
            std::string input_file, output_file;
            std::vector<std::string> extra_args;

            for (int i = 2; i < argc; ++i) {
                if (std::string(argv[i]) == "--input" && i + 1 < argc) {
                    input_file = argv[++i];
                } else if (std::string(argv[i]) == "--output" && i + 1 < argc) {
                    output_file = argv[++i];
                } else if (argv[i][0] != '-') {
                    extra_args.push_back(argv[i]);
                }
            }

            if (input_file.empty()) {
                std::cerr << "Missing --input\n";
                return 1;
            }

            // Parse kernels for template rendering
            parse_kernels(kernels_dir);

            std::string tmpl = read_file(input_file);
            TemplateEngine engine;
            std::string result = engine.render(tmpl, extra_args);

            if (output_file.empty()) {
                std::cout << result;
            } else {
                write_file(output_file, result);
            }
        }
        else {
            print_usage();
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
