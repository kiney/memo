#include <elle/reactor/fwd.hh>

#include <boost/optional.hpp>

#include <infinit/model/Endpoints.hh>
#include <infinit/model/Model.hh>

namespace grpc
{
  class Service;
}

namespace infinit
{
  namespace grpc
  {
    void
    serve_grpc(infinit::model::Model& dht,
               boost::optional<elle::reactor::filesystem::FileSystem&> fs,
               model::Endpoint ep,
               int* effective_port = nullptr);
    std::unique_ptr<::grpc::Service>
    filesystem_service(elle::reactor::filesystem::FileSystem& fs);
    std::unique_ptr<::grpc::Service>
    doughnut_service(infinit::model::Model& dht);
  }
}