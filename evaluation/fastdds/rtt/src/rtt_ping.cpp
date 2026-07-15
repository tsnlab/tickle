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

std::ofstream csv_output_;


class PingNode
{
private:
    using clock_t = std::chrono::steady_clock;
    using timestamp_t = clock_t::time_point;
    using time_diff_t = std::chrono::duration<uint64_t, std::nano>;

    const time_diff_t ttl_ = time_diff_t(1000000000);

    evaluate_rtt ping_rtt_;

    DomainParticipant* participant_;

    Publisher* ping_publisher_;
    Subscriber* pong_subscriber_;

    DataWriter* ping_writer_;
    DataReader* pong_reader_;

    Topic* rtt_topic_;
    TypeSupport rtt_type_;

    std::chrono::milliseconds cumulative_time_ms_;

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
                DataWriter*,
                const PublicationMatchedStatus& info) override
        {
            if (info.current_count_change == 1)
            {
                matched_ = info.total_count;
                std::cout << "Publisher matched." << std::endl;
            }
            else if (info.current_count_change == -1)
            {
                matched_ = info.total_count;
                std::cout << "Publisher unmatched." << std::endl;
            }
            else
            {
                std::cout << info.current_count_change
                        << " is not a valid value for PublicationMatchedStatus current count change." << std::endl;
            }
        }

        std::atomic_int matched_;

    } ping_listener_;

    class SubListener : public DataReaderListener
    {
    public:

        SubListener()
            : samples_(0)
        {
        }

        ~SubListener() override
        {
        }

        void on_subscription_matched(
                DataReader*,
                const SubscriptionMatchedStatus& info) override
        {
            if (info.current_count_change == 1)
            {
                std::cout << "Subscriber matched." << std::endl;
            }
            else if (info.current_count_change == -1)
            {
                std::cout << "Subscriber unmatched." << std::endl;
            }
            else
            {
                std::cout << info.current_count_change
                          << " is not a valid value for SubscriptionMatchedStatus current count change" << std::endl;
            }
        }

        // pong subscriber callback
        void on_data_available(
                DataReader* reader) override
        {
            SampleInfo info;
            if (reader->take_next_sample(&pong_rtt_, &info) == eprosima::fastdds::dds::RETCODE_OK)
            {
                if (info.valid_data)
                {
                    uint64_t sequence_number;
                    timestamp_t old_timestamp;
                    std::chrono::steady_clock::duration rtt;
                    
                    sequence_number = *reinterpret_cast<uint64_t*>(pong_rtt_.payload().data());
                    old_timestamp = timestamp_map_.at(sequence_number);
                    rtt = std::chrono::steady_clock::now() - old_timestamp;
                    timestamp_map_.erase(sequence_number);
                    samples_++;
                    samples_per_sec++;
                    rtt_sum_ += rtt;
                }
            }
        }

        evaluate_rtt pong_rtt_;
        std::map<uint64_t, timestamp_t> timestamp_map_;
        time_diff_t rtt_sum_;

        std::atomic_int samples_;
        std::atomic_int samples_per_sec;

    }
    pong_listener_;

public:

    PingNode()
        : participant_(nullptr)
        , ping_publisher_(nullptr)
        , pong_subscriber_(nullptr)
        , ping_writer_(nullptr)
        , pong_reader_(nullptr)
        , rtt_topic_(nullptr)
        , rtt_type_(new evaluate_rttPubSubType())
        , cumulative_time_ms_(0)
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
        if (rtt_topic_ != nullptr)
        {
            participant_->delete_topic(rtt_topic_);
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
        rtt_topic_ = participant_->create_topic("RttTopic", "evaluate_rtt", TOPIC_QOS_DEFAULT);

        if (rtt_topic_ == nullptr)
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
        ping_writer_ = ping_publisher_->create_datawriter(rtt_topic_, DATAWRITER_QOS_DEFAULT, &ping_listener_);

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
        pong_reader_ = pong_subscriber_->create_datareader(rtt_topic_, DATAREADER_QOS_DEFAULT, &pong_listener_);

        if (pong_reader_ == nullptr)
        {
            return false;
        }

        ping_rtt_.payload() = {0, };
        return true;
    }

    //!Send a ping
    bool publish_ping()
    {
        if (ping_listener_.matched_ > 0)
        {
            uint64_t* current_sequence_number = reinterpret_cast<uint64_t*>(ping_rtt_.payload().data());

            pong_listener_.timestamp_map_[*current_sequence_number++] = clock_t::now();
            ping_writer_->write(&ping_rtt_);
            return true;
        }
        return false;
    }

    //!Run the ping publisher and pong subscriber
    void run(
            uint32_t samples, uint32_t interval_ms)
    {
        std::chrono::seconds cumulative_time_s(0);
        uint64_t samples_sent_per_sec = 0;

        while (true)
        {
            timestamp_t current_timestamp;

            if (publish_ping())
            {
                samples_sent_per_sec++;
            }
            current_timestamp = clock_t::now();
            // actual interval = interval_ms + write overhead
            std::chrono::milliseconds interval(interval_ms);
            cumulative_time_ms_ += interval;
            std::chrono::seconds current_s = std::chrono::duration_cast<std::chrono::seconds>(cumulative_time_ms_);
            if (current_s > cumulative_time_s)
            {
                cumulative_time_s = current_s;
                std::cout << "avg_rtt=" << static_cast<double>(pong_listener_.rtt_sum_.count()) / pong_listener_.samples_per_sec;
                std::cout << "  received=" << pong_listener_.samples_per_sec;
                std::cout << "  loss=" << samples_sent_per_sec - pong_listener_.samples_per_sec;
                std::cout << '\n';
                pong_listener_.samples_per_sec = 0;
                pong_listener_.rtt_sum_ = std::chrono::nanoseconds(0);
                samples_sent_per_sec = 0;
                while (true)
                {
                    auto entry = pong_listener_.timestamp_map_.begin();
                    if (entry->second - current_timestamp > ttl_)
                    {
                        pong_listener_.timestamp_map_.erase(entry->first);
                        continue;
                    }
                    break;
                }
            }
            std::this_thread::sleep_for(interval); // TODO: more accurate interval
            if (pong_listener_.timestamp_map_.empty())
            {
                break;
            }
        }
    }
};

int main(
        int argc,
        char** argv)
{
    uint64_t interval_ms = 1000;
    uint64_t count = 1000;
    PingNode* ping_node = new PingNode();
    std::string csv_filename;
    std::strstream sstream;

    sstream << "rtt_fastdds";
    sstream << "_i" << interval_ms << "_s8" << "_c" << count << ".csv";
    sstream >> csv_filename;
    csv_output_.open(csv_filename);

    if(ping_node->init())
    {
        ping_node->run(count, interval_ms);
    }

    delete ping_node;
    return 0;
}
