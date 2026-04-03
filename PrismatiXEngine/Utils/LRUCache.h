#pragma once

#include <list>
#include <stdexcept>
#include <unordered_map>
#include <functional>

template <typename Key, typename Value>
class LRUCache {
public:
    typedef typename std::pair<Key, Value> KeyValuePair;
    typedef typename std::list<KeyValuePair>::iterator Iterator;

    LRUCache(size_t maxSize, std::function<void(Value)> onEvict = nullptr) : maxSize(maxSize), onEvict(onEvict) {}

    ~LRUCache() {
        if (onEvict) {
            for (auto& pair : cacheList) {
                onEvict(pair.second);
            }
        }
    }

    void Put(const Key& key, Value value) {
        auto it = cacheMap.find(key);
        if (it != cacheMap.end()) {
            cacheList.erase(it->second);
            cacheMap.erase(it);
        }

        cacheList.push_front(KeyValuePair(key, value));
        cacheMap[key] = cacheList.begin();

        if (cacheMap.size() > maxSize) {
            auto last = cacheList.end();
            last--;
            if (onEvict) {
                onEvict(last->second);
            }
            cacheMap.erase(last->first);
            cacheList.pop_back();
        }
    }

    bool Get(const Key& key, Value& value) {
        auto it = cacheMap.find(key);
        if (it == cacheMap.end()) {
            return false;
        }

        // Move to front
        cacheList.splice(cacheList.begin(), cacheList, it->second);
        value = it->second->second;
        return true;
    }

    bool Exists(const Key& key) const { return cacheMap.find(key) != cacheMap.end(); }

    void Remove(const Key& key) {
        auto it = cacheMap.find(key);
        if (it != cacheMap.end()) {
            if (onEvict) {
                onEvict(it->second->second);
            }
            cacheList.erase(it->second);
            cacheMap.erase(it);
        }
    }

    void Clear() {
        if (onEvict) {
            for (auto& pair : cacheList) {
                onEvict(pair.second);
            }
        }
        cacheMap.clear();
        cacheList.clear();
    }

private:
    size_t maxSize;
    std::list<KeyValuePair> cacheList;
    std::unordered_map<Key, Iterator> cacheMap;
    std::function<void(Value)> onEvict;
};
