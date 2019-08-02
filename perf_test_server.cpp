#include <forward_list>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <grpc++/grpc++.h>

#include "PerfTest.grpc.pb.h"
#include <grpc++/impl/codegen/async_stream.h>

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerCompletionQueue;
using grpc::Status;
using perftest::PerfTest;
using perftest::HelloRequest;
using perftest::HelloResponse;
using namespace std::placeholders;

int response_count;

static Status ProcessSimpleRPC(const perftest::HelloRequest *request,
                               perftest::HelloResponse *response) {
    if (request->text().size() > 0) {
        response->set_text("Received");
    }
    response_count++;
    return Status::OK;
}

class ServerRpcContext {
public:
    ServerRpcContext() {}
    virtual ~ServerRpcContext(){};
    virtual bool RunNextState(bool) = 0;  // next state, return false if done
    virtual void Reset() = 0;             // start this back at a clean state
};

class ServerRpcContextHelloWorld GRPC_FINAL : public ServerRpcContext {
public:
    ServerRpcContextHelloWorld(std::function<void(ServerContext *ctx, HelloRequest *, grpc::ServerAsyncResponseWriter<HelloResponse> *, void *)> request_method,
                               std::function<grpc::Status(const HelloRequest *, HelloResponse *)> invoke_method)
    : srv_ctx_(new ServerContext),
      next_state_(&ServerRpcContextHelloWorld::invoker),
      request_method_(request_method),
      invoke_method_(invoke_method),
      response_writer_(srv_ctx_.get())
    {
        request_method_(srv_ctx_.get(), &req_, &response_writer_, (void *)this);
    }
    
    ~ServerRpcContextHelloWorld() GRPC_OVERRIDE {}
    
    bool RunNextState(bool ok) GRPC_OVERRIDE {
        return (this->*next_state_)(ok);
    }
    void Reset() GRPC_OVERRIDE {
        srv_ctx_.reset(new ServerContext);
        req_ = HelloRequest();
        response_writer_ = grpc::ServerAsyncResponseWriter<HelloResponse>(srv_ctx_.get());
        
        // Then request the method
        next_state_ = &ServerRpcContextHelloWorld::invoker;
        request_method_(srv_ctx_.get(), &req_, &response_writer_, (void *)this);
    }

    
private:
    bool finisher(bool) { return false; }

    bool invoker(bool ok) {
        if (!ok) {
            return false;
        }
        
        HelloResponse response;
        
        // Call the RPC processing function
        grpc::Status status = invoke_method_(&req_, &response);
        
        // Have the response writer work and invoke on_finish when done
        next_state_ = &ServerRpcContextHelloWorld::finisher;
        response_writer_.Finish(response, status, (void *)this);
        return true;
    }
    
    std::unique_ptr<ServerContext> srv_ctx_;

    HelloRequest req_;
    grpc::ServerAsyncResponseWriter<HelloResponse> response_writer_;

    bool (ServerRpcContextHelloWorld::*next_state_)(bool);

    std::function<void(ServerContext *ctx, 
                       HelloRequest *, 
                       grpc::ServerAsyncResponseWriter<HelloResponse> *, 
                       void *)> request_method_;
    std::function<grpc::Status(const HelloRequest *, HelloResponse *)> invoke_method_;
    
};

class ServerImpl {

private:
    ServerBuilder builder;
    std::vector<std::unique_ptr<grpc::ServerCompletionQueue>> srv_cqs_;
    std::forward_list<ServerRpcContext *> contexts_;
    std::vector<std::thread> threads_;
    
    perftest::PerfTest::AsyncService service_;
    std::unique_ptr<Server> server_;
    int num_threads;
    
public:
    ServerImpl() {
        num_threads = 5;
        std::string server_address("unix:/tmp/grpc_socket");
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(&service_);
        
        auto process_rpc_bound = std::bind(ProcessSimpleRPC, _1, _2);
        
        // Add one completion queue per thread
        for (int i = 0; i < num_threads; i++) {
            srv_cqs_.emplace_back(builder.AddCompletionQueue());
        }
        
        server_ = builder.BuildAndStart();
        
        // Add a bunch of contexts
        for (int i = 0; i < 10000 / num_threads; i++) {
            for (int j = 0; j < num_threads; j++) {
                auto request_unary = std::bind(&perftest::PerfTest::AsyncService::RequestHelloWorld, &service_, _1, _2, _3, srv_cqs_[j].get(), srv_cqs_[j].get(), _4);
                contexts_.push_front(new ServerRpcContextHelloWorld(request_unary, process_rpc_bound));
            }
        }
        
        for (int i = 0; i < num_threads; i++) {
            shutdown_state_.emplace_back(new PerThreadShutdownState());
        }

        for (int i = 0; i < num_threads; i++) {
            threads_.emplace_back(&ServerImpl::ThreadFunc, this, i);
        }

    }
    
    ~ServerImpl() {
        for (auto thr = threads_.begin(); thr != threads_.end(); thr++) {
            thr->join();
        }
        server_->Shutdown();
        for (auto ss = shutdown_state_.begin(); ss != shutdown_state_.end(); ++ss) {
            (*ss)->set_shutdown();
        }
        for (auto cq = srv_cqs_.begin(); cq != srv_cqs_.end(); ++cq) {
            (*cq)->Shutdown();
            bool ok;
            void *got_tag;
            while ((*cq)->Next(&got_tag, &ok))
                ;
        }
        while (!contexts_.empty()) {
            delete contexts_.front();
            contexts_.pop_front();
        }
    }

    void ThreadFunc(int rank) {
        // Wait until work is available or we are shutting down
        bool ok;
        void *got_tag;
        
        while (srv_cqs_[rank]->Next(&got_tag, &ok)) {
            
            ServerRpcContextHelloWorld *ctx = reinterpret_cast<ServerRpcContextHelloWorld *>(got_tag);
            
            // The tag is a pointer to an RPC context to invoke
            const bool still_going = ctx->RunNextState(ok);
            if (!shutdown_state_[rank]->shutdown()) {
                // this RPC context is done, so refresh it
                if (!still_going) {
                    ctx->Reset();
                }
            } else {
                return;
            }
        }
        return;
    }
    
    class PerThreadShutdownState {
    public:
        PerThreadShutdownState() : shutdown_(false) {}
        
        bool shutdown() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return shutdown_;
        }
        
        void set_shutdown() {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        
    private:
        mutable std::mutex mutex_;
        bool shutdown_;
    };
    std::vector<std::unique_ptr<PerThreadShutdownState>> shutdown_state_;
    

};

int main(int argc, char** argv) {
    ServerImpl server;
    return 0;
}



