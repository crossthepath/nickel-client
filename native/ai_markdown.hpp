// Nickel AI answers in Markdown. The chat bubbles show its common parts instead of the raw marks:
// - **bold** and __bold__ (the menu's semibold font); *italic* and _italic_ lose their marks and stay plain, since the
//   menu has no italic font;
// - `code` (tinted, on a light background) and ``` code blocks;
// - # headings (bold), bullet lists (- * +) and numbered lists (1. or 1)), nested by indentation;
// - > quotes, --- rules, [links](url) (their text only) and \ escapes.
// Anything else, and any mark without its closing mark, stays as written. The player's own messages are never parsed.
//
// No ImGui here: LayOut takes a function that measures text, so tests/ai_ui_checks.cpp checks the parsing and the
// wrapping with a fake font. ai_ui.hpp draws the laid-out lines.
#ifndef NICKEL_AI_MARKDOWN_HPP
#define NICKEL_AI_MARKDOWN_HPP

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace nickel::ai::md {

// A piece of text with one style.
struct Span {
    std::string text;
    bool bold = false;
    bool code = false;
};

enum class BlockKind { Paragraph, Heading, Bullet, Numbered, CodeLine, Quote, Rule, Blank };

// One source line (a paragraph line, a list item, a heading...), with its inline spans.
struct Block {
    BlockKind kind = BlockKind::Paragraph;
    int level = 0;      // list nesting, 0 at the left; for a code line, its leading spaces
    std::string marker; // "3." for a numbered item
    std::vector<Span> spans;
};

namespace detail {

inline void Append(std::vector<Span>& out, std::string_view text, bool bold, bool code) {
    if (text.empty()) {
        return;
    }
    if (!out.empty() && out.back().bold == bold && out.back().code == code) {
        out.back().text.append(text);
    } else {
        out.push_back({std::string(text), bold, code});
    }
}

inline bool Word(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || static_cast<unsigned char>(c) >= 0x80;
}

// The closing `mark` after `from`, preceded by a non-space character; npos when there is none.
inline size_t Closing(std::string_view s, size_t from, std::string_view mark) {
    for (size_t at = s.find(mark, from); at != std::string_view::npos; at = s.find(mark, at + 1)) {
        if (at <= from || s[at - 1] == ' ') {
            continue;
        }
        if (mark.size() == 1) {
            if (at + 1 < s.size() && s[at + 1] == mark[0]) {
                ++at; // part of a double mark
                continue;
            }
            if (mark[0] == '_' && at + 1 < s.size() && Word(s[at + 1])) {
                continue; // snake_case: not a closing mark
            }
        }
        return at;
    }
    return std::string_view::npos;
}

inline void Inline(std::string_view s, bool bold, std::vector<Span>& out) {
    std::string plain;
    auto flush = [&] {
        Append(out, plain, bold, false);
        plain.clear();
    };
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() && std::ispunct(static_cast<unsigned char>(s[i + 1]))) {
            plain += s[i + 1];
            i += 2;
            continue;
        }
        if (c == '`') {
            const size_t end = s.find('`', i + 1);
            if (end != std::string_view::npos && end > i + 1) {
                flush();
                Append(out, s.substr(i + 1, end - i - 1), bold, true);
                i = end + 1;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 2 < s.size() && s[i + 1] == c && s[i + 2] != ' ') {
            const size_t end = Closing(s, i + 2, s.substr(i, 2));
            if (end != std::string_view::npos) {
                flush();
                Inline(s.substr(i + 2, end - i - 2), true, out);
                i = end + 2;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] != ' ' && s[i + 1] != c
            && (c == '*' || i == 0 || !Word(s[i - 1]))) {
            const size_t end = Closing(s, i + 1, s.substr(i, 1));
            if (end != std::string_view::npos) {
                flush();
                Inline(s.substr(i + 1, end - i - 1), bold, out); // italic: plain text without its marks
                i = end + 1;
                continue;
            }
        }
        if (c == '[') {
            const size_t close = s.find("](", i + 1);
            const size_t end = close == std::string_view::npos ? close : s.find(')', close + 2);
            if (end != std::string_view::npos && close > i + 1) {
                flush();
                Inline(s.substr(i + 1, close - i - 1), bold, out);
                i = end + 1;
                continue;
            }
        }
        plain += c;
        ++i;
    }
    flush();
}

inline std::string_view TrimLeft(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

inline std::string_view TrimRight(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

inline int Indent(std::string_view s) {
    int n = 0;
    for (char c : s) {
        if (c == ' ') {
            ++n;
        } else if (c == '\t') {
            n += 4;
        } else {
            break;
        }
    }
    return n;
}

inline bool IsRule(std::string_view t) {
    if (t.size() < 3 || (t[0] != '-' && t[0] != '*' && t[0] != '_')) {
        return false;
    }
    int count = 0;
    for (char c : t) {
        if (c == t[0]) {
            ++count;
        } else if (c != ' ') {
            return false;
        }
    }
    return count >= 3;
}

// Bytes of the UTF-8 character starting at `lead`.
inline size_t CharBytes(unsigned char lead) {
    return lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
}

} // namespace detail

// The spans of one line of text (no block marks).
inline std::vector<Span> ParseInline(std::string_view line) {
    std::vector<Span> out;
    detail::Inline(line, false, out);
    return out;
}

// The blocks of a whole answer. Runs of blank lines become one Blank block; leading and trailing ones are dropped.
inline std::vector<Block> Parse(std::string_view text) {
    std::vector<Block> blocks;
    bool fence = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view raw = detail::TrimRight(text.substr(start, end - start));
        start = end + 1;
        const std::string_view t = detail::TrimLeft(raw);
        if (t.substr(0, 3) == "```") {
            fence = !fence;
            continue;
        }
        if (fence) {
            blocks.push_back({BlockKind::CodeLine, detail::Indent(raw), {}, {{std::string(t), false, true}}});
            continue;
        }
        if (t.empty()) {
            if (!blocks.empty() && blocks.back().kind != BlockKind::Blank) {
                blocks.push_back({BlockKind::Blank, 0, {}, {}});
            }
            continue;
        }
        const int level = std::min(3, detail::Indent(raw) / 2);
        if (detail::IsRule(t)) {
            blocks.push_back({BlockKind::Rule, 0, {}, {}});
            continue;
        }
        size_t hashes = 0;
        while (hashes < t.size() && hashes < 6 && t[hashes] == '#') {
            ++hashes;
        }
        if (hashes > 0 && hashes < t.size() && t[hashes] == ' ') {
            Block b{BlockKind::Heading, 0, {}, ParseInline(detail::TrimLeft(t.substr(hashes)))};
            for (Span& span : b.spans) {
                span.bold = true;
            }
            blocks.push_back(std::move(b));
            continue;
        }
        if (t.size() > 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
            blocks.push_back({BlockKind::Bullet, level, {}, ParseInline(detail::TrimLeft(t.substr(2)))});
            continue;
        }
        size_t digits = 0;
        while (digits < t.size() && digits < 3 && std::isdigit(static_cast<unsigned char>(t[digits]))) {
            ++digits;
        }
        if (digits > 0 && digits + 1 < t.size() && (t[digits] == '.' || t[digits] == ')') && t[digits + 1] == ' ') {
            blocks.push_back({BlockKind::Numbered, level, std::string(t.substr(0, digits)) + ".",
                              ParseInline(detail::TrimLeft(t.substr(digits + 2)))});
            continue;
        }
        if (t[0] == '>') {
            blocks.push_back({BlockKind::Quote, 0, {}, ParseInline(detail::TrimLeft(t.substr(1)))});
            continue;
        }
        blocks.push_back({BlockKind::Paragraph, 0, {}, ParseInline(t)});
    }
    while (!blocks.empty() && blocks.back().kind == BlockKind::Blank) {
        blocks.pop_back();
    }
    return blocks;
}

// A laid-out piece of text: `x` from where the line's text starts, `w` its width.
struct Run {
    std::string text;
    bool bold = false;
    bool code = false;
    float x = 0;
    float w = 0;
};

struct Line {
    BlockKind kind = BlockKind::Paragraph;
    bool first = false; // the first line of its block: draws the bullet or the number
    int level = 0;
    std::string marker;
    float indent = 0; // where the text starts, from the left edge
    float y = 0;
    std::vector<Run> runs;
};

struct Layout {
    std::vector<Line> lines;
    float width = 0;  // the widest line, indent included
    float height = 0; // the last line's bottom
};

struct Metrics {
    float maxWidth = 0;   // available width
    float lineH = 0;      // one line of text
    float gap = 0;        // between paragraphs (a blank line), around a heading or a rule
    float itemGap = 0;    // between list items
    float indentStep = 0; // per list level
    float markerW = 0;    // room for a bullet or a number
};

// Width of `text` in the bold or regular font.
using Measure = std::function<float(const std::string& text, bool bold)>;

// Wraps the blocks at spaces (a word wider than the line is cut between characters).
inline Layout LayOut(const std::vector<Block>& blocks, const Metrics& m, const Measure& measure) {
    Layout out;
    float y = 0;
    bool any = false;
    BlockKind previous = BlockKind::Blank;
    const float space[2] = {measure(" ", false), measure(" ", true)};
    auto isList = [](BlockKind k) { return k == BlockKind::Bullet || k == BlockKind::Numbered; };
    for (const Block& block : blocks) {
        if (block.kind == BlockKind::Blank) {
            previous = block.kind;
            continue;
        }
        if (any) {
            if (previous == BlockKind::Blank || block.kind == BlockKind::Heading || block.kind == BlockKind::Rule
                || previous == BlockKind::Rule) {
                y += m.gap;
            } else if (isList(block.kind) && isList(previous)) {
                y += m.itemGap;
            }
        }
        any = true;
        previous = block.kind;
        Line line;
        line.kind = block.kind;
        line.first = true;
        line.level = block.level;
        line.marker = block.marker;
        line.y = y;
        if (block.kind == BlockKind::Rule) {
            out.lines.push_back(line);
            y += m.gap;
            continue;
        }
        if (isList(block.kind)) {
            line.indent = block.level * m.indentStep + m.markerW;
        } else if (block.kind == BlockKind::Quote) {
            line.indent = m.markerW * .6f;
        } else if (block.kind == BlockKind::CodeLine) {
            line.indent = std::min(m.maxWidth / 2, block.level * space[0]);
        }
        const float room = std::max(1.f, m.maxWidth - line.indent);
        float x = 0;
        bool pendingSpace = false;
        auto newLine = [&] {
            out.width = std::max(out.width, line.indent + x);
            out.lines.push_back(line);
            y += m.lineH;
            line.runs.clear();
            line.first = false;
            line.y = y;
            x = 0;
            pendingSpace = false;
        };
        auto put = [&](const std::string& word, bool bold, bool code, float w) {
            const float gap = pendingSpace && !line.runs.empty() ? space[bold ? 1 : 0] : 0;
            Run* last = line.runs.empty() ? nullptr : &line.runs.back();
            if (last && last->bold == bold && last->code == code) {
                last->text += (gap > 0 ? " " : "") + word;
                last->w = x + gap + w - last->x;
            } else {
                line.runs.push_back({word, bold, code, x + gap, w});
            }
            x += gap + w;
            pendingSpace = false;
        };
        for (const Span& span : block.spans) {
            const std::string& s = span.text;
            size_t i = 0;
            while (i < s.size()) {
                if (s[i] == ' ') {
                    pendingSpace = pendingSpace || !line.runs.empty();
                    ++i;
                    continue;
                }
                size_t end = s.find(' ', i);
                if (end == std::string::npos) {
                    end = s.size();
                }
                std::string word = s.substr(i, end - i);
                i = end;
                float w = measure(word, span.bold);
                const float gap = pendingSpace && !line.runs.empty() ? space[span.bold ? 1 : 0] : 0;
                if (!line.runs.empty() && x + gap + w > room) {
                    newLine();
                }
                while (x + w > room && word.size() > 1) { // too wide for the line: cut between characters
                    size_t cut = 0;
                    float cutW = 0;
                    for (size_t at = 0; at < word.size();) {
                        const size_t next = std::min(word.size(), at + detail::CharBytes(static_cast<unsigned char>(word[at])));
                        const float width = measure(word.substr(0, next), span.bold);
                        if (x + width > room && cut > 0) {
                            break;
                        }
                        cut = next;
                        cutW = width;
                        at = next;
                    }
                    if (cut >= word.size()) {
                        break;
                    }
                    put(word.substr(0, cut), span.bold, span.code, cutW);
                    newLine();
                    word = word.substr(cut);
                    w = measure(word, span.bold);
                }
                put(word, span.bold, span.code, w);
            }
        }
        newLine();
    }
    out.height = y;
    return out;
}

} // namespace nickel::ai::md

#endif // NICKEL_AI_MARKDOWN_HPP
