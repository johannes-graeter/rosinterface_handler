#pragma once
#include <cstdlib>
#include <mutex>
#include <thread>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <ros/callback_queue.h>
#include <ros/publication.h>
#include <ros/publisher.h>
#include <ros/topic_manager.h>

namespace rosinterface_handler {
namespace detail {
template <typename T>
struct Dereference {
    static inline constexpr decltype(auto) get(const T& t) {
        return t;
    }
};

template <typename T>
struct Dereference<T*> {
    static inline constexpr decltype(auto) get(const T*& t) {
        return *t;
    }
};

template <typename T>
struct Dereference<std::shared_ptr<T>> {
    static constexpr decltype(auto) get(const std::shared_ptr<T>& t) {
        return *t;
    }
};
} // namespace detail
/**
 * @brief Subscriber that only actually subscribes to a topic if someone subscribes to a publisher
 * This is useful to avoid overhead for computing results that no one actually cares for.
 * Because this subscriber internally unsubscribes from a topic, upstream nodes are able to stop
 * publishing useless results as well.
 *
 * The smart subscriber can also be used for synchronized subscription via message_filters::TimeSynchronizer or similar.
 *
 * Set the environment variable NO_SMART_SUBSCRIBE to 1 to disable smart subscriptions.
 *
 * Usage example:
 * @code
 * void messageCallback(const std_msgs::Header::ConstPtr& msg) {
 * // do the work
 * }
 *
 * // subscribe in your main() or nodelet
 * ros::NodeHandle nh;
 * ros::Publisher myPub = nh.advertise<std_msgs::Header>("/output_topic", 5);
 * utils_ros::SmartSubscriber<std_msgs::Header> subscriber(myPub);
 * subscriber.subscribe(nh, "/header_topic", 5);
 * subscriber.addCallback(messageCallback);
 * @endcode
 */
template <class Message>
class SmartSubscriber : public message_filters::Subscriber<Message> {
public:
    using Publishers = std::vector<ros::Publisher>;

    template <typename... PublishersT>
    explicit SmartSubscriber(const PublishersT&... trackedPublishers) {
        // check for always-on-mode
        const auto smart_subscribe = std::getenv("NO_SMART_SUBSCRIBE");
        try {
            if (smart_subscribe && std::stoi(smart_subscribe) > 0) {
                setSmart(false);
            }
        } catch (const std::invalid_argument&) {
        }
        ros::SubscriberStatusCallback cb = boost::bind(&SmartSubscriber::subscribeCallback, this);
        callback_ =
            boost::make_shared<ros::SubscriberCallbacks>(cb, cb, ros::VoidConstPtr(), ros::getGlobalCallbackQueue());

        publisherInfo_.reserve(sizeof...(trackedPublishers));
        using Workaround = int[];
        Workaround{(addPublisher(trackedPublishers), 0)...};
    }

    ~SmartSubscriber() {
        // void the callback
        std::lock_guard<std::mutex> m(callbackLock_);
        for (auto& pub : publisherInfo_) {
            removeCallback(pub.topic);
        }
        callback_->disconnect_ = +[](const ros::SingleSubscriberPublisher&) {};
        callback_->connect_ = +[](const ros::SingleSubscriberPublisher&) {};
    }

    /**
     * @brief Subscribe to a topic.
     *
     * Calls the message_filtes::Subscriber's subscribe internally.
     *
     * @param nh The ros::NodeHandle to use to subscribe.
     * @param topic The topic to subscribe to.
     * @param queue_size The subscription queue size
     * @param transport_hints The transport hints to pass along
     * @param callback_queue The callback queue to pass along
     */
    void subscribe(ros::NodeHandle& nh, const std::string& topic, uint32_t queue_size,
                   const ros::TransportHints& transport_hints = ros::TransportHints(),
                   ros::CallbackQueueInterface* callback_queue = nullptr) override {
        message_filters::Subscriber<Message>::subscribe(nh, topic, queue_size, transport_hints, callback_queue);
        subscribeCallback();
    }

    using message_filters::Subscriber<Message>::subscribe;

    /**
     * @brief Adds a new publisher to monitor
     * @param publisher to look after
     * Requires that the publisher has "getTopic" and a "getNumSubscribers" function.
     * The SmartSubscriber does *not* manage the publisher and keeps a reference to it. If it goes out of scope, there
     * will be trouble.
     */
    template <typename Publisher>
    void addPublisher(const Publisher& publisher) {
        publisherInfo_.push_back({[&]() { return detail::Dereference<Publisher>::get(publisher).getTopic(); },
                                  [&]() { return detail::Dereference<Publisher>::get(publisher).getNumSubscribers(); },
                                  detail::Dereference<Publisher>::get(publisher).getTopic()});
        addCallback(publisherInfo_.back().topic);

        // check for subscribe
        subscribeCallback();
    }

    /**
     * @brief stops tracking a publisher.
     * Does nothing if the publisher does not exist.
     * @return true if publisher existed and was removed
     */
    bool removePublisher(const std::string& topic) {
        // remove from vector
        auto found = std::find_if(publisherInfo_.begin(), publisherInfo_.end(),
                                  [&](const auto& pubInfo) { return topic == pubInfo.getTopic(); });
        if (found == publisherInfo_.end()) {
            return false;
        }
        publisherInfo_.erase(found);
        removeCallback(topic);
        return true;
    }

    /**
     * @brief updates the topics of the tracked subscribers
     * This can be necessary if these have changed through a reconfigure request
     */
    void updateTopics() {
        for (auto& publisher : publisherInfo_) {
            const auto currTopic = publisher.getTopic();
            if (currTopic != publisher.topic) {
                addCallback(currTopic);
                removeCallback(publisher.topic);
                publisher.topic = currTopic;
            }
        }
    }

    /**
     * @brief returns whether this subsciber is currently subscribed to something
     * @return true if subscribed
     */
    bool isSubscribed() const {
        return bool(this->getSubscriber());
    }

    /**
     * @brief returns whether this subscriber is currently in smart mode
     * @return true if in smart mode
     * If the subscriber is not in smart mode, it will behave like a normal ros publisher and will always be subscribed
     */
    bool smart() const {
        return smart_;
    }

    /**
     * @brief enable/disable smart mode
     * @param smart new mode for subscriber
     */
    void setSmart(bool smart) {
        smart_ = smart;
        subscribeCallback();
    }

    /**
     * @brief pass this callback to all non-standard publisher that you have
     * @return subscriber callback of this SmartSubscriber
     */
    const ros::SubscriberCallbacksPtr callback() const {
        return callback_;
    }

    /**
     * @brief checks for new subscribers and subscribes or unsubscribes if anything changed.
     * This function is not supposed to be called actively, it is only here so that you can pass it as callback to any
     * special publisher
     * (like image transport)
     */
    void subscribeCallback() {
        std::lock_guard<std::mutex> m(callbackLock_);
        const auto subscribed = isSubscribed();
        bool subscribe = !smart() || std::any_of(publisherInfo_.begin(), publisherInfo_.end(),
                                                 [](auto& p) { return p.getNumSubscriber() > 0; });

        if (subscribe && !subscribed) {
            ROS_DEBUG_STREAM("Got new subscribers. Subscribing to " << this->getSubscriber().getTopic());
            this->subscribe();
        }
        if (!subscribe && subscribed) {
            ROS_DEBUG_STREAM("No subscribers found. Unsubscribing from " << this->getSubscriber().getTopic());
            this->unsubscribe();
        }
    }

private:
    void addCallback(const std::string& topic) {
        auto pub = ros::TopicManager::instance()->lookupPublication(publisherInfo_.back().topic);
        if (!!pub) {
            pub->addCallbacks(callback_);
        } else {
        }
    }

    void removeCallback(const std::string& topic) {
        auto pub = ros::TopicManager::instance()->lookupPublication(publisherInfo_.back().topic);
        if (!!pub) {
            pub->removeCallbacks(callback_);
        }
    }

    struct PublisherInfo {
        std::function<std::string()> getTopic;
        std::function<uint32_t()> getNumSubscriber;
        std::string topic;
    };
    std::vector<PublisherInfo> publisherInfo_;
    ros::SubscriberCallbacksPtr callback_;
    std::mutex callbackLock_{};
    bool smart_{true};
};

template <class Message>
using SmartSubscriberPtr = std::shared_ptr<SmartSubscriber<Message>>;
} // namespace rosinterface_handler
