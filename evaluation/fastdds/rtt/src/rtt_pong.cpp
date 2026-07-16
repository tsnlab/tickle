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

#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <queue>
#include <thread>

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

std::condition_variable cv;
std::mutex cv_mutex;

class PongNode
{
private:
    
    std::queue<uint64_t> seqnum_queue_;
    evaluate_rtt pong_rtt_;

    DomainParticipant* participant_;

    Publisher* pong_publisher_;
    Subscriber* ping_subscriber_;

    DataWriter* pong_writer_;
    DataReader* ping_reader_;

    Topic* ping_topic_;
    Topic* pong_topic_;

    TypeSupport rtt_type_;

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

    } pong_listener_;

    class SubListener : public DataReaderListener
    {
    public:

        SubListener(std::queue<uint64_t>& seqnum_queue)
            : seqnum_queue_(seqnum_queue)
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
                std::lock_guard<std::mutex> lock(cv_mutex);
                seqnum_queue_ = std::queue<uint64_t>();
                std::cout << "Subscriber unmatched." << std::endl;
            }
            else
            {
                std::cout << info.current_count_change
                          << " is not a valid value for SubscriptionMatchedStatus current count change" << std::endl;
            }
        }

        // ping subscriber callback
        void on_data_available(
                DataReader* reader) override
        {
            SampleInfo info;
            if (reader->take_next_sample(&ping_rtt_, &info) == eprosima::fastdds::dds::RETCODE_OK)
            {
                if (info.valid_data)
                {
                    std::lock_guard<std::mutex> lock(cv_mutex);

                    uint64_t sequence_number = *reinterpret_cast<uint64_t*>(ping_rtt_.payload().data());
                    seqnum_queue_.push(sequence_number);
                    cv.notify_all();
                }
            }
        }
        std::queue<uint64_t>& seqnum_queue_;
        evaluate_rtt ping_rtt_;

    }
    ping_listener_;

public:

    PongNode()
        : participant_(nullptr)
        , pong_publisher_(nullptr)
        , ping_subscriber_(nullptr)
        , pong_writer_(nullptr)
        , ping_reader_(nullptr)
        , ping_topic_(nullptr)
        , pong_topic_(nullptr)
        , rtt_type_(new evaluate_rttPubSubType())
        , ping_listener_(seqnum_queue_)
    {
    }

    virtual ~PongNode()
    {
        if (ping_reader_ != nullptr)
        {
            ping_subscriber_->delete_datareader(ping_reader_);
        }
        if (pong_writer_ != nullptr)
        {
            pong_publisher_->delete_datawriter(pong_writer_);
        }
        if (pong_publisher_ != nullptr)
        {
            participant_->delete_publisher(pong_publisher_);
        }
        if (ping_topic_ != nullptr)
        {
            participant_->delete_topic(ping_topic_);
        }
        if (pong_topic_ != nullptr)
        {
            participant_->delete_topic(pong_topic_);
        }
        if (ping_subscriber_ != nullptr)
        {
            participant_->delete_subscriber(ping_subscriber_);
        }
        DomainParticipantFactory::get_instance()->delete_participant(participant_);
    }

    //!Initialize the pong node
    bool init()
    {
        DomainParticipantQos participantQos;
        participantQos.name("Participant_pong");
        participant_ = DomainParticipantFactory::get_instance()->create_participant(0, participantQos);

        if (participant_ == nullptr)
        {
            return false;
        }

        // Register the Type
        rtt_type_.register_type(participant_);

        // Create the subscriptions Topic
        TopicQos topic_qos;

        topic_qos.reliability().kind = ReliabilityQosPolicyKind::BEST_EFFORT_RELIABILITY_QOS;
        topic_qos.durability().kind = DurabilityQosPolicyKind::VOLATILE_DURABILITY_QOS;
        topic_qos.history().kind = HistoryQosPolicyKind::KEEP_LAST_HISTORY_QOS;
        topic_qos.history().depth = 1;
        ping_topic_ = participant_->create_topic("PingTopic", "evaluate_rtt", topic_qos); // TODO: BE QoS
        pong_topic_ = participant_->create_topic("PongTopic", "evaluate_rtt", topic_qos); // TODO: BE QoS

        if (ping_topic_ == nullptr || pong_topic_ == nullptr)
        {
            return false;
        }

        // Create the Publisher
        pong_publisher_ = participant_->create_publisher(PUBLISHER_QOS_DEFAULT, nullptr);

        if (pong_publisher_ == nullptr)
        {
            return false;
        }

        // Create the DataWriter
        pong_writer_ = pong_publisher_->create_datawriter(pong_topic_, DATAWRITER_QOS_DEFAULT, &pong_listener_);

        if (pong_writer_ == nullptr)
        {
            return false;
        }

        // Create the Subscriber
        ping_subscriber_ = participant_->create_subscriber(SUBSCRIBER_QOS_DEFAULT, nullptr);

        if (ping_subscriber_ == nullptr)
        {
            return false;
        }

        // Create the DataReader
        ping_reader_ = ping_subscriber_->create_datareader(ping_topic_, DATAREADER_QOS_DEFAULT, &ping_listener_);

        if (ping_reader_ == nullptr)
        {
            return false;
        }

        return true;
    }

    //!Run the pong publisher and ping subscriber
    void run()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(cv_mutex);

            cv.wait(lock);
            while (seqnum_queue_.size() > 0)
            {
                uint64_t seqnum = seqnum_queue_.front();

                seqnum_queue_.pop();
                *reinterpret_cast<uint64_t*>(pong_rtt_.payload().data()) = seqnum;
                pong_writer_->write(&pong_rtt_);
            }
        }
    }

};

int main(int argc, char** argv)
{
    std::cout << "Starting subscriber." << std::endl;

    PongNode* mysub = new PongNode();
    if (mysub->init())
    {
        mysub->run();
    }

    delete mysub;
    return 0;
}
