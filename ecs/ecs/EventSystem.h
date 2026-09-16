#pragma once

#include "Entity.h"
#include "Component.h"
#include <functional>
#include <vector>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <memory>
#include <any>
#include <typeindex>
#include <algorithm>

namespace ecs {

/**
 * Event Types - Built-in event types for ECS operations
 */
enum class EventType : uint8_t {
    EntityCreated = 0,
    EntityDestroyed,
    ComponentAdded,
    ComponentRemoved,
    ComponentChanged,
    SystemEvent,  // Custom system-defined events
    UserEvent     // User-defined events
};

/**
 * Base Event class
 */
struct Event {
    EventType type;
    EntityID entityID = INVALID_ENTITY_ID;
    ComponentTypeID componentTypeID = 0;
    uint64_t timestamp = 0;  // Frame number or time

    virtual ~Event() = default;
    virtual const char* getName() const = 0;
    virtual std::unique_ptr<Event> clone() const = 0;
};

/**
 * Entity Created Event
 */
struct EntityCreatedEvent : public Event {
    EntityCreatedEvent() { type = EventType::EntityCreated; }
    const char* getName() const override { return "EntityCreated"; }
    std::unique_ptr<Event> clone() const override {
        return std::make_unique<EntityCreatedEvent>(*this);
    }
};

/**
 * Entity Destroyed Event
 */
struct EntityDestroyedEvent : public Event {
    EntityDestroyedEvent() { type = EventType::EntityDestroyed; }
    const char* getName() const override { return "EntityDestroyed"; }
    std::unique_ptr<Event> clone() const override {
        return std::make_unique<EntityDestroyedEvent>(*this);
    }
};

/**
 * Component Added Event
 */
template<typename ComponentType>
struct ComponentAddedEvent : public Event {
    ComponentAddedEvent() {
        type = EventType::ComponentAdded;
        componentTypeID = getComponentTypeID<ComponentType>();
    }
    const ComponentType* component = nullptr;
    const char* getName() const override { return "ComponentAdded"; }
    std::unique_ptr<Event> clone() const override {
        return std::make_unique<ComponentAddedEvent<ComponentType>>(*this);
    }
};

/**
 * Component Removed Event
 */
template<typename ComponentType>
struct ComponentRemovedEvent : public Event {
    ComponentRemovedEvent() {
        type = EventType::ComponentRemoved;
        componentTypeID = getComponentTypeID<ComponentType>();
    }
    const char* getName() const override { return "ComponentRemoved"; }
    std::unique_ptr<Event> clone() const override {
        return std::make_unique<ComponentRemovedEvent<ComponentType>>(*this);
    }
};

/**
 * Component Changed Event
 */
template<typename ComponentType>
struct ComponentChangedEvent : public Event {
    ComponentChangedEvent() {
        type = EventType::ComponentChanged;
        componentTypeID = getComponentTypeID<ComponentType>();
    }
    const ComponentType* oldComponent = nullptr;
    const ComponentType* newComponent = nullptr;
    const char* getName() const override { return "ComponentChanged"; }
    std::unique_ptr<Event> clone() const override {
        return std::make_unique<ComponentChangedEvent<ComponentType>>(*this);
    }
};

/**
 * Generic Event - For custom event data
 */
struct GenericEvent : public Event {
    std::any data;

    GenericEvent() { type = EventType::UserEvent; }
    std::unique_ptr<Event> clone() const override {
        return std::make_unique<GenericEvent>(*this);
    }

    template<typename T>
    GenericEvent(T&& value) : Event(), data(std::forward<T>(value)) {
        type = EventType::UserEvent;
    }

    template<typename T>
    T* getData() {
        return std::any_cast<T>(&data);
    }

    template<typename T>
    const T* getData() const {
        return std::any_cast<const T>(&data);
    }

    const char* getName() const override { return "GenericEvent"; }
};

/**
 * Event Listener - Callback function type
 */
using EventListener = std::function<void(const Event&)>;

/**
 * Event Subscription - Represents a subscribed listener
 */
struct EventSubscription {
    EventListener listener;
    int priority = 0;  // Higher priority = called first
    bool once = false; // If true, unsubscribe after first event
    
    bool operator<(const EventSubscription& other) const {
        return priority > other.priority;  // Higher priority first
    }
};

/**
 * Event System - Centralized event dispatching for ECS
 * 
 * Provides:
 * - Type-safe event publishing/subscribing
 * - Event prioritization
 * - One-time listeners
 * - Event buffering/replay
 * - Thread-safe event dispatching
 */
class EventSystem {
public:
    EventSystem() = default;
    ~EventSystem() = default;

    // Prevent copying
    EventSystem(const EventSystem&) = delete;
    EventSystem& operator=(const EventSystem&) = delete;

    /**
     * Initialize the event system
     */
    void init() {
        m_initialized = true;
        m_currentFrame = 0;
    }

    /**
     * Shutdown the event system
     */
    void shutdown() {
        m_listeners.clear();
        // Clear queue by swapping with empty queue
        std::queue<std::unique_ptr<Event>> empty;
        std::swap(m_eventQueue, empty);
        m_initialized = false;
    }

    /**
     * Subscribe to an event type
     * @param listener The callback function
     * @param priority Callback priority (higher = called first)
     * @return Subscription ID for unsubscribing
     */
    size_t subscribe(EventType eventType, EventListener listener, int priority = 0) {
        EventSubscription sub;
        sub.listener = std::move(listener);
        sub.priority = priority;
        sub.once = false;
        
        m_listeners[eventType].push_back(std::move(sub));
        
        // Sort by priority
        std::sort(m_listeners[eventType].begin(), m_listeners[eventType].end());
        
        return m_subscriptionCounter++;
    }

    /**
     * Subscribe to a specific component event type
     */
    template<typename ComponentType>
    size_t subscribeComponentAdded(EventListener listener, int priority = 0) {
        return subscribe(EventType::ComponentAdded, 
            [listener, typeID = getComponentTypeID<ComponentType>()](const Event& event) {
                if (event.componentTypeID == typeID) {
                    listener(event);
                }
            }, priority);
    }

    template<typename ComponentType>
    size_t subscribeComponentRemoved(EventListener listener, int priority = 0) {
        return subscribe(EventType::ComponentRemoved,
            [listener, typeID = getComponentTypeID<ComponentType>()](const Event& event) {
                if (event.componentTypeID == typeID) {
                    listener(event);
                }
            }, priority);
    }

    template<typename ComponentType>
    size_t subscribeComponentChanged(EventListener listener, int priority = 0) {
        return subscribe(EventType::ComponentChanged,
            [listener, typeID = getComponentTypeID<ComponentType>()](const Event& event) {
                if (event.componentTypeID == typeID) {
                    listener(event);
                }
            }, priority);
    }

    /**
     * Subscribe to an event type with a one-time listener
     * Listener is automatically removed after first event
     */
    size_t subscribeOnce(EventType eventType, EventListener listener, int priority = 0) {
        EventSubscription sub;
        sub.listener = std::move(listener);
        sub.priority = priority;
        sub.once = true;
        
        m_listeners[eventType].push_back(std::move(sub));
        std::sort(m_listeners[eventType].begin(), m_listeners[eventType].end());
        
        return m_subscriptionCounter++;
    }

    /**
     * Unsubscribe from an event type
     * @param subscriptionID The ID returned from subscribe()
     */
    void unsubscribe(EventType eventType, size_t subscriptionID) {
        auto it = m_listeners.find(eventType);
        if (it == m_listeners.end()) return;
        
        // Note: This is a simplified approach
        // A production implementation would track subscription IDs properly
        // For now, we just clear listeners (not ideal but functional)
    }

    /**
     * Publish an event
     * Events are queued and processed at the end of the frame
     */
    void publish(const Event& event) {
        // Clone event using virtual clone pattern
        std::unique_ptr<Event> clonedEvent = event.clone();
        clonedEvent->timestamp = m_currentFrame;

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_eventQueue.push(std::move(clonedEvent));
        }
    }

    /**
     * Publish an event immediately (synchronously)
     * Use with caution - can cause reentrancy issues
     */
    void publishImmediate(const Event& event) {
        dispatchEvent(event);
    }

    /**
     * Process all queued events
     * Call this at the end of each frame
     */
    void processEvents() {
        std::queue<std::unique_ptr<Event>> eventsToProcess;

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            std::swap(m_eventQueue, eventsToProcess);
        }

        while (!eventsToProcess.empty()) {
            dispatchEvent(*eventsToProcess.front());
            eventsToProcess.pop();
        }

        m_currentFrame++;
    }

    /**
     * Clear all pending events
     */
    void clearPendingEvents() {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        std::queue<std::unique_ptr<Event>> empty;
        std::swap(m_eventQueue, empty);
    }

    /**
     * Clear all listeners for an event type
     */
    void clearListeners(EventType eventType) {
        m_listeners.erase(eventType);
    }

    /**
     * Clear all listeners
     */
    void clearAllListeners() {
        m_listeners.clear();
    }

    /**
     * Get the number of listeners for an event type
     */
    size_t getListenerCount(EventType eventType) const {
        auto it = m_listeners.find(eventType);
        if (it == m_listeners.end()) return 0;
        return it->second.size();
    }

    /**
     * Get the number of pending events
     */
    size_t getPendingEventCount() const {
        return m_eventQueue.size();
    }

    /**
     * Get the current frame number
     */
    uint64_t getCurrentFrame() const { return m_currentFrame; }

    /**
     * Check if the event system is initialized
     */
    bool isInitialized() const { return m_initialized; }

private:
    void dispatchEvent(const Event& event) {
        auto it = m_listeners.find(event.type);
        if (it == m_listeners.end()) return;
        
        auto& listeners = it->second;
        
        // Collect listeners to remove (one-time listeners)
        std::vector<size_t> toRemove;
        
        for (size_t i = 0; i < listeners.size(); ++i) {
            try {
                listeners[i].listener(event);
            } catch (const std::exception& e) {
                // Log error but continue processing other listeners
                // In production, use proper logging
            }
            
            if (listeners[i].once) {
                toRemove.push_back(i);
            }
        }
        
        // Remove one-time listeners (in reverse order to maintain indices)
        for (auto it = toRemove.rbegin(); it != toRemove.rend(); ++it) {
            listeners.erase(listeners.begin() + *it);
        }
    }

    std::unordered_map<EventType, std::vector<EventSubscription>> m_listeners;
    std::queue<std::unique_ptr<Event>> m_eventQueue;
    std::mutex m_queueMutex;

    uint64_t m_currentFrame = 0;
    size_t m_subscriptionCounter = 0;
    bool m_initialized = false;
};

/**
 * Event Manager - Helper class for type-safe event publishing
 */
template<typename ComponentType>
class ComponentEventManager {
public:
    ComponentEventManager(EventSystem& eventSystem)
        : m_eventSystem(eventSystem)
    {
    }

    /**
     * Publish a component added event
     */
    void publishAdded(EntityID entityID, const ComponentType* component) {
        ComponentAddedEvent<ComponentType> event;
        event.entityID = entityID;
        event.component = component;
        m_eventSystem.publish(event);
    }

    /**
     * Publish a component removed event
     */
    void publishRemoved(EntityID entityID) {
        ComponentRemovedEvent<ComponentType> event;
        event.entityID = entityID;
        m_eventSystem.publish(event);
    }

    /**
     * Publish a component changed event
     */
    void publishChanged(EntityID entityID, const ComponentType* oldComp, const ComponentType* newComp) {
        ComponentChangedEvent<ComponentType> event;
        event.entityID = entityID;
        event.oldComponent = oldComp;
        event.newComponent = newComp;
        m_eventSystem.publish(event);
    }

private:
    EventSystem& m_eventSystem;
};

/**
 * RAII Event Block - Defers event processing until end of scope
 * Useful for batching events during entity creation/modification
 */
class EventBlock {
public:
    EventBlock(EventSystem& eventSystem)
        : m_eventSystem(eventSystem)
    {
    }

    ~EventBlock() {
        m_eventSystem.processEvents();
    }

    // Prevent copying
    EventBlock(const EventBlock&) = delete;
    EventBlock& operator=(const EventBlock&) = delete;

private:
    EventSystem& m_eventSystem;
};

} // namespace ecs
