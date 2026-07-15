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

class PongNode
{
private:

    DomainParticipant* participant_;

    Publisher* pong_publisher_;
    Subscriber* ping_subscriber_;

    DataWriter* pong_writer_;
    DataReader* ping_reader_;

    Topic* rtt_topic_;

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

        SubListener(DataWriter* pong_writer)
            : pong_writer_(pong_writer)
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

        // ping subscriber callback
        void on_data_available(
                DataReader* reader) override
        {
            SampleInfo info;
            if (reader->take_next_sample(&ping_rtt_, &info) == eprosima::fastdds::dds::RETCODE_OK)
            {
                if (info.valid_data)
                {
                    pong_writer_->write(&ping_rtt_);
                }
            }
        }

        DataWriter* pong_writer_;
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
        , rtt_topic_(nullptr)
        , rtt_type_(new evaluate_rttPubSubType())
        , ping_listener_(pong_writer_)
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
        if (rtt_topic_ != nullptr)
        {
            participant_->delete_topic(rtt_topic_);
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
        rtt_topic_ = participant_->create_topic("RttTopic", "evaluate_rtt", TOPIC_QOS_DEFAULT); // TODO: BE QoS

        if (rtt_topic_ == nullptr)
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
        pong_writer_ = pong_publisher_->create_datawriter(rtt_topic_, DATAWRITER_QOS_DEFAULT, &pong_listener_);

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
        ping_reader_ = ping_subscriber_->create_datareader(rtt_topic_, DATAREADER_QOS_DEFAULT, &ping_listener_);

        if (ping_reader_ == nullptr)
        {
            return false;
        }

        return true;
    }

    //!Run the pong publisher and ping subscriber
    void run(
            uint32_t samples)
    {
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

};

int main(
        int argc,
        char** argv)
{
    std::cout << "Starting subscriber." << std::endl;
    uint32_t samples = 10;

    PongNode* mysub = new PongNode();
    if (mysub->init())
    {
        mysub->run(samples);
    }

    delete mysub;
    return 0;
}
