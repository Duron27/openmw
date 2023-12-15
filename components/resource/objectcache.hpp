// Resource ObjectCache for OpenMW, forked from osgDB ObjectCache by Robert Osfield, see copyright notice below.
// Changes:
// - removeExpiredObjectsInCache no longer keeps a lock while the unref happens.
// - template allows customized KeyType.
// - objects with uninitialized time stamp are not removed.

/* -*-c++-*- OpenSceneGraph - Copyright (C) 1998-2006 Robert Osfield
 *
 * This library is open source and may be redistributed and/or modified under
 * the terms of the OpenSceneGraph Public License (OSGPL) version 0.0 or
 * (at your option) any later version.  The full license is in LICENSE file
 * included with this distribution, and on the openscenegraph.org website.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * OpenSceneGraph Public License for more details.
 */

#ifndef OPENMW_COMPONENTS_RESOURCE_OBJECTCACHE
#define OPENMW_COMPONENTS_RESOURCE_OBJECTCACHE

#include <osg/Node>
#include <osg/Referenced>
#include <osg/ref_ptr>

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace osg
{
    class Object;
    class State;
    class NodeVisitor;
}

namespace Resource
{

    template <typename KeyType>
    class GenericObjectCache : public osg::Referenced
    {
    public:
        GenericObjectCache()
            : osg::Referenced(true)
        {
        }

        // Update last usage timestamp using referenceTime for each cache time if they are not nullptr and referenced
        // from somewhere else. Remove items with last usage > expiryTime. Note: last usage might be updated from other
        // places so nullptr or not references elsewhere items are not always removed.
        void update(double referenceTime, double expiryDelay)
        {
            std::vector<osg::ref_ptr<osg::Object>> objectsToRemove;
            {
                const double expiryTime = referenceTime - expiryDelay;
                std::lock_guard<std::mutex> lock(_objectCacheMutex);
                std::erase_if(_objectCache, [&](auto& v) {
                    Item& item = v.second;
                    if ((item.mValue != nullptr && item.mValue->referenceCount() > 1) || item.mLastUsage == 0)
                        item.mLastUsage = referenceTime;
                    if (item.mLastUsage > expiryTime)
                        return false;
                    if (item.mValue != nullptr)
                        objectsToRemove.push_back(std::move(item.mValue));
                    return true;
                });
            }
            // note, actual unref happens outside of the lock
            objectsToRemove.clear();
        }

        /** Remove all objects in the cache regardless of having external references or expiry times.*/
        void clear()
        {
            std::lock_guard<std::mutex> lock(_objectCacheMutex);
            _objectCache.clear();
        }

        /** Add a key,object,timestamp triple to the Registry::ObjectCache.*/
        void addEntryToObjectCache(const KeyType& key, osg::Object* object, double timestamp = 0.0)
        {
            std::lock_guard<std::mutex> lock(_objectCacheMutex);
            _objectCache[key] = Item{ object, timestamp };
        }

        /** Remove Object from cache.*/
        void removeFromObjectCache(const KeyType& key)
        {
            std::lock_guard<std::mutex> lock(_objectCacheMutex);
            typename ObjectCacheMap::iterator itr = _objectCache.find(key);
            if (itr != _objectCache.end())
                _objectCache.erase(itr);
        }

        /** Get an ref_ptr<Object> from the object cache*/
        osg::ref_ptr<osg::Object> getRefFromObjectCache(const KeyType& key)
        {
            std::lock_guard<std::mutex> lock(_objectCacheMutex);
            typename ObjectCacheMap::iterator itr = _objectCache.find(key);
            if (itr != _objectCache.end())
                return itr->second.mValue;
            else
                return nullptr;
        }

        std::optional<osg::ref_ptr<osg::Object>> getRefFromObjectCacheOrNone(const KeyType& key)
        {
            const std::lock_guard<std::mutex> lock(_objectCacheMutex);
            const auto it = _objectCache.find(key);
            if (it == _objectCache.end())
                return std::nullopt;
            return it->second.mValue;
        }

        /** Check if an object is in the cache, and if it is, update its usage time stamp. */
        bool checkInObjectCache(const KeyType& key, double timeStamp)
        {
            std::lock_guard<std::mutex> lock(_objectCacheMutex);
            typename ObjectCacheMap::iterator itr = _objectCache.find(key);
            if (itr != _objectCache.end())
            {
                itr->second.mLastUsage = timeStamp;
                return true;
            }
            else
                return false;
        }

        /** call releaseGLObjects on all objects attached to the object cache.*/
        void releaseGLObjects(osg::State* state)
        {
            std::lock_guard<std::mutex> lock(_objectCacheMutex);
            for (typename ObjectCacheMap::iterator itr = _objectCache.begin(); itr != _objectCache.end(); ++itr)
            {
                osg::Object* object = itr->second.mValue.get();
                object->releaseGLObjects(state);
            }
        }

        /** call node->accept(nv); for all nodes in the objectCache. */
        void accept(osg::NodeVisitor& nv)
        {
            std::lock_guard<std::mutex> lock(_objectCacheMutex);
            for (typename ObjectCacheMap::iterator itr = _objectCache.begin(); itr != _objectCache.end(); ++itr)
            {
                if (osg::Object* object = itr->second.mValue.get())
                {
                    osg::Node* node = dynamic_cast<osg::Node*>(object);
                    if (node)
                        node->accept(nv);
                }
            }
        }

        /** call operator()(KeyType, osg::Object*) for each object in the cache. */
        template <class Functor>
        void call(Functor&& f)
        {
            std::lock_guard<std::mutex> lock(_objectCacheMutex);
            for (typename ObjectCacheMap::iterator it = _objectCache.begin(); it != _objectCache.end(); ++it)
                f(it->first, it->second.mValue.get());
        }

        /** Get the number of objects in the cache. */
        unsigned int getCacheSize() const
        {
            std::lock_guard<std::mutex> lock(_objectCacheMutex);
            return _objectCache.size();
        }

        template <class K>
        std::optional<std::pair<KeyType, osg::ref_ptr<osg::Object>>> lowerBound(K&& key)
        {
            const std::lock_guard<std::mutex> lock(_objectCacheMutex);
            const auto it = _objectCache.lower_bound(std::forward<K>(key));
            if (it == _objectCache.end())
                return std::nullopt;
            return std::pair(it->first, it->second.mValue);
        }

    protected:
        struct Item
        {
            osg::ref_ptr<osg::Object> mValue;
            double mLastUsage;
        };

        virtual ~GenericObjectCache() {}

        using ObjectCacheMap = std::map<KeyType, Item, std::less<>>;

        ObjectCacheMap _objectCache;
        mutable std::mutex _objectCacheMutex;
    };

    class ObjectCache : public GenericObjectCache<std::string>
    {
    };

}

#endif
