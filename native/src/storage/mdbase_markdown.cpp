// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/mdbase_markdown.h"

#include <QFile>
#include <QUrl>

#include <filesystem>
#include <QCryptographicHash>
#include <QByteArray>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <optional>

namespace todobench::mdbase_transfer {
namespace {

QString path_string(const std::filesystem::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    const auto bytes = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()), bytes.size());
#endif
}

bool is_remote_url(const std::string& target){
    return target.rfind("http://",0)==0 || target.rfind("https://",0)==0 || target.rfind("data:",0)==0 || target.rfind("ftp://",0)==0;
}

bool is_within_collection(const std::filesystem::path& collectionRoot, const std::filesystem::path& candidate){
    try{
        auto canonRoot = std::filesystem::weakly_canonical(collectionRoot);
        auto canonCand = std::filesystem::weakly_canonical(candidate);
        std::string r = canonRoot.generic_string();
        std::string c = canonCand.generic_string();
        if(c==r) return true;
        if(c.size()>r.size() && c[r.size()]=='/' && c.rfind(r,0)==0) return true;
        return false;
    } catch(...) { return false; }
}

std::optional<QByteArray> file_hash(const std::filesystem::path& path) {
    QFile file(path_string(path));
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1 << 20);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) return std::nullopt;
        hash.addData(chunk);
    }
    return hash.result();
}

bool copy_file_checked(const std::filesystem::path& source,
                       const std::filesystem::path& destination,
                       TransferCancellation* cancellation,
                       std::string& error) {
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!input || !output) {
        error = "unable to open attachment for copy";
        return false;
    }
    char buffer[1 << 20];
    while (input) {
        if (cancellation && cancellation->is_cancelled()) {
            error = "cancelled";
            return false;
        }
        input.read(buffer, sizeof(buffer));
        const auto count = input.gcount();
        if (count > 0) output.write(buffer, count);
        if (!output) {
            error = "attachment write failed";
            return false;
        }
    }
    if (!input.eof()) {
        error = "attachment read failed";
        return false;
    }
    output.flush();
    if (!output) {
        error = "attachment flush failed";
        return false;
    }
    const auto source_hash = file_hash(source);
    const auto destination_hash = file_hash(destination);
    if (!source_hash || !destination_hash || *source_hash != *destination_hash) {
        error = "attachment verification failed";
        return false;
    }
    return true;
}

std::string percent_decode(const std::string& s){
    std::string out;
    out.reserve(s.size());
    for(size_t i=0;i<s.size();++i){
        if(s[i]=='%' && i+2<s.size()){
            char hex[3]={s[i+1],s[i+2],0};
            char* e=nullptr;
            long v=strtol(hex,&e,16);
            if(e && *e==0){ out.push_back(char(v)); i+=2; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
}

bool is_in_fence(const std::string& body, size_t pos) {
    size_t fenceOpen = 0, idx = 0;
    while ((idx = body.find("```", idx)) != std::string::npos) {
        if (fenceOpen == 0) fenceOpen = idx;
        else {
            if (pos > fenceOpen && pos < idx + 3) return true;
            fenceOpen = 0;
        }
        idx += 3;
    }
    return fenceOpen != 0 && pos > fenceOpen;
}

bool is_in_code(const std::string& body, size_t pos) {
    if (is_in_fence(body, pos)) return true;
    const auto previousNewline = body.rfind('\n', pos);
    const size_t lineStart = previousNewline == std::string::npos ? 0 : previousNewline + 1;
    const size_t lineEnd = body.find('\n', pos);
    const std::string line = body.substr(lineStart,
        lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);
    if (line.starts_with("    ") || line.starts_with("\t")) return true;
    const auto before = std::count(body.begin() + lineStart, body.begin() + pos, '`');
    const auto after = lineEnd == std::string::npos ? 0
        : std::count(body.begin() + pos, body.begin() + lineEnd, '`');
    return before % 2 == 1 && after % 2 == 1;
}

std::string trim(const std::string& text) {
    size_t start = 0, end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(start, end - start);
}

using DestinationMap = std::unordered_map<std::string, std::string>;
struct RewriteContext {
    const MarkdownRewriteRequest& request;
    const DestinationMap& destinations;
    MarkdownRewriteResult& result;
    std::string source_path;

    void diagnostic(TransferSeverity severity, const std::string& code,
                    const std::string& message, const std::string& target) {
        result.diagnostics.push_back({severity, code, message, source_path, target, {}});
    }

    std::string relative_destination(const std::string& destination) const {
        const auto current = destinations.find(source_path);
        if (current == destinations.end()) return destination;
        return std::filesystem::relative(request.dest_workspace_root / destination,
            (request.dest_workspace_root / current->second).parent_path()).generic_string();
    }
};

std::optional<std::filesystem::path> asset_destination(
        const std::filesystem::path& source, const std::filesystem::path& directory,
        RewriteContext& context, const std::string& target) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        context.diagnostic(TransferSeverity::Error, "asset_copy_failed", error.message(), target);
        return std::nullopt;
    }
    const auto hash = file_hash(source);
    if (!hash) {
        context.diagnostic(TransferSeverity::Error, "asset_copy_failed", "unable to hash attachment", target);
        return std::nullopt;
    }
    auto destination = directory / source.filename();
    if (!std::filesystem::exists(destination)) return destination;
    const auto existing = file_hash(destination);
    if (!existing) {
        context.diagnostic(TransferSeverity::Error, "asset_copy_failed",
            "unable to verify existing attachment", target);
        return std::nullopt;
    }
    if (*existing != *hash) {
        const auto suffix = hash->toHex().left(12).toStdString();
        destination = directory / (source.stem().string() + "-" + suffix + source.extension().string());
    }
    return destination;
}

std::string copy_asset(const std::filesystem::path& source, const std::string& original,
                       const std::string& target, const std::string& anchor,
                       RewriteContext& context) {
    const auto current = context.destinations.find(context.source_path);
    if (current == context.destinations.end()) return original;
    const auto current_directory =
        (context.request.dest_workspace_root / current->second).parent_path();
    const auto destination = asset_destination(source, current_directory / "assets", context, target);
    if (!destination) return original;
    if (!std::filesystem::exists(*destination)) {
        std::string error;
        if (!copy_file_checked(source, *destination, context.request.cancellation, error)) {
            std::error_code ignored;
            std::filesystem::remove(*destination, ignored);
            const bool cancelled = error == "cancelled";
            context.diagnostic(cancelled ? TransferSeverity::Warning : TransferSeverity::Error,
                cancelled ? "cancelled" : "asset_copy_failed", error, target);
            return original;
        }
        ++context.result.copied_asset_count;
    }
    const auto relative = std::filesystem::relative(*destination, current_directory).generic_string();
    return QUrl::toPercentEncoding(QString::fromStdString(relative), QByteArray("/")).toStdString() + anchor;
}

std::optional<std::string> resolve_target(const std::string& target, RewriteContext& context) {
    const auto& root = context.request.source_collection_root;
    const auto sourceDirectory = root / std::filesystem::path(context.source_path).parent_path();
    const auto candidate = target.starts_with("/") ? root / target.substr(1)
        : sourceDirectory / percent_decode(target);
    std::filesystem::path normalized;
    try { normalized = std::filesystem::weakly_canonical(candidate); }
    catch (...) { normalized = candidate; }
    if (!is_within_collection(root, normalized)) {
        context.diagnostic(TransferSeverity::Warning, "link_outside_collection",
            "link target outside collection, preserving original", target);
        return std::nullopt;
    }
    std::string relative;
    try { relative = std::filesystem::relative(normalized, root).generic_string(); }
    catch (...) { relative.clear(); }
    if (relative.empty()) {
        context.diagnostic(TransferSeverity::Warning, "link_unresolvable",
            "link target not inside collection", target);
        return std::nullopt;
    }
    return relative;
}

std::string rewrite_target(const std::string& original, RewriteContext& context) {
    if (is_remote_url(original)) return original;
    const auto hash = original.find('#');
    const auto anchor = hash == std::string::npos ? "" : original.substr(hash);
    const auto target = trim(original.substr(0, hash));
    if (target.empty()) return original;
    const auto relative = resolve_target(target, context);
    if (!relative) return original;
    const auto mapped = context.destinations.find(*relative);
    if (mapped != context.destinations.end()) {
        return context.relative_destination(mapped->second) + anchor;
    }
    const auto asset = context.request.source_collection_root / *relative;
    std::error_code error;
    if (std::filesystem::is_regular_file(asset, error) && !error) {
        return copy_asset(asset, original, target, anchor, context);
    }
    context.diagnostic(TransferSeverity::Warning, "link_target_missing",
        "link target missing: " + *relative, target);
    return original;
}

struct Replacement { size_t start; size_t end; std::string text; };
using Replacements = std::vector<Replacement>;

size_t skip_space(const std::string& body, size_t position, size_t end) {
    while (position < end && std::isspace(static_cast<unsigned char>(body[position]))) ++position;
    return position;
}

std::string inline_target(const std::string& inside) {
    for (size_t i = 1; i < inside.size(); ++i) {
        if (inside[i] == '"' && std::isspace(static_cast<unsigned char>(inside[i - 1]))) {
            return trim(inside.substr(0, i));
        }
    }
    return trim(inside);
}

void add_target_replacement(const std::string& target, size_t start, RewriteContext& context,
                            Replacements& replacements) {
    if (target.empty() || is_remote_url(target)) return;
    const auto rewritten = rewrite_target(target, context);
    if (rewritten != target) replacements.push_back({start, start + target.size(), rewritten});
}

void collect_inline(const std::string& body, RewriteContext& context, Replacements& replacements) {
    size_t position = 0;
    while (position < body.size()) {
        const auto open = body.find('[', position);
        if (open == std::string::npos) break;
        position = open + 1;
        if (open + 1 < body.size() && body[open + 1] == '[') { ++position; continue; }
        if (is_in_code(body, open)) continue;
        const auto close = body.find(']', open + 1);
        if (close == std::string::npos) break;
        position = close + 1;
        const auto paren = skip_space(body, close + 1, body.size());
        if (paren >= body.size() || body[paren] != '(') continue;
        const auto end = body.find(')', paren + 1);
        if (end == std::string::npos) break;
        const auto start = skip_space(body, paren + 1, end);
        add_target_replacement(inline_target(body.substr(start, end - start)), start, context, replacements);
        position = end + 1;
    }
}

void collect_reference_line(const std::string& line, size_t offset, RewriteContext& context,
                            Replacements& replacements) {
    const auto left = line.find('[');
    const auto right = line.find(']', left == std::string::npos ? 0 : left);
    const auto colon = line.find(':', right == std::string::npos ? 0 : right);
    if (left == std::string::npos || right == std::string::npos || colon == std::string::npos) return;
    const auto start = skip_space(line, colon + 1, line.size());
    auto end = start;
    while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) ++end;
    add_target_replacement(line.substr(start, end - start), offset + start, context, replacements);
}

void collect_references(const std::string& body, RewriteContext& context, Replacements& replacements) {
    size_t position = 0;
    while (position < body.size()) {
        const auto end = body.find('\n', position);
        if (!is_in_code(body, position)) {
            collect_reference_line(body.substr(position,
                end == std::string::npos ? std::string::npos : end - position),
                position, context, replacements);
        }
        if (end == std::string::npos) break;
        position = end + 1;
    }
}

struct WikiTarget { std::string target; std::string alias; std::string anchor; };
WikiTarget split_wiki_target(const std::string& inside) {
    WikiTarget parsed{inside, {}, {}};
    const auto bar = inside.find('|');
    const auto hash = inside.find('#');
    if (bar != std::string::npos) {
        parsed.target = inside.substr(0, bar);
        parsed.alias = inside.substr(bar + 1);
    } else if (hash != std::string::npos) {
        parsed.target = inside.substr(0, hash);
        parsed.anchor = inside.substr(hash);
    }
    parsed.target = trim(parsed.target);
    return parsed;
}

std::optional<std::string> wiki_id_destination(const std::string& target, RewriteContext& context) {
    if (target.find('/') != std::string::npos || target.find('.') != std::string::npos) return std::nullopt;
    if (!context.destinations.contains(context.source_path)) return std::nullopt;
    const auto& preview = *context.request.preview;
    for (const auto& entry : preview.source_id_value_to_native) {
        const auto id = entry.second.find(target);
        if (id == entry.second.end()) continue;
        const auto destination = preview.native_id_to_dest_rel.find(id->second);
        if (destination != preview.native_id_to_dest_rel.end()) {
            return context.relative_destination(destination->second);
        }
    }
    return std::nullopt;
}

std::string wiki_display(const WikiTarget& parsed, const std::string& rewritten) {
    if (!parsed.alias.empty()) return parsed.alias;
    const auto slash = rewritten.find_last_of('/');
    if (slash == std::string::npos) return parsed.target;
    auto base = rewritten.substr(slash + 1);
    base = base.substr(0, base.find('.'));
    return base.empty() ? parsed.target : base;
}

void collect_wiki_link(const std::string& body, size_t open, size_t close,
                        RewriteContext& context, Replacements& replacements) {
    const auto parsed = split_wiki_target(body.substr(open + 2, close - open - 2));
    if (parsed.target.empty() || is_remote_url(parsed.target)) return;
    const auto id_destination = wiki_id_destination(parsed.target, context);
    auto rewritten = id_destination ? *id_destination : rewrite_target(parsed.target, context);
    if (!parsed.anchor.empty() && rewritten.find('#') == std::string::npos) rewritten += parsed.anchor;
    if (rewritten == parsed.target) return;
    const bool embed = open > 0 && body[open - 1] == '!';
    const auto text = std::string(embed ? "![" : "[") + wiki_display(parsed, rewritten)
        + "](" + rewritten + ")";
    replacements.push_back({embed ? open - 1 : open, close + 2, text});
}

void collect_wiki(const std::string& body, RewriteContext& context, Replacements& replacements) {
    size_t position = 0;
    while (position + 1 < body.size()) {
        const auto open = body.find("[[", position);
        if (open == std::string::npos) break;
        position = open + 2;
        if (is_in_code(body, open)) continue;
        const auto close = body.find("]]", open + 2);
        if (close == std::string::npos) break;
        collect_wiki_link(body, open, close, context, replacements);
        position = close + 2;
    }
}

std::string apply_replacements(std::string body, Replacements replacements) {
    std::sort(replacements.begin(), replacements.end(), [](const auto& a, const auto& b) {
        return a.start > b.start;
    });
    size_t previous_start = std::string::npos;
    for (const auto& replacement : replacements) {
        if (replacement.end > previous_start) continue;
        body.replace(replacement.start, replacement.end - replacement.start, replacement.text);
        previous_start = replacement.start;
    }
    return body;
}

DestinationMap source_destinations(const TransferPreview& preview) {
    DestinationMap destinations;
    for (const auto& entry : preview.source_path_to_native_id) {
        const auto destination = preview.native_id_to_dest_rel.find(entry.second);
        if (destination != preview.native_id_to_dest_rel.end()) destinations[entry.first] = destination->second;
    }
    return destinations;
}
} // namespace

MarkdownRewriteResult rewrite_markdown_links(const MarkdownRewriteRequest& request) {
    MarkdownRewriteResult result;
    if (!request.preview) {
        result.diagnostics.push_back({TransferSeverity::Error, "bad_request", "preview null", {}, {}, {}});
        result.ok = false;
        return result;
    }
    const auto destinations = source_destinations(*request.preview);
    for (const auto& record : request.preview->records) {
        RewriteContext context{request, destinations, result, record.source_path};
        Replacements replacements;
        collect_inline(record.body, context, replacements);
        collect_references(record.body, context, replacements);
        collect_wiki(record.body, context, replacements);
        result.rewritten_body_by_source[record.source_path] = apply_replacements(record.body, replacements);
    }
    result.ok = std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == TransferSeverity::Error ||
            diagnostic.severity == TransferSeverity::Blocking || diagnostic.code == "cancelled";
    });
    return result;
}

} // namespace todobench::mdbase_transfer
