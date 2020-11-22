// Generated by the gRPC C++ plugin.
// If you make any local change, they will be lost.
// source: kvsService.proto

#include "kvsService.pb.h"
#include "kvsService.grpc.pb.h"

#include <functional>
#include <grpcpp/impl/codegen/async_stream.h>
#include <grpcpp/impl/codegen/async_unary_call.h>
#include <grpcpp/impl/codegen/channel_interface.h>
#include <grpcpp/impl/codegen/client_unary_call.h>
#include <grpcpp/impl/codegen/client_callback.h>
#include <grpcpp/impl/codegen/message_allocator.h>
#include <grpcpp/impl/codegen/method_handler.h>
#include <grpcpp/impl/codegen/rpc_service_method.h>
#include <grpcpp/impl/codegen/server_callback.h>
#include <grpcpp/impl/codegen/server_callback_handlers.h>
#include <grpcpp/impl/codegen/server_context.h>
#include <grpcpp/impl/codegen/service_type.h>
#include <grpcpp/impl/codegen/sync_stream.h>

static const char* kvsService_method_names[] = {
  "/kvsService/kvsPut",
  "/kvsService/kvsGet",
};

std::unique_ptr< kvsService::Stub> kvsService::NewStub(const std::shared_ptr< ::grpc::ChannelInterface>& channel, const ::grpc::StubOptions& options) {
  (void)options;
  std::unique_ptr< kvsService::Stub> stub(new kvsService::Stub(channel));
  return stub;
}

kvsService::Stub::Stub(const std::shared_ptr< ::grpc::ChannelInterface>& channel)
  : channel_(channel), rpcmethod_kvsPut_(kvsService_method_names[0], ::grpc::internal::RpcMethod::NORMAL_RPC, channel)
  , rpcmethod_kvsGet_(kvsService_method_names[1], ::grpc::internal::RpcMethod::NORMAL_RPC, channel)
  {}

::grpc::Status kvsService::Stub::kvsPut(::grpc::ClientContext* context, const ::putRequest& request, ::putResponse* response) {
  return ::grpc::internal::BlockingUnaryCall(channel_.get(), rpcmethod_kvsPut_, context, request, response);
}

void kvsService::Stub::experimental_async::kvsPut(::grpc::ClientContext* context, const ::putRequest* request, ::putResponse* response, std::function<void(::grpc::Status)> f) {
  ::grpc::internal::CallbackUnaryCall(stub_->channel_.get(), stub_->rpcmethod_kvsPut_, context, request, response, std::move(f));
}

void kvsService::Stub::experimental_async::kvsPut(::grpc::ClientContext* context, const ::putRequest* request, ::putResponse* response, ::grpc::experimental::ClientUnaryReactor* reactor) {
  ::grpc::internal::ClientCallbackUnaryFactory::Create(stub_->channel_.get(), stub_->rpcmethod_kvsPut_, context, request, response, reactor);
}

::grpc::ClientAsyncResponseReader< ::putResponse>* kvsService::Stub::PrepareAsynckvsPutRaw(::grpc::ClientContext* context, const ::putRequest& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncResponseReaderFactory< ::putResponse>::Create(channel_.get(), cq, rpcmethod_kvsPut_, context, request, false);
}

::grpc::ClientAsyncResponseReader< ::putResponse>* kvsService::Stub::AsynckvsPutRaw(::grpc::ClientContext* context, const ::putRequest& request, ::grpc::CompletionQueue* cq) {
  auto* result =
    this->PrepareAsynckvsPutRaw(context, request, cq);
  result->StartCall();
  return result;
}

::grpc::Status kvsService::Stub::kvsGet(::grpc::ClientContext* context, const ::getRequest& request, ::getResponse* response) {
  return ::grpc::internal::BlockingUnaryCall(channel_.get(), rpcmethod_kvsGet_, context, request, response);
}

void kvsService::Stub::experimental_async::kvsGet(::grpc::ClientContext* context, const ::getRequest* request, ::getResponse* response, std::function<void(::grpc::Status)> f) {
  ::grpc::internal::CallbackUnaryCall(stub_->channel_.get(), stub_->rpcmethod_kvsGet_, context, request, response, std::move(f));
}

void kvsService::Stub::experimental_async::kvsGet(::grpc::ClientContext* context, const ::getRequest* request, ::getResponse* response, ::grpc::experimental::ClientUnaryReactor* reactor) {
  ::grpc::internal::ClientCallbackUnaryFactory::Create(stub_->channel_.get(), stub_->rpcmethod_kvsGet_, context, request, response, reactor);
}

::grpc::ClientAsyncResponseReader< ::getResponse>* kvsService::Stub::PrepareAsynckvsGetRaw(::grpc::ClientContext* context, const ::getRequest& request, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncResponseReaderFactory< ::getResponse>::Create(channel_.get(), cq, rpcmethod_kvsGet_, context, request, false);
}

::grpc::ClientAsyncResponseReader< ::getResponse>* kvsService::Stub::AsynckvsGetRaw(::grpc::ClientContext* context, const ::getRequest& request, ::grpc::CompletionQueue* cq) {
  auto* result =
    this->PrepareAsynckvsGetRaw(context, request, cq);
  result->StartCall();
  return result;
}

kvsService::Service::Service() {
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      kvsService_method_names[0],
      ::grpc::internal::RpcMethod::NORMAL_RPC,
      new ::grpc::internal::RpcMethodHandler< kvsService::Service, ::putRequest, ::putResponse>(
          [](kvsService::Service* service,
             ::grpc::ServerContext* ctx,
             const ::putRequest* req,
             ::putResponse* resp) {
               return service->kvsPut(ctx, req, resp);
             }, this)));
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      kvsService_method_names[1],
      ::grpc::internal::RpcMethod::NORMAL_RPC,
      new ::grpc::internal::RpcMethodHandler< kvsService::Service, ::getRequest, ::getResponse>(
          [](kvsService::Service* service,
             ::grpc::ServerContext* ctx,
             const ::getRequest* req,
             ::getResponse* resp) {
               return service->kvsGet(ctx, req, resp);
             }, this)));
}

kvsService::Service::~Service() {
}

::grpc::Status kvsService::Service::kvsPut(::grpc::ServerContext* context, const ::putRequest* request, ::putResponse* response) {
  (void) context;
  (void) request;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}

::grpc::Status kvsService::Service::kvsGet(::grpc::ServerContext* context, const ::getRequest* request, ::getResponse* response) {
  (void) context;
  (void) request;
  (void) response;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}


