#include <iostream>
#include <memory>
#include <string>
#include <queue>
#include <thread>

#include <grpc++/grpc++.h>

#include "PerfTest.grpc.pb.h"
#include <grpc++/impl/codegen/async_stream.h>

#define MAX_RPCS_AFT 1000

using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;
using perftest::PerfTest;
using perftest::HelloRequest;
using perftest::HelloResponse;

int send_count;
int reply_count;

class PerfTestClient {
public:
    explicit PerfTestClient(std::shared_ptr<Channel> channel, int rpc_count)
    : stub_(PerfTest::NewStub(channel)), num_rpcs(rpc_count) {
        
        std::time_t start_time = std::time(nullptr);
        std::cout << "Start time: " << std::asctime(std::localtime(&start_time)) << start_time << std::endl;
        
        sender =  std::thread(&PerfTestClient::PerfHelloSend, this, "Hello", rpc_count);
        collector =  std::thread(&PerfTestClient::PerfHelloWait, this, rpc_count);
    }
    
    ~PerfTestClient() {
        sender.join();
        std::time_t sender_end_time = std::time(nullptr);
        std::cout << "Sent count: " << send_count << std::endl;
        std::cout << "Sender End time: " << std::asctime(std::localtime(&sender_end_time)) << sender_end_time << std::endl;
        collector.join();
        std::time_t collector_end_time = std::time(nullptr);
        std::cout << "Reply count: " << reply_count << std::endl;
        std::cout << "Sender End time: " << std::asctime(std::localtime(&collector_end_time)) << collector_end_time << std::endl;
    }
    
    void PerfHelloSend(const std::string& text, int num_rpcs) {
        Status status;
        
        for (int i = 0; i < num_rpcs; i++) {
            HelloRequest request;
            request.set_text(text);
            ClientContext *context(new grpc::ClientContext);
            
            // std::lock_guard<std::mutex> lock(m);
            rpc_vector.push_back(stub_->AsyncHelloWorld(context, request, &cq));
            send_count++;
        }
 
    }

    void PerfHelloWait(int num_rpcs) {
        Status status;
        int count = 0;
        while ((volatile int)send_count != num_rpcs) {
            sleep(1);
        }
        while (count < num_rpcs) {
            HelloResponse reply;
            void* got_tag;
            bool ok = false;
            
            // std::lock_guard<std::mutex> lock(m);
            if (rpc_vector.size() && rpc_vector.back()) {
                rpc_vector.back()->Finish(&reply, &status, (void*)count);
                
                cq.Next(&got_tag, &ok);
                GPR_ASSERT(got_tag == (void*)count);
                GPR_ASSERT(ok);
                
                if (status.ok()) {
                    // std::cout << reply.text() << std::endl;
                    reply_count++;
                }
                count++;
                
                rpc_vector.pop_back();
            }
        }
    }
    
    CompletionQueue cq;
    std::unique_ptr<PerfTest::Stub> stub_;
    std::vector<std::unique_ptr<ClientAsyncResponseReader<HelloResponse> >> rpc_vector;
    int num_rpcs;
    
    mutable std::mutex m;
    std::thread sender;
    std::thread collector;
};

int main(int argc, char** argv) {
    
    PerfTestClient perfClient(grpc::CreateChannel("unix:/tmp/grpc_socket", grpc::InsecureChannelCredentials()), 1000000);
    
    return 0;
}

