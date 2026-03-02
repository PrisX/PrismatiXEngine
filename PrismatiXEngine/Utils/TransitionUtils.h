#pragma once

#include <SDL2/SDL.h>
#include <algorithm>
#include <functional>

namespace TransitionUtils {
	enum class OverlayEvent {
		None,
		ReachedPeak,
		ReachedClear
	};

	enum class TransitionType {
		BlackFade,
		WhiteFlash
	};

	inline float StepTowards(float current, float target, float step) {
		if (step <= 0.0f) return current;
		if (current < target) return std::min(current + step, target);
		if (current > target) return std::max(current - step, target);
		return current;
	}

	inline bool MoveTowards(float& current, float target, float step) {
		current = StepTowards(current, target, step);
		return current == target;
	}

	inline bool FadeIn(float& alpha, float step, float maxAlpha = 255.0f) {
		return MoveTowards(alpha, maxAlpha, step);
	}

	inline bool FadeOut(float& alpha, float step) {
		return MoveTowards(alpha, 0.0f, step);
	}

	inline OverlayEvent UpdateOverlay(float& alpha, bool& isEntering, bool& isLeaving, float step, float maxAlpha = 255.0f) {
		if (isEntering) {
			if (FadeIn(alpha, step, maxAlpha)) {
				isEntering = false;
				isLeaving = true;
				return OverlayEvent::ReachedPeak;
			}
		}
		else if (isLeaving) {
			if (FadeOut(alpha, step)) {
				isLeaving = false;
				return OverlayEvent::ReachedClear;
			}
		}

		return OverlayEvent::None;
	}

	inline void DrawOverlay(SDL_Renderer* renderer, TransitionType type, Uint8 alpha, int width, int height) {
		if (!renderer || alpha == 0) return;
		SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
		if (type == TransitionType::WhiteFlash) {
			SDL_SetRenderDrawColor(renderer, 255, 255, 255, alpha);
		}
		else {
			SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
		}
		SDL_Rect fullscreen = { 0, 0, width, height };
		SDL_RenderFillRect(renderer, &fullscreen);
	}

	struct Transition {
		float alpha = 0.0f;
		bool isEntering = false;
		bool isLeaving = false;
		float speed = 8.0f;
		float maxAlpha = 255.0f;
		TransitionType type = TransitionType::BlackFade;
		std::function<void()> onPeakAction;

		bool IsActive() const {
			return isEntering || isLeaving;
		}

		bool Start(TransitionType transitionType = TransitionType::BlackFade, float peakAlpha = 255.0f) {
			if (IsActive()) return false;
			type = transitionType;
			maxAlpha = std::clamp(peakAlpha, 0.0f, 255.0f);
			isEntering = true;
			isLeaving = false;
			alpha = 0.0f;
			onPeakAction = nullptr;
			return true;
		}

		bool Start(std::function<void()> action, TransitionType transitionType = TransitionType::BlackFade, float peakAlpha = 255.0f) {
			if (!Start(transitionType, peakAlpha)) return false;
			onPeakAction = std::move(action);
			return true;
		}

		OverlayEvent Update() {
			OverlayEvent event = UpdateOverlay(alpha, isEntering, isLeaving, speed, maxAlpha);
			if (event == OverlayEvent::ReachedPeak && onPeakAction) {
				onPeakAction();
				onPeakAction = nullptr;
			}
			return event;
		}

		void Draw(SDL_Renderer* renderer, int width, int height) const {
			DrawOverlay(renderer, type, static_cast<Uint8>(alpha), width, height);
		}
	};

	inline Uint8 AlphaFromElapsed(Uint32 elapsed, Uint32 duration) {
		if (duration == 0) return 255;
		if (elapsed >= duration) return 255;
		return static_cast<Uint8>((elapsed * 255) / duration);
	}
}
