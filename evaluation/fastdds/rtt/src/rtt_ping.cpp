// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "rttPubSubTypes.hpp"

#include <chrono>
#include <fstream>
#include <map>
#include <strstream>
#include <thread>
#include <unistd.h>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

using namespace eprosima::fastdds::dds;

static std::ofstream csv_output;
static std::mutex logging_lock;
static std::mutex timestamp_map_lock;

static const uint32_t default_interval_ms = 500;
static const uint32_t default_count = 10;

class PingNode
{
private:
    using clock_t = std::chrono::steady_clock;
    using timestamp_t = clock_t::time_point;
    using time_diff_t = std::chrono::duration<uint64_t, std::nano>;

    const time_diff_t ttl_ = std::chrono::seconds(1);
    evaluate_rtt ping_rtt_;

    DomainParticipant* participant_;

    Publisher* ping_publisher_;
    Subscriber* pong_subscriber_;

    DataWriter* ping_writer_;
    DataReader* pong_reader_;

    Topic* ping_topic_;
    Topic* pong_topic_;
    TypeSupport rtt_type_;

    std::atomic_uint count_;
    std::atomic_int stop_;
    uint64_t loss_count_;

    class PubListener : public DataWriterListener
    {
    public:

        PubListener()
            : matched_(0)
        {
        }

        ~PubListener() override
        {
        }

        void on_publication_matched(
                DataWriter* /*writer*/,
                const PublicationMatchedStatus& info) override
        {
            if (info.current_count_change == 1)
            {
                matched_ = info.total_count;
                std::cout << "Publisher matched.\n";
            }
            else if (info.current_count_change == -1)
            {
                matched_ = info.total_count;
                std::cout << "Publisher unmatched.\n";
            }
            else
            {
                std::cout << info.current_count_change
                        << " is not a valid value for PublicationMatchedStatus current count change.\n";
            }
        }

        std::atomic_int matched_;

    } ping_listener_;

    class SubListener : public DataReaderListener
    {
    public:

        SubListener()
            : rtt_sum_(0)
            , samples_per_sec_(0)
        {
        }

        ~SubListener() override
        {
        }

        void on_subscription_matched(
                DataReader* /*reader*/,
                const SubscriptionMatchedStatus& info) override
        {
            if (info.current_count_change == 1)
            {
                std::cout << "Subscriber matched.\n";
            }
            else if (info.current_count_change == -1)
            {
                std::cout << "Subscriber unmatched.\n";
            }
            else
            {
                std::cout << info.current_count_change
                          << " is not a valid value for SubscriptionMatchedStatus current count change\n";
            }
        }

        // pong subscriber callback
        void on_data_available(
                DataReader* reader) override
        {
            timestamp_t now = clock_t::now();
            SampleInfo info;

            if (reader->take_next_sample(&pong_rtt_, &info) == eprosima::fastdds::dds::RETCODE_OK)
            {
                if (!info.valid_data)
                {
                    std::cout << "Received invalid data\n";
                    return;
                }
                uint64_t seqnum;
                timestamp_t old_timestamp;
                std::chrono::steady_clock::duration rtt;

                seqnum = *reinterpret_cast<uint64_t*>(pong_rtt_.payload().data());
                timestamp_map_lock.lock();
                if (timestamp_map_.count(seqnum) == 0)
                {
                    return;
                }
                old_timestamp = timestamp_map_[seqnum];
                rtt = now - old_timestamp;
                timestamp_map_.erase(seqnum);
                timestamp_map_lock.unlock();
                logging_lock.lock();
                rtt_sum_ += rtt;
                samples_per_sec_++;
                logging_lock.unlock();
                csv_output << seqnum << ", ";
                csv_output << old_timestamp.time_since_epoch().count() << ", ";
                csv_output << rtt.count() << '\n';

            }
        }

        evaluate_rtt pong_rtt_;
        std::map<uint64_t, timestamp_t> timestamp_map_;
        time_diff_t rtt_sum_;

        uint32_t samples_per_sec_;

    }
    pong_listener_;

public:

    PingNode()
        : participant_(nullptr)
        , ping_publisher_(nullptr)
        , pong_subscriber_(nullptr)
        , ping_writer_(nullptr)
        , pong_reader_(nullptr)
        , ping_topic_(nullptr)
        , pong_topic_(nullptr)
        , rtt_type_(new evaluate_rttPubSubType())
        , stop_(0)
        , loss_count_(0)
    {
    }

    virtual ~PingNode()
    {
        if (pong_reader_ != nullptr)
        {
            pong_subscriber_->delete_datareader(pong_reader_);
        }
        if (ping_writer_ != nullptr)
        {
            ping_publisher_->delete_datawriter(ping_writer_);
        }
        if (ping_publisher_ != nullptr)
        {
            participant_->delete_publisher(ping_publisher_);
        }
        if (ping_topic_ != nullptr)
        {
            participant_->delete_topic(ping_topic_);
        }
        if (pong_topic_ != nullptr)
        {
            participant_->delete_topic(pong_topic_);
        }
        if (pong_subscriber_ != nullptr)
        {
            participant_->delete_subscriber(pong_subscriber_);
        }
        DomainParticipantFactory::get_instance()->delete_participant(participant_);
    }

    //!Initialize the ping node
    bool init()
    {
        DomainParticipantQos participantQos;
        participantQos.name("Participant_ping");
        participant_ = DomainParticipantFactory::get_instance()->create_participant(0, participantQos);

        if (participant_ == nullptr)
        {
            return false;
        }

        // Register the Type
        rtt_type_.register_type(participant_);

        // Create the ping publications Topic
        TopicQos topic_qos;

        topic_qos.reliability().kind = ReliabilityQosPolicyKind::BEST_EFFORT_RELIABILITY_QOS;
        topic_qos.durability().kind = DurabilityQosPolicyKind::VOLATILE_DURABILITY_QOS;
        topic_qos.history().kind = HistoryQosPolicyKind::KEEP_LAST_HISTORY_QOS;
        topic_qos.history().depth = 1;
        ping_topic_ = participant_->create_topic("PingTopic", "evaluate_rtt", topic_qos);
        pong_topic_ = participant_->create_topic("PongTopic", "evaluate_rtt", topic_qos);

        if (ping_topic_ == nullptr || ping_topic_ == nullptr)
        {
            return false;
        }

        // Create the Publisher
        ping_publisher_ = participant_->create_publisher(PUBLISHER_QOS_DEFAULT, nullptr);

        if (ping_publisher_ == nullptr)
        {
            return false;
        }

        // Create the DataWriter
        ping_writer_ = ping_publisher_->create_datawriter(ping_topic_, DATAWRITER_QOS_DEFAULT, &ping_listener_);

        if (ping_writer_ == nullptr)
        {
            return false;
        }

        // Create the Subscriber
        pong_subscriber_ = participant_->create_subscriber(SUBSCRIBER_QOS_DEFAULT, nullptr);

        if (pong_subscriber_ == nullptr)
        {
            return false;
        }

        // Create the DataReader
        pong_reader_ = pong_subscriber_->create_datareader(pong_topic_, DATAREADER_QOS_DEFAULT, &pong_listener_);

        if (pong_reader_ == nullptr)
        {
            return false;
        }

        ping_rtt_.payload() = {0, };
        return true;
    }

    void log_rtt(std::atomic_int& samples_sent_per_sec)
    {
        std::chrono::duration<double, std::milli> rtt_sum;

        logging_lock.lock();
        rtt_sum = pong_listener_.rtt_sum_;
        pong_listener_.rtt_sum_ = std::chrono::nanoseconds(0);
        logging_lock.unlock();
        if (pong_listener_.samples_per_sec_ == 0)
        {
            std::cout << "avg_rtt=0";
        }
        else
        {
            std::cout << "avg_rtt=" << rtt_sum.count() / pong_listener_.samples_per_sec_;
        }
        std::cout << "  received=" << pong_listener_.samples_per_sec_;
        std::cout << "  loss=" << loss_count_;
        std::cout << '\n';

        pong_listener_.samples_per_sec_ = 0;
        samples_sent_per_sec = 0;
    }

    void monitor_timestamp()
    {
        std::lock_guard<std::mutex> lock(timestamp_map_lock);

        while (!pong_listener_.timestamp_map_.empty())
        {
            timestamp_t now = clock_t::now();
            auto entry = pong_listener_.timestamp_map_.begin();
            time_diff_t diff = now - entry->second;
            if (diff > ttl_)
            {
                ++loss_count_;
                pong_listener_.timestamp_map_.erase(entry->first);
                continue;
            }
            break;
        }
        if (count_ == 0 && pong_listener_.timestamp_map_.empty())
        {
            stop_ = 1;
        }
    }

    bool publish_ping()
    {
        static uint64_t seqnum = 0;

        if (ping_listener_.matched_ < 1)
        {
            return false;
        }

        *reinterpret_cast<uint64_t*>(ping_rtt_.payload().data()) = seqnum;
        timestamp_map_lock.lock();
        pong_listener_.timestamp_map_[seqnum] = clock_t::now();
        timestamp_map_lock.unlock();
        ping_writer_->write(&ping_rtt_);
        ++seqnum;
        return true;
    }

    //!Run the ping publisher and pong subscriber
    void run(uint32_t count, uint32_t interval_ms)
    {
        std::chrono::milliseconds interval(interval_ms);
        std::atomic_int samples_sent_per_sec(0);

        count_ = count;
        // periodically update timestamp_map
        // discard ping if it reaches TTL
        std::thread ts_update_trd([this]()
        {
            const std::chrono::milliseconds update_interval(100);
            while (!stop_)
            {
                auto now = clock_t::now();
                auto next_wake = now + update_interval;
                monitor_timestamp();
                std::this_thread::sleep_until(next_wake);
            }
        });

        // log statistics per 1 second
        std::thread log_trd([this, &samples_sent_per_sec]()
        {
            const std::chrono::milliseconds log_interval(1000);
            while (!stop_)
            {
                auto now = clock_t::now();
                auto next_wake = now + log_interval;
                log_rtt(samples_sent_per_sec);
                std::this_thread::sleep_until(next_wake);
            }
            log_rtt(samples_sent_per_sec);
        });

        // publish ping message
        while (count_ > 0)
        {
            auto now = clock_t::now();
            auto next_wake = now + interval;

            if (publish_ping()) {
                ++samples_sent_per_sec;
                --count_;
            }
            std::this_thread::sleep_until(next_wake);
        }

        ts_update_trd.join();
        log_trd.join();
    }
};

int main(
        int argc,
        char** argv)
{
    // get command line arguments
    uint64_t interval_ms = default_interval_ms;
    uint64_t count = default_count;
    int opt;

    while ((opt = getopt(argc, argv, "i:c:")) != -1) {
         switch (opt) {
         case 'i':
             interval_ms = atoi(optarg);
             break;
         case 'c':
             count = atoi(optarg);
             break;
         default:
             fprintf(stderr, "Usage: %s [-i interval_ms] [-c count]\n",
                     argv[0]);
             exit(1);
         }
    }

    // initialize object for csv file
    std::string csv_filename;
    std::strstream sstream;

    sstream << "rtt_fastdds";
    sstream << "_i" << interval_ms << "_s8" << "_c" << count << ".csv";
    sstream >> csv_filename;
    csv_output.open(csv_filename);
    csv_output << "count, timestamp_ns, rtt_ns\n";


    // run DDS
    PingNode* ping_node = new PingNode();

    if(ping_node->init())
    {
        ping_node->run(count, interval_ms);
    }
    delete ping_node;
    return 0;
}
