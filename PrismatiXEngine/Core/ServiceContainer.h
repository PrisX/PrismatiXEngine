#pragma once
#include <any>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <typeindex>

namespace PrismatiX {
namespace App {

enum class ServiceLifetime { Singleton, Transient };

class ServiceContainer {
    std::map<std::type_index, std::any> singletons;
    std::map<std::type_index, std::function<std::any()>> factories;

public:
    template <typename T>
    void RegisterSingleton(T* instance) {
        singletons[typeid(T)] = instance;
    }

    template <typename T>
    void RegisterFactory(std::function<T*()> factory) {
        factories[typeid(T)] = [factory]() -> std::any { return factory(); };
    }

    template <typename T>
    T* Resolve() {
        auto it = singletons.find(typeid(T));
        if (it != singletons.end()) {
            return std::any_cast<T*>(it->second);
        }

        auto fit = factories.find(typeid(T));
        if (fit != factories.end()) {
            return std::any_cast<T*>(fit->second());
        }

        return nullptr;
    }

    template <typename T>
    bool IsRegistered() const {
        return singletons.count(typeid(T)) > 0 || factories.count(typeid(T)) > 0;
    }

    void Clear() {
        singletons.clear();
        factories.clear();
    }
};

}  // namespace App
}  // namespace PrismatiX
