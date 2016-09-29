#include <memory>

#include <infinit/model/doughnut/Dock.hh>

#include <elle/log.hh>
#include <elle/network/Interface.hh>
#include <elle/os.hh>

#include <reactor/network/utp-server.hh>

#include <infinit/model/doughnut/Doughnut.hh>
#include <infinit/model/doughnut/Local.hh>
#include <infinit/model/doughnut/consensus/Paxos.hh>
#include <infinit/overlay/Overlay.hh>

ELLE_LOG_COMPONENT("infinit.model.doughnut.Dock")

namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      /*-------------.
      | Construction |
      `-------------*/

    static void retry_forever(elle::Duration start_delay, elle::Duration max_delay,
                              std::string action_name,
                              std::function<void()> action)
    {
      elle::Duration delay = start_delay;
      while (true)
      {
        try
        {
          action();
          return;
        }
        catch (elle::Exception const& e)
        {
          ELLE_WARN("%s: execption %s", action_name, e.what());
          delay = std::min(delay * 2, max_delay);
          reactor::sleep(delay);
        }
      }
    }

      Dock::Dock(Doughnut& doughnut, Protocol protocol)
        : _doughnut(doughnut)
        , _protocol(protocol)
        , _local_utp_server(
          doughnut.local() ? nullptr : new reactor::network::UTPServer)
        , _utp_server(doughnut.local() ?
                      *doughnut.local()->utp_server() :
                      *this->_local_utp_server)
      {
        if (this->_local_utp_server)
        {
          bool v6 = elle::os::getenv("INFINIT_NO_IPV6", "").empty()
            && doughnut.version() >= elle::Version(0, 7, 0);
          this->_local_utp_server->listen(0, v6);
        }
        auto rdv_host = elle::os::getenv("INFINIT_RDV", "rdv.infinit.sh:7890");
        if (!rdv_host.empty())
        {
          auto uid = elle::sprintf("%x", _doughnut.id());
          this->_rdv_connect_thread.reset(
            new reactor::Thread(
              "rdv_connect",
              [this, uid, rdv_host]
              {
                // The remotes_server does not accept incoming connections,
                // it is used to connect Remotes
                retry_forever(10_sec, 120_sec, "Dock RDV connect",
                              [&] {
                                this->_utp_server.rdv_connect(
                                    uid, rdv_host, 120_sec);
                              });
              }));
        }
        for (auto const& interface: elle::network::Interface::get_map(
               elle::network::Interface::Filter::only_up |
               elle::network::Interface::Filter::no_loopback |
               elle::network::Interface::Filter::no_autoip))
          if (interface.second.ipv4_address.size() > 0)
            this->_local_ips.emplace_back(
              boost::asio::ip::address::from_string(
                interface.second.ipv4_address));
      }

      Dock::Dock(Dock&& source)
        : _doughnut(source._doughnut)
        , _protocol(source._protocol)
        , _local_utp_server(std::move(source._local_utp_server))
        , _utp_server(source._utp_server)
      {}

      Dock::~Dock()
      {
        if (this->_rdv_connect_thread)
          this->_rdv_connect_thread->terminate_now();
        this->_rdv_connect_thread.reset();
      }

      void
      Dock::cleanup()
      {
        if (this->_rdv_connect_thread)
          this->_rdv_connect_thread->terminate_now();
        this->_rdv_connect_thread.reset();
      }
      /*-----.
      | Peer |
      `-----*/

      overlay::Overlay::WeakMember
      Dock::make_peer(NodeLocation loc,
                      boost::optional<EndpointsRefetcher> refetcher)
      {
        ELLE_TRACE_SCOPE("%s: get %f", this, loc);
        static bool disable_cache = getenv("INFINIT_DISABLE_PEER_CACHE");
        if (loc.id() == this->_doughnut.id())
        {
          ELLE_TRACE("peer is ourself");
          return this->_doughnut.local();
        }
        if (!disable_cache)
        {
          auto it = this->_peer_cache.find(loc.id());
          if (it != _peer_cache.end())
            return it->second;
        }
        try
        {
          auto res =
            std::make_shared<model::doughnut::consensus::Paxos::RemotePeer>(
              this->doughnut(),
              loc.id(),
              loc.endpoints(),
              this->_utp_server,
              refetcher,
              this->_protocol);
          auto weak_res = overlay::Overlay::WeakMember::own(std::move(res));
          if (!disable_cache)
            this->_peer_cache.emplace(loc.id(), weak_res);
          return weak_res;
        }
        catch (elle::Error const& e)
        {
          elle::err("failed to connect to %f", loc);
        }
      }
    }
  }
}
