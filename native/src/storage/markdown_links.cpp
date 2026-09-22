// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/markdown_links.h"
#include "storage/command_transaction.h"
#include <QUrl>
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace todobench {
namespace {
size_t run(const std::string& text, size_t start, char character) {
    auto end = start;
    while (end < text.size() && text[end] == character) ++end;
    return end - start;
}
bool escaped(const std::string& text, size_t position) {
    size_t count = 0;
    while (position > count && text[position - count - 1] == '\\') ++count;
    return count % 2 != 0;
}
size_t spaces(const std::string& text, size_t position) {
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t' || text[position] == '\r')) ++position;
    return position;
}
struct FenceState { char marker{0}; size_t length{0}; };
bool code_line(const std::string& text, size_t line, size_t end, FenceState& fence) {
    const auto start = spaces(text, line);
    const auto marker = start < end ? text[start] : '\0';
    const auto length = (marker == '`' || marker == '~') ? run(text, start, marker) : 0;
    const bool fenced = fence.marker != 0;
    if (fence.marker && marker == fence.marker && length >= fence.length && spaces(text, start + length) >= end) fence.marker = 0;
    else if (!fence.marker && start - line <= 3 && length >= 3) { fence.marker = marker; fence.length = length; }
    return fenced || fence.marker || start - line >= 4 || (line < end && text[line] == '\t');
}
void mask_inline_code(const std::string& text, std::vector<bool>& mask) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (mask[i] || text[i] != '`' || escaped(text, i)) continue;
        const auto length = run(text, i, '`');
        auto close = text.find(std::string(length, '`'), i + length);
        while (close != std::string::npos && run(text, close, '`') != length)
            close = text.find(std::string(length, '`'), close + run(text, close, '`'));
        if (close != std::string::npos) {
            std::fill(mask.begin() + i, mask.begin() + close + length, true);
            i = close + length - 1;
        } else i += length - 1;
    }
}
std::vector<bool> code_mask(const std::string& text) {
    std::vector<bool> mask(text.size(), false);
    FenceState fence;
    for (size_t line = 0; line < text.size();) {
        auto end = text.find('\n', line);
        if (end == std::string::npos) end = text.size();
        if (code_line(text, line, end, fence)) std::fill(mask.begin() + line, mask.begin() + end, true);
        line = end + 1;
    }
    mask_inline_code(text, mask);
    return mask;
}
struct Span { size_t start, end; };
Span destination(const std::string& text, size_t position) {
    position = spaces(text, position);
    if (position >= text.size()) return {position, position};
    if (text[position] == '<') {
        auto end = text.find('>', position + 1);
        if (end == std::string::npos) return {position, position};
        return {position + 1, end};
    }
    auto end = position;
    int depth = 0;
    while (end < text.size()) {
        const auto ch = text[end];
        if (!escaped(text, end)) {
            if (std::isspace(static_cast<unsigned char>(ch))) break;
            if (ch == '(') ++depth;
            if (ch == ')' && depth-- == 0) break;
        }
        ++end;
    }
    return {position, end};
}
size_t label_end(const std::string& text, const std::vector<bool>& mask, size_t start) {
    int depth = 1;
    for (size_t index = start + 1; index < text.size(); ++index) {
        if (mask[index] || escaped(text, index)) continue;
        if (text[index] == '[') ++depth;
        if (text[index] == ']' && --depth == 0) return index;
    }
    return std::string::npos;
}
bool closes_inline(const std::string& text, Span target) {
    size_t end = target.end;
    if (target.start > 0 && text[target.start - 1] == '<') ++end;
    end = spaces(text, end);
    if (end >= text.size()) return false;
    if (text[end] == ')') return true;
    const char quote = text[end] == '(' ? ')' : text[end];
    if (quote != ')' && quote != '\'' && quote != '"') return false;
    auto close = text.find(quote, end + 1);
    while (close != std::string::npos && escaped(text, close)) close = text.find(quote, close + 1);
    if (close == std::string::npos) return false;
    close = spaces(text, close + 1);
    return close < text.size() && text[close] == ')';
}
struct Replacement { size_t start, end; std::string text; };
void add_destination(const std::string& text, Span span, bool wiki, const LinkRewriter& rewrite, std::vector<Replacement>& edits) {
    if (span.end <= span.start) return;
    const auto target = text.substr(span.start, span.end - span.start);
    const auto rewritten = rewrite(target, wiki);
    if (rewritten != target) edits.push_back({span.start, span.end, rewritten});
}
size_t wiki_link(const std::string& text, size_t start, const LinkRewriter& rewrite, bool convert, std::vector<Replacement>& edits) {
    const auto close = text.find("]]", start + 2);
    if (close == std::string::npos) return start + 1;
    const auto alias = text.find('|', start + 2);
    const auto end = alias < close ? alias : close;
    const auto original = text.substr(start + 2, end - start - 2);
    const auto target = rewrite(original, true);
    if (target == original) return close + 1;
    if (!convert) edits.push_back({start + 2, end, target});
    else {
        auto label = alias < close ? text.substr(alias + 1, close - alias - 1) : std::filesystem::path(target).stem().string();
        edits.push_back({start, close + 2, "[" + label + "](" + target + ")"});
    }
    return close + 1;
}
void reference_links(const std::string& text, const std::vector<bool>& mask, const LinkRewriter& rewrite, std::vector<Replacement>& edits) {
    for (size_t line = 0; line < text.size();) {
        const auto start = spaces(text, line);
        const auto close = text.find(']', start + 1);
        if (start < text.size() && !mask[start] && text[start] == '[' && close != std::string::npos && close + 1 < text.size() && text[close + 1] == ':')
            add_destination(text, destination(text, close + 2), false, rewrite, edits);
        const auto end = text.find('\n', line);
        if (end == std::string::npos) break;
        line = end + 1;
    }
}
bool within(const std::filesystem::path& path, const std::filesystem::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}
std::string unescape_path(std::string text) {
    for (size_t i = 0; i + 1 < text.size(); ++i)
        if (text[i] == '\\' && std::ispunct(static_cast<unsigned char>(text[i + 1]))) text.erase(i, 1);
    return QUrl::fromPercentEncoding(QByteArray::fromStdString(text)).toStdString();
}
bool local_target(const std::string& path, bool wiki) {
    if (path.empty() || path.starts_with("//")) return false;
    const bool absolute = std::filesystem::path(path).is_absolute();
    if (path.find(':') != std::string::npos && !absolute && !QUrl(QString::fromStdString(path)).isLocalFile()) return false;
    return !wiki || path.find('/') != std::string::npos || path.find('.') != std::string::npos;
}
std::string encode_relocated_path(const std::string& relative, const std::string& original) {
    const auto old_parts = QString::fromStdString(original).split('/');
    auto parts = QString::fromStdString(relative).split('/');
    for (auto& part : parts) {
        const auto found = std::find_if(old_parts.begin(), old_parts.end(), [&](const auto& old) { return unescape_path(old.toStdString()) == part.toStdString(); });
        part = found != old_parts.end() ? *found : QString::fromLatin1(QUrl::toPercentEncoding(part, "-._~"));
    }
    return parts.join('/').toStdString();
}
std::string relocated_destination(const std::string& original, const std::filesystem::path& source,
                                   const std::filesystem::path& target, bool wiki) {
    if (QUrl(QString::fromStdString(original)).isLocalFile())
        return QUrl::fromLocalFile(QString::fromStdString(target.string())).toEncoded().toStdString();
    auto relative = target.lexically_relative(source.parent_path()).generic_string();
    if (std::filesystem::path(original).is_absolute()) relative = target.generic_string();
    if (relative == unescape_path(original)) return original;
    if (wiki && original.find('%') == std::string::npos) return relative;
    return encode_relocated_path(relative, original);
}
std::string relocate_link(const std::string& target, bool wiki, const std::filesystem::path& source,
                          const std::filesystem::path& root, const DirectoryMoves& moves, bool source_moves) {
    const auto cut = target.find_first_of("#?");
    const auto path_text = target.substr(0, cut);
    if (!local_target(path_text, wiki)) return target;
    const auto url = QUrl(QString::fromStdString(path_text));
    const auto decoded = url.isLocalFile() ? url.toLocalFile().toStdString() : unescape_path(path_text);
    const auto path = (source.parent_path() / std::filesystem::path(decoded)).lexically_normal();
    if (!within(path, root) || !within(std::filesystem::weakly_canonical(path), std::filesystem::weakly_canonical(root))) return target;
    const auto new_source = source_moves ? moved_path(source, moves) : source;
    const auto new_target = moved_path(path, moves);
    if (new_target == path && (new_source == source || url.isLocalFile())) return target;
    return relocated_destination(path_text, new_source, new_target, wiki)
        + (cut == std::string::npos ? "" : target.substr(cut));
}
} // namespace

std::string rewrite_link_destinations(const std::string& text, const LinkRewriter& rewrite, bool convert_wiki) {
    const auto mask = code_mask(text);
    std::vector<Replacement> edits;
    for (size_t i = 0; i < text.size(); ++i) {
        if (mask[i] || text[i] != '[' || escaped(text, i)) continue;
        if (i + 1 < text.size() && text[i + 1] == '[') { i = wiki_link(text, i, rewrite, convert_wiki, edits); continue; }
        const auto close = label_end(text, mask, i);
        if (close == std::string::npos) continue;
        const auto paren = spaces(text, close + 1);
        if (paren >= text.size() || text[paren] != '(') continue;
        const auto target = destination(text, paren + 1);
        if (closes_inline(text, target)) add_destination(text, target, false, rewrite, edits);
    }
    reference_links(text, mask, rewrite, edits);
    std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return a.start > b.start; });
    auto result = text;
    auto boundary = text.size();
    for (const auto& edit : edits) {
        if (edit.end > boundary) continue;
        result.replace(edit.start, edit.end - edit.start, edit.text);
        boundary = edit.start;
    }
    return result;
}

std::filesystem::path moved_path(const std::filesystem::path& path, const DirectoryMoves& moves) {
    for (const auto& [from, to] : moves) if (within(path, from)) {
        const auto relative = path.lexically_relative(from);
        return relative == "." ? to : (to / relative).lexically_normal();
    }
    return path;
}

std::string rewrite_document_links(const std::string& markdown, const std::filesystem::path& source,
                                   const std::filesystem::path& root, const DirectoryMoves& moves, bool source_moves) {
    size_t body = 0;
    const size_t begin = markdown.starts_with("\xef\xbb\xbf") ? 3 : 0;
    if (markdown.compare(begin, 4, "---\n") == 0 || markdown.compare(begin, 5, "---\r\n") == 0) {
        size_t line = markdown.find('\n', begin) + 1;
        while (line < markdown.size()) {
            auto end = markdown.find('\n', line);
            if (end == std::string::npos) end = markdown.size();
            const auto delimiter = markdown.substr(line, end - line);
            if (delimiter == "---" || delimiter == "---\r") { body = std::min(end + 1, markdown.size()); break; }
            line = end + 1;
        }
    }
    return markdown.substr(0, body) + rewrite_link_destinations(markdown.substr(body),
        [&](const auto& target, bool wiki) { return relocate_link(target, wiki, source, root, moves, source_moves); });
}

void rewrite_workspace_links(const std::filesystem::path& root, const DirectoryMoves& moves, bool source_moves) {
    if (moves.empty()) return;
    auto* transaction = CommandTransaction::current();
    if (!transaction) throw std::runtime_error("Link updates require a transaction");
    // Includes task/project bodies and other live Markdown documents, excludes internal archives.
    for (std::filesystem::recursive_directory_iterator it(root), end; it != end; ++it) {
        if (it->path() == root / ".todobench" || it->is_symlink()) { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file() || it->path().extension() != ".md") continue;
        const auto source = it->path();
        if (!transaction->exists(source)) continue;
        const auto body = transaction->read(source);
        const auto rewritten = rewrite_document_links(body, source, root, moves, source_moves);
        if (body != rewritten) transaction->write(source, rewritten);
    }
}
} // namespace todobench
