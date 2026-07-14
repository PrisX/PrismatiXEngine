#include "Engine/Graphics/Renderer2D.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace px::graphics {

namespace {
constexpr std::size_t kTextCacheLimit = 600;

SDL_FColor ToFColor(Color c, float alphaScale = 1.0f) { return SDL_FColor{ c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (c.a / 255.0f) * alphaScale }; }

void AppendArc(std::vector<SDL_Vertex>& out, float cx, float cy, float radius, float a0, float a1, SDL_FColor color, int segments) {
    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(std::max(segments, 1));
        const float a = a0 + (a1 - a0) * t;
        SDL_Vertex v{};
        v.position = SDL_FPoint{ cx + std::cos(a) * radius, cy + std::sin(a) * radius };
        v.color = color;
        out.push_back(v);
    }
}

std::string MakeKey(const std::string& text, const std::string& font, int size, Color c, int outline, int wrap, int ss, int alignment) {
    return text + "\x01" + font + "\x01" + std::to_string(size) + "\x01" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "," + std::to_string(c.a) + "\x01" + std::to_string(outline) + "\x01" + std::to_string(wrap) + "\x01" +
           std::to_string(ss)+"\x01"+std::to_string(alignment);
}
}  // namespace

Renderer2D::Renderer2D(SDL_Renderer* renderer, AssetCache& assets)
    : m_renderer(renderer), m_assets(assets) {}

Renderer2D::~Renderer2D() {
    ClearTextCache();
}

void Renderer2D::SetLogicalSize(int width, int height, bool updateTextDensity) {
    m_logicalW = width;
    m_logicalH = height;
    SDL_SetRenderLogicalPresentation(m_renderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    if(updateTextDensity&&m_textSamplingMode==TextSamplingMode::Auto&&width>0&&height>0){int outputW=width,outputH=height;(void)SDL_GetCurrentRenderOutputSize(m_renderer,&outputW,&outputH);const float density=std::min(static_cast<float>(outputW)/width,static_cast<float>(outputH)/height);const int effective=std::clamp(static_cast<int>(std::ceil(std::max(2.0f,density))),2,4);if(effective!=m_effectiveTextSupersample){m_effectiveTextSupersample=effective;ClearTextCache();}}
}

void Renderer2D::GetLogicalSize(int& width, int& height) const {
    width = m_logicalW;
    height = m_logicalH;
}

void Renderer2D::DrawRect(const Rect& rect, Color color) {
    color=TransformColor(color);const auto p0=TransformPoint({rect.x,rect.y}),p1=TransformPoint({rect.x+rect.w,rect.y}),p2=TransformPoint({rect.x+rect.w,rect.y+rect.h}),p3=TransformPoint({rect.x,rect.y+rect.h});const SDL_FColor fc=ToFColor(color);SDL_Vertex vertices[4]{{{p0.x,p0.y},fc,{}},{{p1.x,p1.y},fc,{}},{{p2.x,p2.y},fc,{}},{{p3.x,p3.y},fc,{}}};constexpr int indices[6]{0,1,2,0,2,3};SDL_SetRenderDrawBlendMode(m_renderer,SDL_BLENDMODE_BLEND);SDL_RenderGeometry(m_renderer,nullptr,vertices,4,indices,6);
}

void Renderer2D::DrawRoundedRect(const Rect& rect, float radius, Color color) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    const float r = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    if (r <= 0.5f) {
        DrawRect(rect, color);
        return;
    }

    const SDL_FColor fc = ToFColor(TransformColor(color));
    const float left = rect.x;
    const float top = rect.y;
    const float right = left + rect.w;
    const float bottom = top + rect.h;
    constexpr float kPi = 3.14159265358979323846f;
    constexpr int kSeg = 6;

    std::vector<SDL_Vertex> ring;
    ring.reserve(4 * (kSeg + 1));
    AppendArc(ring, right - r, top + r, r, -kPi * 0.5f, 0.0f, fc, kSeg);
    AppendArc(ring, right - r, bottom - r, r, 0.0f, kPi * 0.5f, fc, kSeg);
    AppendArc(ring, left + r, bottom - r, r, kPi * 0.5f, kPi, fc, kSeg);
    AppendArc(ring, left + r, top + r, r, kPi, kPi * 1.5f, fc, kSeg);

    SDL_Vertex center{};
    center.position = SDL_FPoint{ left + rect.w * 0.5f, top + rect.h * 0.5f };
    center.color = fc;

    std::vector<SDL_Vertex> fan;
    fan.reserve(ring.size() * 3);
    for (std::size_t i = 0; i < ring.size(); ++i) {
        fan.push_back(center);
        fan.push_back(ring[i]);
        fan.push_back(ring[(i + 1) % ring.size()]);
    }

    for(auto& vertex:fan){const auto transformed=TransformPoint({vertex.position.x,vertex.position.y});vertex.position={transformed.x,transformed.y};}
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(m_renderer, nullptr, fan.data(), static_cast<int>(fan.size()), nullptr, 0);
}

Vec2 Renderer2D::TransformPoint(const Vec2 point) const {const auto& t=m_transformStack.back();return {t.a*point.x+t.c*point.y+t.tx+m_camX,t.b*point.x+t.d*point.y+t.ty+m_camY};}
Color Renderer2D::TransformColor(const Color color) const {const auto tint=m_transformStack.back().modulate;return {static_cast<std::uint8_t>((static_cast<unsigned>(color.r)*tint.r)/255),static_cast<std::uint8_t>((static_cast<unsigned>(color.g)*tint.g)/255),static_cast<std::uint8_t>((static_cast<unsigned>(color.b)*tint.b)/255),static_cast<std::uint8_t>((static_cast<unsigned>(color.a)*tint.a)/255)};}
Rect Renderer2D::TransformBounds(const Rect rect) const {const std::array<Vec2,4> points{TransformPoint({rect.x,rect.y}),TransformPoint({rect.x+rect.w,rect.y}),TransformPoint({rect.x+rect.w,rect.y+rect.h}),TransformPoint({rect.x,rect.y+rect.h})};float left=points[0].x,right=left,top=points[0].y,bottom=top;for(const auto point:points){left=std::min(left,point.x);right=std::max(right,point.x);top=std::min(top,point.y);bottom=std::max(bottom,point.y);}return {left,top,right-left,bottom-top};}
void Renderer2D::PushTransform(const Vec2 pivot,const Vec2 scale,const float rotationDegrees,const Color modulate){const float radians=rotationDegrees*3.14159265358979323846f/180.0f,cs=std::cos(radians),sn=std::sin(radians);TransformState local{cs*scale.x,sn*scale.x,-sn*scale.y,cs*scale.y,0,0,modulate};local.tx=pivot.x-(local.a*pivot.x+local.c*pivot.y);local.ty=pivot.y-(local.b*pivot.x+local.d*pivot.y);const auto& parent=m_transformStack.back();TransformState combined;combined.a=parent.a*local.a+parent.c*local.b;combined.b=parent.b*local.a+parent.d*local.b;combined.c=parent.a*local.c+parent.c*local.d;combined.d=parent.b*local.c+parent.d*local.d;combined.tx=parent.a*local.tx+parent.c*local.ty+parent.tx;combined.ty=parent.b*local.tx+parent.d*local.ty+parent.ty;combined.modulate={static_cast<std::uint8_t>((static_cast<unsigned>(parent.modulate.r)*modulate.r)/255),static_cast<std::uint8_t>((static_cast<unsigned>(parent.modulate.g)*modulate.g)/255),static_cast<std::uint8_t>((static_cast<unsigned>(parent.modulate.b)*modulate.b)/255),static_cast<std::uint8_t>((static_cast<unsigned>(parent.modulate.a)*modulate.a)/255)};m_transformStack.push_back(combined);}
void Renderer2D::PopTransform(){if(m_transformStack.size()>1)m_transformStack.pop_back();}

void Renderer2D::SetPreviewContext(float displayPixelsPerLogical, bool clarityCompensation) {
    m_displayPixelsPerLogical = std::max(0.01f, displayPixelsPerLogical);
    m_clarityCompensation = clarityCompensation;
}

void Renderer2D::DrawBorder(const Rect& rect, float width, float, Color color) {
    if (rect.w <= 0.0f || rect.h <= 0.0f || width <= 0.0f) return;
    const float pixel = 1.0f / m_displayPixelsPerLogical;
    const float stroke = m_clarityCompensation ? std::max(width, pixel) : width;
    const auto align = [&](float value) {
        return m_clarityCompensation ? std::round(value / pixel) * pixel : value;
    };
    const float left=align(rect.x),top=align(rect.y),right=align(rect.x+rect.w),bottom=align(rect.y+rect.h);
    DrawRect({left,top,std::max(0.0f,right-left),stroke},color);
    DrawRect({left,std::max(top,bottom-stroke),std::max(0.0f,right-left),stroke},color);
    DrawRect({left,top+stroke,stroke,std::max(0.0f,bottom-top-stroke*2)},color);
    DrawRect({std::max(left,right-stroke),top+stroke,stroke,std::max(0.0f,bottom-top-stroke*2)},color);
}

void Renderer2D::PushClip(const Rect& requested) {
    Rect clip = TransformBounds(requested);
    if (!m_clipStack.empty()) {
        const Rect parent = m_clipStack.back();
        const float left = std::max(parent.x, clip.x);
        const float top = std::max(parent.y, clip.y);
        const float right = std::min(parent.x + parent.w, clip.x + clip.w);
        const float bottom = std::min(parent.y + parent.h, clip.y + clip.h);
        clip = {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
    }
    m_clipStack.push_back(clip);
    const int left=static_cast<int>(std::floor(clip.x)),top=static_cast<int>(std::floor(clip.y));
    const int right=static_cast<int>(std::ceil(clip.x+clip.w)),bottom=static_cast<int>(std::ceil(clip.y+clip.h));
    const SDL_Rect value{left,top,std::max(0,right-left),std::max(0,bottom-top)};
    SDL_SetRenderClipRect(m_renderer, &value);
}

void Renderer2D::PopClip() {
    if (m_clipStack.empty()) return;
    m_clipStack.pop_back();
    if (m_clipStack.empty()) {
        SDL_SetRenderClipRect(m_renderer, nullptr);
    } else {
        const Rect clip = m_clipStack.back();
        const int left=static_cast<int>(std::floor(clip.x)),top=static_cast<int>(std::floor(clip.y));
        const int right=static_cast<int>(std::ceil(clip.x+clip.w)),bottom=static_cast<int>(std::ceil(clip.y+clip.h));
        const SDL_Rect value{left,top,std::max(0,right-left),std::max(0,bottom-top)};
        SDL_SetRenderClipRect(m_renderer, &value);
    }
}

void Renderer2D::Blit(SDL_Texture* texture, const Rect& dst, std::uint8_t alpha) {
    if (!texture) {
        return;
    }
    const auto p0=TransformPoint({dst.x,dst.y}),p1=TransformPoint({dst.x+dst.w,dst.y}),p2=TransformPoint({dst.x+dst.w,dst.y+dst.h}),p3=TransformPoint({dst.x,dst.y+dst.h});const Color color=TransformColor({255,255,255,alpha});const SDL_FColor fc=ToFColor(color);SDL_Vertex vertices[4]{{{p0.x,p0.y},fc,{0,0}},{{p1.x,p1.y},fc,{1,0}},{{p2.x,p2.y},fc,{1,1}},{{p3.x,p3.y},fc,{0,1}}};constexpr int indices[6]{0,1,2,0,2,3};SDL_RenderGeometry(m_renderer,texture,vertices,4,indices,6);
}

void Renderer2D::BlitRegion(SDL_Texture* texture,const Rect& sourcePixels,const Rect& dst,const std::uint8_t alpha){
    if(!texture||dst.w<=0||dst.h<=0||sourcePixels.w<=0||sourcePixels.h<=0)return;int width=0,height=0;AssetCache::TextureSize(texture,width,height);if(width<=0||height<=0)return;
    const float u0=sourcePixels.x/static_cast<float>(width),v0=sourcePixels.y/static_cast<float>(height),u1=(sourcePixels.x+sourcePixels.w)/static_cast<float>(width),v1=(sourcePixels.y+sourcePixels.h)/static_cast<float>(height);
    const auto p0=TransformPoint({dst.x,dst.y}),p1=TransformPoint({dst.x+dst.w,dst.y}),p2=TransformPoint({dst.x+dst.w,dst.y+dst.h}),p3=TransformPoint({dst.x,dst.y+dst.h});const SDL_FColor color=ToFColor(TransformColor({255,255,255,alpha}));
    SDL_Vertex vertices[4]{{{p0.x,p0.y},color,{u0,v0}},{{p1.x,p1.y},color,{u1,v0}},{{p2.x,p2.y},color,{u1,v1}},{{p3.x,p3.y},color,{u0,v1}}};constexpr int indices[6]{0,1,2,0,2,3};SDL_RenderGeometry(m_renderer,texture,vertices,4,indices,6);
}

void Renderer2D::DrawImage(const std::string& path, const Rect& dst, std::uint8_t alpha) { Blit(m_assets.Texture(path), dst, alpha); }

void Renderer2D::DrawImageInRect(const std::string& path, const Rect& bounds,
                                 ContentScaleMode mode, HorizontalAlignment horizontal,
                                 VerticalAlignment vertical, std::uint8_t alpha) {
    SDL_Texture* texture=m_assets.Texture(path);if(!texture||bounds.w<=0||bounds.h<=0)return;
    int tw=0,th=0;AssetCache::TextureSize(texture,tw,th);if(tw<=0||th<=0)return;
    if(mode==ContentScaleMode::Stretch){Blit(texture,bounds,alpha);return;}
    float scale=1.0f;
    if(mode==ContentScaleMode::Fit)scale=std::min(bounds.w/static_cast<float>(tw),bounds.h/static_cast<float>(th));
    else if(mode==ContentScaleMode::Fill)scale=std::max(bounds.w/static_cast<float>(tw),bounds.h/static_cast<float>(th));
    const float w=tw*scale,h=th*scale;float x=bounds.x,y=bounds.y;
    if(horizontal==HorizontalAlignment::Center)x+=(bounds.w-w)*.5f;else if(horizontal==HorizontalAlignment::Right)x+=bounds.w-w;
    if(vertical==VerticalAlignment::Center)y+=(bounds.h-h)*.5f;else if(vertical==VerticalAlignment::Bottom)y+=bounds.h-h;
    PushClip(bounds);Blit(texture,{x,y,w,h},alpha);PopClip();
}

void Renderer2D::DrawNinePatch(const std::string& path,const Rect& bounds,Rect margins,const bool drawCenter,const std::uint8_t alpha){
    SDL_Texture* texture=m_assets.Texture(path);if(!texture||bounds.w<=0||bounds.h<=0)return;int width=0,height=0;AssetCache::TextureSize(texture,width,height);if(width<=0||height<=0)return;
    float left=std::clamp(margins.x,0.0f,static_cast<float>(width)),top=std::clamp(margins.y,0.0f,static_cast<float>(height));
    float right=std::clamp(margins.w,0.0f,static_cast<float>(width)-left),bottom=std::clamp(margins.h,0.0f,static_cast<float>(height)-top);
    const float destinationLeft=std::min(left,bounds.w*.5f),destinationRight=std::min(right,bounds.w-destinationLeft);
    const float destinationTop=std::min(top,bounds.h*.5f),destinationBottom=std::min(bottom,bounds.h-destinationTop);
    const float sourceX[4]{0,left,static_cast<float>(width)-right,static_cast<float>(width)};
    const float sourceY[4]{0,top,static_cast<float>(height)-bottom,static_cast<float>(height)};
    const float destinationX[4]{bounds.x,bounds.x+destinationLeft,bounds.x+bounds.w-destinationRight,bounds.x+bounds.w};
    const float destinationY[4]{bounds.y,bounds.y+destinationTop,bounds.y+bounds.h-destinationBottom,bounds.y+bounds.h};
    for(int y=0;y<3;++y)for(int x=0;x<3;++x){if(!drawCenter&&x==1&&y==1)continue;BlitRegion(texture,{sourceX[x],sourceY[y],sourceX[x+1]-sourceX[x],sourceY[y+1]-sourceY[y]},{destinationX[x],destinationY[y],destinationX[x+1]-destinationX[x],destinationY[y+1]-destinationY[y]},alpha);}
}

void Renderer2D::DrawTexture(SDL_Texture* texture, const Rect& dst, std::uint8_t alpha) { Blit(texture, dst, alpha); }

Rect Renderer2D::DrawImageAuto(const std::string& path, DisplayMode mode, std::uint8_t alpha, int offsetX, int offsetY, float scale, Shadow shadow) {
    SDL_Texture* tex = m_assets.Texture(path);
    if (!tex) {
        return {};
    }
    int tw = 0, th = 0;
    AssetCache::TextureSize(tex, tw, th);
    if (tw <= 0 || th <= 0) {
        return {};
    }

    const float rw = static_cast<float>(m_logicalW);
    const float rh = static_cast<float>(m_logicalH);
    float w = tw * scale;
    float h = th * scale;
    float x = 0.0f, y = 0.0f;

    switch (mode) {
        case DisplayMode::TopLeft:
            x = 0, y = 0;
            break;
        case DisplayMode::TopRight:
            x = rw - w, y = 0;
            break;
        case DisplayMode::BottomLeft:
            x = 0, y = rh - h;
            break;
        case DisplayMode::BottomRight:
            x = rw - w, y = rh - h;
            break;
        case DisplayMode::Top:
            x = (rw - w) * 0.5f, y = 0;
            break;
        case DisplayMode::Bottom:
            x = (rw - w) * 0.5f, y = rh - h;
            break;
        case DisplayMode::Left:
            x = 0, y = (rh - h) * 0.5f;
            break;
        case DisplayMode::Right:
            x = rw - w, y = (rh - h) * 0.5f;
            break;
        case DisplayMode::Center:
            x = (rw - w) * 0.5f, y = (rh - h) * 0.5f;
            break;
        case DisplayMode::FitWidthBottom: {
            const float s = (rw / tw) * scale;
            w = tw * s, h = th * s, x = 0, y = rh - h;
            break;
        }
        case DisplayMode::Fit: {
            const float s = std::min(rw / tw, rh / th) * scale;
            w = tw * s, h = th * s, x = (rw - w) * 0.5f, y = (rh - h) * 0.5f;
            break;
        }
        case DisplayMode::Fill: {
            const float s = std::max(rw / tw, rh / th) * scale;
            w = tw * s, h = th * s, x = (rw - w) * 0.5f, y = (rh - h) * 0.5f;
            break;
        }
    }

    x += offsetX;
    y += offsetY;
    const Rect dst{ x, y, w, h };

    if (shadow.enabled) {
        SDL_SetTextureColorMod(tex, 0, 0, 0);
        Blit(tex, Rect{ x + shadow.offsetX, y + shadow.offsetY, w, h }, shadow.alpha);
        SDL_SetTextureColorMod(tex, 255, 255, 255);
    }
    Blit(tex, dst, alpha);
    return dst;
}

const Renderer2D::CachedText* Renderer2D::AcquireText(const std::string& text, const std::string& fontPath, int size, Color color, int outline, int wrap, HorizontalAlignment alignment) {
    const int ss = m_effectiveTextSupersample;
    const std::string key = MakeKey(text, fontPath, size, color, outline, wrap, ss,static_cast<int>(alignment));
    if (auto it = m_textCache.find(key); it != m_textCache.end()) {
        return &it->second;
    }

    // Backlogs and long sessions can otherwise grow the cache without bound.
    if (m_textCache.size() >= kTextCacheLimit) {
        ClearTextCache();
    }

    TTF_Font* font = m_assets.Font(fontPath, size * ss, outline * ss);
    if (!font || text.empty()) {
        return nullptr;
    }

    const SDL_Color col{color.r,color.g,color.b,color.a};
    const auto wrapAlignment=alignment==HorizontalAlignment::Center?TTF_HORIZONTAL_ALIGN_CENTER:alignment==HorizontalAlignment::Right?TTF_HORIZONTAL_ALIGN_RIGHT:TTF_HORIZONTAL_ALIGN_LEFT;
    TTF_SetFontWrapAlignment(font,wrapAlignment);
    SDL_Surface* surface=wrap>0?TTF_RenderText_Blended_Wrapped(font,text.c_str(),text.size(),col,wrap*ss):TTF_RenderText_Blended(font,text.c_str(),text.size(),col);
    TTF_SetFontWrapAlignment(font,TTF_HORIZONTAL_ALIGN_LEFT);
    if(!surface)return nullptr;
    CachedText entry;
    entry.texture=SDL_CreateTextureFromSurface(m_renderer,surface);entry.w=surface->w/ss;entry.h=surface->h/ss;SDL_DestroySurface(surface);if(!entry.texture)return nullptr;SDL_SetTextureBlendMode(entry.texture,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(entry.texture,SDL_SCALEMODE_LINEAR);

    auto [it, _] = m_textCache.emplace(key, entry);
    return &it->second;
}

void Renderer2D::DrawText(const std::string& text, float x, float y, const std::string& fontPath, int size, Color color, std::uint8_t alpha, int wrap) {
    const CachedText* t = AcquireText(text, fontPath, size, color, 0, wrap);
    if (t) {
        Blit(t->texture,{x,y,static_cast<float>(t->w),static_cast<float>(t->h)},alpha);
    }
}

void Renderer2D::DrawTextOutline(const std::string& text, float x, float y, const std::string& fontPath, int size, Color textColor, Color outlineColor, int outlineSize, std::uint8_t alpha, bool shadow, int wrap) {
    if (shadow) {
        const Color black{ 0, 0, 0, 255 };
        const CachedText* s = AcquireText(text, fontPath, size, black, 0, wrap);
        if (s) {
            const auto sa = static_cast<std::uint8_t>(alpha * 0.35f);
            Blit(s->texture,{x+3,y+3,static_cast<float>(s->w),static_cast<float>(s->h)},sa);
        }
    }

    if (outlineSize > 0) {
        const CachedText* o = AcquireText(text, fontPath, size, outlineColor, outlineSize, wrap);
        if (o) {
            Blit(o->texture,{x,y,static_cast<float>(o->w),static_cast<float>(o->h)},alpha);
        }
    }

    const CachedText* f = AcquireText(text, fontPath, size, textColor, 0, wrap);
    if (f) {
        const float ox = static_cast<float>(outlineSize);
        Blit(f->texture,{x+ox,y+ox,static_cast<float>(f->w),static_cast<float>(f->h)},alpha);
    }
}

Vec2 Renderer2D::MeasureText(const std::string& text, const std::string& fontPath, int size, int wrap) {
    TTF_Font* font = m_assets.Font(fontPath, size, 0);
    if (!font) {
        return {};
    }
    int w = 0, h = 0;
    if (wrap > 0) {
        TTF_GetStringSizeWrapped(font, text.c_str(), text.size(), wrap, &w, &h);
    }
    else {
        TTF_GetStringSize(font, text.c_str(), text.size(), &w, &h);
    }
    return Vec2{ static_cast<float>(w), static_cast<float>(h) };
}

void Renderer2D::ClearTextCache() {
    for (auto& [key, entry] : m_textCache) {
        if (entry.texture) SDL_DestroyTexture(entry.texture);
    }
    m_textCache.clear();
}

void Renderer2D::SetTextSupersample(int factor) {
    factor = std::clamp(factor, 1, 4);
    m_textSamplingMode=TextSamplingMode::Fixed;m_fixedTextSupersample=factor;
    if (factor == m_effectiveTextSupersample) {
        return;
    }
    m_effectiveTextSupersample = factor;
    ClearTextCache();
}

void Renderer2D::DrawTextInRect(const std::string& text, const Rect& bounds,
                                const std::string& fontPath, int size, Color color,
                                HorizontalAlignment horizontal, VerticalAlignment vertical,
                                bool wrap, std::uint8_t alpha) {
    const int wrapWidth=wrap?std::max(1,static_cast<int>(std::floor(bounds.w))):0;
    const CachedText* t=AcquireText(text,fontPath,size,color,0,wrapWidth,horizontal);if(!t)return;
    float x=bounds.x,y=bounds.y;
    if(horizontal==HorizontalAlignment::Center)x+=(bounds.w-t->w)*.5f;else if(horizontal==HorizontalAlignment::Right)x+=bounds.w-t->w;
    if(vertical==VerticalAlignment::Center)y+=(bounds.h-t->h)*.5f;else if(vertical==VerticalAlignment::Bottom)y+=bounds.h-t->h;
    PushClip(bounds);Blit(t->texture,{x,y,static_cast<float>(t->w),static_cast<float>(t->h)},alpha);PopClip();
}

void Renderer2D::SetTextSupersampleAuto(){if(m_textSamplingMode==TextSamplingMode::Auto)return;m_textSamplingMode=TextSamplingMode::Auto;SetLogicalSize(m_logicalW,m_logicalH);}

}  // namespace px::graphics
