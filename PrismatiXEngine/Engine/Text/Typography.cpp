#include "Engine/Text/Typography.h"
#include "Engine/Core/NumberParsing.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>

#if defined(PRISMATIX_UNICODE_TEXT_LIBS)
#include <fribidi/fribidi.h>
#include <linebreak.h>
#include <utf8proc.h>
#endif

namespace px::text { namespace {
std::vector<std::string> Glyphs(std::string_view value){std::vector<std::string> result;for(std::size_t index=0;index<value.size();){const unsigned char lead=value[index];std::size_t count=lead<0x80?1:(lead&0xE0)==0xC0?2:(lead&0xF0)==0xE0?3:4;count=std::min(count,value.size()-index);result.emplace_back(value.substr(index,count));index+=count;}return result;}
bool In(std::string_view glyph,std::string_view set){return set.find(glyph)!=std::string_view::npos;}
#if !defined(PRISMATIX_UNICODE_TEXT_LIBS)
struct Scalar { char32_t value=0;std::size_t start=0,end=0; };
std::vector<Scalar> Scalars(std::string_view text){std::vector<Scalar> result;for(std::size_t offset=0;offset<text.size();){const std::size_t start=offset;const unsigned char lead=text[offset++];char32_t value=0xfffd;std::size_t remaining=0;if(lead<0x80)value=lead;else if((lead&0xe0)==0xc0){value=lead&0x1f;remaining=1;}else if((lead&0xf0)==0xe0){value=lead&0x0f;remaining=2;}else if((lead&0xf8)==0xf0){value=lead&0x07;remaining=3;}for(std::size_t index=0;index<remaining&&offset<text.size();++index){const unsigned char next=text[offset];if((next&0xc0)!=0x80)break;value=(value<<6)|(next&0x3f);++offset;}result.push_back({value,start,offset});}return result;}
bool ExtendsCluster(char32_t value){return (value>=0x0300&&value<=0x036f)||(value>=0x0483&&value<=0x0489)||(value>=0x0591&&value<=0x05bd)||(value>=0x0610&&value<=0x061a)||(value>=0x064b&&value<=0x065f)||(value>=0x0900&&value<=0x0903)||(value>=0x1ab0&&value<=0x1aff)||(value>=0x1dc0&&value<=0x1dff)||(value>=0x20d0&&value<=0x20ff)||(value>=0xfe00&&value<=0xfe0f)||(value>=0xfe20&&value<=0xfe2f)||(value>=0x1f3fb&&value<=0x1f3ff)||(value>=0xe0100&&value<=0xe01ef)||(value>=0xe0020&&value<=0xe007f);}
bool RegionalIndicator(char32_t value){return value>=0x1f1e6&&value<=0x1f1ff;}
#endif
}
namespace {
struct StyleTag { std::string_view name; std::string_view value; bool closing = false; std::size_t length = 0; };
std::optional<StyleTag> ReadStyleTag(const std::string_view text) {
    if (!text.starts_with('[')) return std::nullopt;
    const auto close = text.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    std::string_view body = text.substr(1, close - 1);
    StyleTag tag{.closing = body.starts_with('/'), .length = close + 1};
    if (tag.closing) body.remove_prefix(1);
    const auto equals = body.find('=');
    tag.name = body.substr(0, equals);
    if (equals != std::string_view::npos) tag.value = body.substr(equals + 1);
    const bool valued = tag.name == "font" || tag.name == "size" || tag.name == "color";
    if (!valued && tag.name != "b" && tag.name != "i") return std::nullopt;
    if (tag.closing ? equals != std::string_view::npos
                    : valued != (equals != std::string_view::npos))
        return std::nullopt;
    return tag;
}
std::optional<Color> ParseHexColor(const std::string_view value) {
    if (!value.starts_with('#') || (value.size() != 7 && value.size() != 9)) return std::nullopt;
    std::uint8_t channels[4]{0, 0, 0, 255};
    for (std::size_t index = 1; index < value.size(); index += 2) {
        const auto result = std::from_chars(value.data() + index, value.data() + index + 2,
                                            channels[(index - 1) / 2], 16);
        if (result.ec != std::errc{} || result.ptr != value.data() + index + 2)
            return std::nullopt;
    }
    return Color{channels[0], channels[1], channels[2], channels[3]};
}
void ApplyStyleTag(TextRunStyle& style, const StyleTag& tag) {
    if (tag.name == "b") style.bold = true;
    else if (tag.name == "i") style.italic = true;
    else if (tag.name == "font") { if (!tag.value.empty()) style.font = std::string(tag.value); }
    else if (tag.name == "color") { if (const auto color = ParseHexColor(tag.value)) style.color = color; }
    else if (tag.name == "size") {
        const bool relative = tag.value.ends_with('%');
        const auto digits = relative ? tag.value.substr(0, tag.value.size() - 1) : tag.value;
        double parsed = 0.0;
        if (!ParseFiniteDouble(digits, parsed)) return;
        const float amount = static_cast<float>(parsed);
        if (!std::isfinite(amount) || amount <= 0.0f) return;
        if (relative) style.scale *= std::clamp(amount / 100.0f, 0.1f, 4.0f);
        else { style.size = std::clamp(static_cast<int>(std::lround(amount)), 1, 400); style.scale = 1.0f; }
    }
}
}  // namespace

std::size_t StyleTagLength(const std::string_view text) {
    const auto tag = ReadStyleTag(text);
    return tag ? tag->length : 0;
}

std::string StyledFontPath(const std::string_view path, const bool bold, const bool italic) {
    std::string styled(path);
    if (bold || italic) styled += bold && italic ? "|bi" : bold ? "|b" : "|i";
    return styled;
}

RichText ParseRubyMarkup(std::string_view markup) {
    RichText result;
    // Open style tags in order; closing one reapplies the rest, so tags need
    // not nest strictly.
    std::vector<StyleTag> open;
    TextRunStyle current;
    bool styled = false;
    std::size_t runStart = 0;
    const auto restyle = [&] {
        TextRunStyle next;
        for (const auto& tag : open) ApplyStyleTag(next, tag);
        if (next == current) return;
        if (result.plain.size() > runStart)
            result.runs.push_back({runStart, result.plain.size() - runStart, current});
        runStart = result.plain.size();
        current = std::move(next);
    };
    for (std::size_t index = 0; index < markup.size();) {
        const auto rest = markup.substr(index);
        if (rest.starts_with("[br]")) { result.plain += '\n'; index += 4; continue; }
        if (rest.starts_with("[ruby=")) {
            const auto header = markup.find(']', index);
            const auto close = markup.find("[/ruby]", header == std::string_view::npos ? index : header);
            if (header != std::string_view::npos && close != std::string_view::npos) {
                const std::string reading(markup.substr(index + 6, header - index - 6));
                const std::string base(markup.substr(header + 1, close - header - 1));
                result.ruby.push_back({result.plain, base, reading});
                result.plain += base;
                index = close + 7;
                continue;
            }
        }
        if (const auto tag = ReadStyleTag(rest)) {
            styled = true;
            if (!tag->closing) open.push_back(*tag);
            else if (const auto found = std::find_if(open.rbegin(), open.rend(),
                         [&](const StyleTag& entry) { return entry.name == tag->name; });
                     found != open.rend())
                open.erase(std::next(found).base());
            restyle();
            index += tag->length;
            continue;
        }
        if (markup[index] == '[') {
            const auto close = markup.find(']', index);
            if (close != std::string_view::npos) { index = close + 1; continue; }
        }
        result.plain.push_back(markup[index++]);
    }
    if (styled) {
        if (result.plain.size() > runStart)
            result.runs.push_back({runStart, result.plain.size() - runStart, current});
        if (std::ranges::all_of(result.runs, [](const TextRun& run) { return run.style.Plain(); }))
            result.runs.clear();
    }
    return result;
}
BidiLayoutInfo ResolveBidiDirections(const std::string_view text,
                                     const bool localeRightToLeft) {
    BidiLayoutInfo result;
    result.baseRightToLeft = localeRightToLeft;
#if defined(PRISMATIX_UNICODE_TEXT_LIBS)
    if (text.empty()) return result;
    std::vector<FriBidiChar> codepoints;
    std::vector<std::size_t> byteOffsets;
    codepoints.reserve(text.size());
    byteOffsets.reserve(text.size());
    std::size_t offset = 0;
    while (offset < text.size()) {
        utf8proc_int32_t codepoint = 0;
        const auto consumed = utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t*>(text.data() + offset),
            static_cast<utf8proc_ssize_t>(text.size() - offset), &codepoint);
        if (consumed <= 0) return result;
        byteOffsets.push_back(offset);
        codepoints.push_back(static_cast<FriBidiChar>(codepoint));
        offset += static_cast<std::size_t>(consumed);
    }
    std::vector<FriBidiCharType> types(codepoints.size());
    std::vector<FriBidiBracketType> brackets(codepoints.size());
    std::vector<FriBidiLevel> levels(codepoints.size());
    fribidi_get_bidi_types(codepoints.data(),
                           static_cast<FriBidiStrIndex>(codepoints.size()),
                           types.data());
    fribidi_get_bracket_types(
        codepoints.data(), static_cast<FriBidiStrIndex>(codepoints.size()),
        types.data(), brackets.data());
    FriBidiParType base = localeRightToLeft ? FRIBIDI_PAR_WRTL
                                            : FRIBIDI_PAR_WLTR;
    if (fribidi_get_par_embedding_levels_ex(
            types.data(), brackets.data(),
            static_cast<FriBidiStrIndex>(codepoints.size()), &base,
            levels.data()) == 0)
        return result;
    result.baseRightToLeft = FRIBIDI_IS_RTL(base);
    result.directions.reserve(codepoints.size());
    for (std::size_t index = 0; index < codepoints.size(); ++index)
        result.directions.push_back(
            {byteOffsets[index], (levels[index] & 1u) != 0});
#else
    (void)text;
#endif
    return result;
}
std::vector<std::size_t> GraphemeBoundaries(std::string_view text){
#if defined(PRISMATIX_UNICODE_TEXT_LIBS)
    std::vector<std::size_t> result{0};
    if(text.empty())return result;
    std::size_t offset=0;
    utf8proc_int32_t previous=0,current=0,state=0;
    utf8proc_ssize_t consumed=utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(text.data()),
        static_cast<utf8proc_ssize_t>(text.size()),&previous);
    if(consumed<=0){for(std::size_t i=1;i<=text.size();++i)result.push_back(i);return result;}
    offset=static_cast<std::size_t>(consumed);
    while(offset<text.size()){
        consumed=utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t*>(text.data()+offset),
            static_cast<utf8proc_ssize_t>(text.size()-offset),&current);
        if(consumed<=0){result.push_back(++offset);state=0;previous=0;continue;}
        if(utf8proc_grapheme_break_stateful(previous,current,&state))
            result.push_back(offset);
        previous=current;offset+=static_cast<std::size_t>(consumed);
    }
    if(result.back()!=text.size())result.push_back(text.size());
    return result;
#else
    const auto scalars=Scalars(text);std::vector<std::size_t> result{0};for(std::size_t index=1;index<scalars.size();++index){const char32_t previous=scalars[index-1].value,current=scalars[index].value;bool joins=ExtendsCluster(current)||current==0x200d||previous==0x200d||(previous==0x000d&&current==0x000a);if(RegionalIndicator(previous)&&RegionalIndicator(current)){std::size_t preceding=0;for(std::size_t scan=index;scan>0&&RegionalIndicator(scalars[scan-1].value);--scan)++preceding;joins=(preceding%2)==1;}if(!joins)result.push_back(scalars[index].start);}if(result.back()!=text.size())result.push_back(text.size());return result;
#endif
}

std::vector<std::size_t> LineBreakBoundaries(
    const std::string_view text,const std::string_view language){
    std::vector<std::size_t> result{0};
    if(text.empty())return result;
#if defined(PRISMATIX_UNICODE_TEXT_LIBS)
    std::vector<char> breaks(text.size(),LINEBREAK_NOBREAK);
    const std::string locale(language);
    set_linebreaks_utf8(
        reinterpret_cast<const utf8_t*>(text.data()),text.size(),
        locale.empty()?nullptr:locale.c_str(),breaks.data());
    const auto graphemes=GraphemeBoundaries(text);
    for(std::size_t index=1;index<graphemes.size();++index){
        const std::size_t boundary=graphemes[index];
        const char decision=breaks[boundary-1];
        if(decision==LINEBREAK_ALLOWBREAK||decision==LINEBREAK_MUSTBREAK)
            result.push_back(boundary);
    }
#else
    const auto graphemes=GraphemeBoundaries(text);
    result.insert(result.end(),graphemes.begin()+1,graphemes.end());
#endif
    if(result.back()!=text.size())result.push_back(text.size());
    return result;
}

std::string ApplyCjkKinsoku(std::string_view text,std::size_t maxColumns){
    if(maxColumns==0)return std::string(text);
    const auto boundaries=GraphemeBoundaries(text);
    const auto unicodeBreaks=LineBreakBoundaries(text,"ja");
    const std::vector<std::size_t> breakSet(unicodeBreaks.begin(),unicodeBreaks.end());
    std::string output;std::size_t column=0;
    constexpr std::string_view noStart="、。，．！？)]｝」』】》〉”’ー";
    constexpr std::string_view noEnd="([｛「『【《〈“‘";
    for(std::size_t index=0;index+1<boundaries.size();++index){
        const auto glyph=text.substr(boundaries[index],boundaries[index+1]-boundaries[index]);
        if(glyph=="\n"){output+=glyph;column=0;continue;}
        const auto previous=index==0?std::string_view{}:
            text.substr(boundaries[index-1],boundaries[index]-boundaries[index-1]);
        const bool unicodeAllows=index>0&&std::binary_search(
            breakSet.begin(),breakSet.end(),boundaries[index]);
        if(column>=maxColumns&&!In(glyph,noStart)&&!In(previous,noEnd)&&
           (unicodeAllows||column>maxColumns)){
            output+='\n';column=0;
        }
        output+=glyph;++column;
    }
    return output;
}
std::vector<std::size_t> KinsokuBreakBoundaries(const std::string_view text,
                                                const std::string_view language) {
    // 禁則: closing punctuation, small kana and prolonged sound marks never
    // start a line; opening brackets never end one.
    constexpr std::string_view noStart =
        "、。，．・：；？！ー…‥」』）〕］｝〉》】〙〗’”々ゝゞヽヾぁぃぅぇぉっゃゅょゎゕゖ"
        "ァィゥェォッャュョヮヵヶ‐゠–〜～!),.:;?]}";
    constexpr std::string_view noEnd = "「『（〔［｛〈《【〘〖‘“([{";
    const auto glyphs = GraphemeBoundaries(text);
    const auto glyphAt = [&](const std::size_t offset) {
        const auto found = std::upper_bound(glyphs.begin(), glyphs.end(), offset);
        const std::size_t end = found == glyphs.end() ? text.size() : *found;
        return text.substr(offset, end - offset);
    };
    const auto glyphBefore = [&](const std::size_t offset) {
        const auto found = std::lower_bound(glyphs.begin(), glyphs.end(), offset);
        const std::size_t start = found == glyphs.begin() ? 0 : *std::prev(found);
        return text.substr(start, offset - start);
    };
    std::vector<std::size_t> candidates;
    for (const std::size_t boundary : LineBreakBoundaries(text, language)) {
        if (boundary == 0) continue;
        const bool forced = text[boundary - 1] == '\n';
        if (!forced && boundary < text.size() &&
            (In(glyphAt(boundary), noStart) || In(glyphBefore(boundary), noEnd)))
            continue;
        candidates.push_back(boundary);
    }
    return candidates;
}

std::vector<std::size_t> WrapBreaks(const std::string_view text, const float width,
                                    const std::function<float(std::string_view)>& measure,
                                    const std::string_view language) {
    std::vector<std::size_t> result;
    if (text.empty() || width <= 0.0f || !measure) return result;
    const auto candidates = KinsokuBreakBoundaries(text, language);
    std::size_t lineStart = 0;
    std::size_t lastFit = std::string_view::npos;
    const auto fits = [&](const std::size_t end) {
        std::string_view line = text.substr(lineStart, end - lineStart);
        while (!line.empty() && (line.back() == '\n' || line.back() == ' ')) line.remove_suffix(1);
        return measure(line) <= width;
    };
    for (const std::size_t boundary : candidates) {
        if (fits(boundary)) {
            lastFit = boundary;
        } else if (lastFit != std::string_view::npos && lastFit > lineStart) {
            result.push_back(lastFit);
            lineStart = lastFit;
            lastFit = fits(boundary) ? boundary : std::string_view::npos;
            if (lastFit == std::string_view::npos && boundary < text.size()) {
                // One unbreakable run wider than the line: let it overflow.
                result.push_back(boundary);
                lineStart = boundary;
            }
        } else if (boundary < text.size()) {
            result.push_back(boundary);
            lineStart = boundary;
            lastFit = std::string_view::npos;
        }
        if (text[boundary - 1] == '\n') {
            lineStart = boundary;
            lastFit = std::string_view::npos;
        }
    }
    // Explicit newlines already break; only report inserted ones.
    std::erase_if(result, [&](const std::size_t at) { return at == 0 || text[at - 1] == '\n'; });
    return result;
}

namespace {
// Byte length of a control character (other than '\n') starting at `at`:
// C0 controls and DEL are one byte, C1 controls U+0080-U+009F are C2 80-9F.
std::size_t ControlLength(const std::string_view text, const std::size_t at) {
    const auto byte = static_cast<unsigned char>(text[at]);
    if ((byte < 0x20 && byte != '\n') || byte == 0x7f) return 1;
    if (byte == 0xc2 && at + 1 < text.size()) {
        const auto next = static_cast<unsigned char>(text[at + 1]);
        if (next >= 0x80 && next <= 0x9f) return 2;
    }
    return 0;
}
}  // namespace

bool HasControlCharacters(const std::string_view text) {
    for (std::size_t index = 0; index < text.size(); ++index)
        if (ControlLength(text, index) > 0) return true;
    return false;
}

DrawableText MapDrawableText(const std::string_view text) {
    DrawableText result;
    result.text.reserve(text.size());
    result.sourceStart.reserve(text.size());
    result.sourceEnd.reserve(text.size());
    const auto emit = [&](const char byte, const std::size_t start, const std::size_t end) {
        result.text.push_back(byte);
        result.sourceStart.push_back(start);
        result.sourceEnd.push_back(end);
    };
    for (std::size_t index = 0; index < text.size();) {
        const std::size_t control = ControlLength(text, index);
        if (control == 0) {
            emit(text[index], index, index + 1);
            ++index;
        } else if (text[index] == '\r') {
            const std::size_t end = index + 1 < text.size() && text[index + 1] == '\n'
                ? index + 2 : index + 1;
            emit('\n', index, end);
            index = end;
        } else if (text[index] == '\t') {
            emit(' ', index, index + 1);
            ++index;
        } else {
            index += control;
        }
    }
    return result;
}

std::string MakeDrawable(const std::string_view text) {
    if (!HasControlCharacters(text)) return std::string(text);
    return MapDrawableText(text).text;
}

std::vector<VerticalGlyph> LayoutVertical(std::string_view text,std::size_t rowsPerColumn){rowsPerColumn=std::max<std::size_t>(1,rowsPerColumn);std::vector<VerticalGlyph> result;int column=0,row=0;for(const auto& glyph:Glyphs(text)){if(glyph=="\n"||row>=static_cast<int>(rowsPerColumn)){++column;row=0;if(glyph=="\n")continue;}const bool rotate=glyph.size()==1&&glyph[0]>=0x21&&glyph[0]<=0x7e;result.push_back({glyph,column,row++,rotate});}return result;}
}  // namespace px::text
