#include <thread>
#include <mutex>
#include <chrono>
#include <set>
#include <map>
#include <condition_variable>
#include <gtest/gtest.h>
#include "cppkafka/producer.h"
#include "cppkafka/consumer.h"
#include "cppkafka/utils/compacted_topic_processor.h"

using std::string;
using std::to_string;
using std::stoi;
using std::set;
using std::tie;
using std::vector;
using std::map;
using std::move;
using std::thread;
using std::mutex;
using std::unique_lock;
using std::lock_guard;
using std::condition_variable;

using std::chrono::system_clock;
using std::chrono::seconds;
using std::chrono::milliseconds;

using namespace cppkafka;

class CompactedTopicProcessorTest : public testing::Test {
public:
    static const string KAFKA_TOPIC;

    Configuration make_producer_config() {
        Configuration config;
        config.set("metadata.broker.list", KAFKA_TEST_INSTANCE);
        return config;
    }

    Configuration make_consumer_config() {
        Configuration config;
        config.set("metadata.broker.list", KAFKA_TEST_INSTANCE);
        config.set("enable.auto.commit", false);
        config.set("group.id", "compacted_topic_test");
        return config;
    }
};

const string CompactedTopicProcessorTest::KAFKA_TOPIC = "cppkafka_test1";

TEST_F(CompactedTopicProcessorTest, Consume) {
    Consumer consumer(make_consumer_config());
    // We'll use ints as the key, strings as the value
    using CompactedConsumer = CompactedTopicProcessor<int, string>;
    using Event = CompactedConsumer::Event;
    CompactedConsumer compacted_consumer(consumer);
    // Convert the buffer to an int for the key
    compacted_consumer.set_key_decoder([](const Buffer& buffer) {
        return stoi(buffer);
    });
    // We won't use any formats on the value, just convert it to a string 
    compacted_consumer.set_value_decoder([](int /*key*/, const Buffer& buffer) {
        return string(buffer);
    });

    // Every time there's an event, we'll push it into a vector
    vector<Event> events;
    compacted_consumer.set_event_handler([&](const Event& event) {
        events.push_back(event);
    });
    consumer.subscribe({ KAFKA_TOPIC });
    consumer.poll();
    consumer.poll();
    consumer.poll();

    Producer producer(make_producer_config());

    struct ElementType {
        string value;
        int partition;
    };
    map<string, ElementType> elements = {
        {"42", {"hi there", 0}},
        {"1337", {"heh", 1}}
    };
    for (const auto& element_pair : elements) { 
        const ElementType& element = element_pair.second;
        MessageBuilder builder(KAFKA_TOPIC);
        builder.partition(element.partition).key(element_pair.first).payload(element.value);
        producer.produce(builder);
    }
    // Now erase the first element
    string deleted_key = "42";
    producer.produce(MessageBuilder(KAFKA_TOPIC).partition(0).key(deleted_key));

    for (size_t i = 0; i < 10; ++i) {
        compacted_consumer.process_event();
    }

    size_t set_count = 0;
    size_t delete_count = 0;
    ASSERT_FALSE(events.empty());
    for (const Event& event : events) {
        switch (event.get_type()) {
            case Event::SET_ELEMENT:
                {
                    auto iter = elements.find(to_string(event.get_key()));
                    ASSERT_NE(iter, elements.end());
                    EXPECT_EQ(iter->second.value, event.get_value());
                    EXPECT_EQ(iter->second.partition, event.get_partition());
                    set_count++;
                }
                break;
            case Event::DELETE_ELEMENT:
                EXPECT_EQ(0, event.get_partition());
                EXPECT_EQ(42, event.get_key());
                delete_count++;
                break;
            default:
            break;
        } 
    }
    EXPECT_EQ(2, set_count);
    EXPECT_EQ(1, delete_count);
}
