#include <chrono>

#include <elle/log.hh>

#include <reactor/network/exception.hh>

// FIXME: can be avoided with a `Dock` accessor in `Overlay`
#include <infinit/model/doughnut/Doughnut.hh>
#include <infinit/model/doughnut/Local.hh>
#include <infinit/model/doughnut/Remote.hh>
#include <infinit/model/doughnut/consensus/Paxos.hh>
#include <infinit/overlay/kouncil/Kouncil.hh>

ELLE_LOG_COMPONENT("infinit.overlay.kouncil.Kouncil")

namespace infinit
{
  namespace overlay
  {
    namespace kouncil
    {
      /*------.
      | Entry |
      `------*/

      Kouncil::Entry::Entry(model::Address node, model::Address block)
        : _node(std::move(node))
        , _block(std::move(block))
      {}

      /*-------------.
      | Construction |
      `-------------*/

      Kouncil::Kouncil(
        model::doughnut::Doughnut* dht,
        std::shared_ptr<infinit::model::doughnut::Local> local)
        : Overlay(dht, local)
        , _address_book()
        , _peers()
        , _new_entries()
        , _broadcast_thread(new reactor::Thread(
                              elle::sprintf("%s: broadcast", this),
                              std::bind(&Kouncil::_broadcast, this)))
        , _watcher_thread(new reactor::Thread(
                          elle::sprintf("%s: watch", this),
                          std::bind(&Kouncil::_watcher, this)))
      {
        using model::Address;
        ELLE_TRACE_SCOPE("%s: construct", this);
        if (local)
        {
          this->_peers.emplace(local);
          this->_infos.insert(std::make_pair(this->id(),
            PeerInfo {
              local->server_endpoints(),
              {},
              std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()).count(),
              std::chrono::high_resolution_clock::now(),
              std::chrono::high_resolution_clock::now()
            }));
          for (auto const& key: local->storage()->list())
            this->_address_book.emplace(this->id(), key);
          ELLE_DEBUG("loaded %s entries from storage",
                     this->_address_book.size());
          this->_connections.emplace_back(local->on_store().connect(
            [this] (model::blocks::Block const& b)
            {
              this->_address_book.emplace(this->id(), b.address());
              std::unordered_set<Address> entries;
              entries.emplace(b.address());
              this->_new_entries.put(b.address());
            }));
          // Add server-side kouncil RPCs.
          local->on_connect().connect(
            [this] (RPCServer& rpcs)
            {
              // List all blocks owned by this node.
              rpcs.add(
                "kouncil_fetch_entries",
                std::function<std::unordered_set<Address> ()>(
                  [this] ()
                  {
                    std::unordered_set<Address> res;
                    auto range = this->_address_book.equal_range(this->id());
                    for (auto it = range.first; it != range.second; ++it)
                      res.emplace(it->block());
                    return res;
                  }));
              // Lookup owners of a block on this node.
              rpcs.add(
                "kouncil_lookup",
                std::function<std::unordered_set<Address>(Address)>(
                  [this] (Address const& addr)
                  {
                    std::unordered_set<Address> res;
                    auto range = this->_address_book.get<1>().equal_range(addr);
                    for (auto it = range.first; it != range.second; ++it)
                      res.emplace(it->node());
                    return res;
                  }));
              // Send known peers to this node and retrieve its known peers.
              if (this->doughnut()->version() < elle::Version(0, 8, 0))
                rpcs.add(
                  "kouncil_advertise",
                  std::function<NodeLocations(NodeLocations const&)>(
                    [this](NodeLocations const& peers)
                    {
                      this->_discover(peers);
                      return this->peers_locations();
                    }));
                else
                  rpcs.add(
                    "kouncil_advertise",
                    std::function<PeerInfos(PeerInfos const&)>(
                      [this](PeerInfos  const& infos)
                      {
                        this->_discover(infos);
                        return this->_infos;
                      }));
            });
        }
        // Add client-side Kouncil RPCs.
        this->doughnut()->dock().on_connect().connect(
          [this] (model::doughnut::Remote& r)
          {
            // Notify this node of new peers.
            if (this->doughnut()->version() < elle::Version(0, 8, 0))
              r.rpc_server().add(
                "kouncil_discover",
                std::function<void (NodeLocations const&)>(
                  [this] (NodeLocations const& nls)
                  {
                    this->_discover(nls);
                  }));
            else
              r.rpc_server().add(
                "kouncil_discover",
                std::function<void(PeerInfos const&)>(
                  [this](PeerInfos const& pis)
                  {
                    this->_discover(pis);
                  }));
            // Notify this node of new blocks owned by the peer.
            r.rpc_server().add(
              "kouncil_add_entries",
              std::function<void (std::unordered_set<Address> const&)>(
                [this, &r] (std::unordered_set<Address> const& entries)
                {
                  for (auto const& addr: entries)
                    this->_address_book.emplace(r.id(), addr);
                  ELLE_TRACE("%s: added %s entries from %f",
                             this, entries.size(), r.id());
                }));
          });
        this->_validate();
      }

      Kouncil::~Kouncil()
      {
        ELLE_TRACE("%s: destruct", this);
        this->_connections.clear();
      }

      NodeLocations
      Kouncil::peers_locations() const
      {
        NodeLocations res;
        for (auto const& p: this->peers())
        {
          if (auto r = dynamic_cast<model::doughnut::Remote const*>(p.get()))
            res.emplace_back(r->id(), r->endpoints());
          else if (auto l = dynamic_cast<model::doughnut::Local const*>(p.get()))
            res.emplace_back(p->id(), elle::unconst(l)->server_endpoints());
        }
        return res;
      }

      elle::json::Json
      Kouncil::query(std::string const& k, boost::optional<std::string> const& v)
      {
        elle::json::Object res;
        if (k == "stats")
        {
          res["peers"] = this->peer_list();
          res["id"] = elle::sprintf("%s", this->doughnut()->id());
        }
        return res;
      }

      void
      Kouncil::_validate() const
      {}

      /*-------------.
      | Address book |
      `-------------*/

      void
      Kouncil::_broadcast()
      {
        while (true)
        {
          std::unordered_set<model::Address> entries;
          auto addr = this->_new_entries.get();
          entries.insert(addr);
          while (!this->_new_entries.empty())
            entries.insert(this->_new_entries.get());
          ELLE_TRACE("%s: broadcast new entry: %f", this, entries);

          this->local()->broadcast<void>(
            "kouncil_add_entries", std::move(entries));
        }
      }

      /*------.
      | Peers |
      `------*/

      void
      Kouncil::_discover(NodeLocations const& peers)
      {
        PeerInfos infos;
        for (auto const& peer: peers)
        {
          infos.emplace(
            peer.id(),
            PeerInfo{
              {},
              peer.endpoints(),
              0,
              std::chrono::high_resolution_clock::now(),
            });
        }
        this->_discover(std::move(infos));
      }

      void
      Kouncil::_discover(PeerInfos::value_type const& p)
      {
        ELLE_ASSERT_NEQ(p.first, this->doughnut()->id());
        NodeLocation nl(p.first, p.second.endpoints_stamped);
        nl.endpoints().merge(p.second.endpoints_unstamped);
        overlay::Overlay::Member peer;
        try
        {
          peer = this->doughnut()->dock().make_peer(
            nl,
            model::EndpointsRefetcher([this] (model::Address id)
                                      {
                                        return this->_endpoints_refetch(id);
                                      })).lock();
        }
        catch (reactor::network::Exception const& e)
        {
          ELLE_TRACE("%s: network exception connecting to %s: %s", this, p, e);
          return;
        }
        if (peer->id() == model::Address::null)
          if (auto r = dynamic_cast<model::doughnut::Remote*>(peer.get()))
          {
            reactor::Barrier b;
            r->id_discovered().connect([&] { b.open();});
            reactor::wait(b);
          }
        if (p.first == model::Address::null)
        {
          auto it = this->_infos.find(peer->id());
          if (it != this->_infos.end())
          {
            ELLE_DEBUG("%s: anon connect gave us a known peer %f", this, peer->id());
            it->second.merge(p.second);
            return;
          }
          this->_infos.insert(std::make_pair(peer->id(), p.second));
        }
        this->_discover(peer);
      }

      void
      Kouncil::_discover(Overlay::Member peer)
      {
        ELLE_ASSERT_NEQ(peer->id(), this->doughnut()->id());
        ELLE_DEBUG("%s: discovered %s", this, peer->id());
        // FIXME: handle local !
        if (auto r = std::dynamic_pointer_cast<model::doughnut::Remote>(peer))
        {
          auto fetch = r->make_rpc<std::unordered_set<model::Address> ()>(
            "kouncil_fetch_entries");
          auto entries = fetch();
          ELLE_ASSERT_NEQ(peer->id(), model::Address::null);
          for (auto const& b: entries)
            this->_address_book.emplace(peer->id(), b);
          ELLE_DEBUG("added %s entries from %f", entries.size(), peer);
        }
        ELLE_ASSERT_NEQ(peer->id(), model::Address::null);
        peer->disconnected().connect([this, ptr=peer.get()] {
            ELLE_TRACE("peer %s disconnected", ptr);
            this->_peer_disconnected(ptr);
        });
        peer->connected().connect([this, ptr=peer.get()] {
            ELLE_TRACE("peer %s connected", ptr);
            auto it = this->_disconnected_peers.get<1>().find(ptr);
            if (it == this->_disconnected_peers.get<1>().end())
            {
              // This can happen at initial connection time, if we registered
              // this function to connected() before it triggered
              ELLE_ASSERT_NEQ(this->_peers.find(ptr->id()), this->_peers.end());
              return;
            }
            this->_peers.insert(*it);
            this->_disconnected_peers.get<1>().erase(it);
            auto r = dynamic_cast<model::doughnut::Remote*>(ptr);
            auto fetch = r->make_rpc<std::unordered_set<model::Address> ()>(
              "kouncil_fetch_entries");
            auto entries = fetch();
            ELLE_ASSERT_NEQ(r->id(), model::Address::null);
            for (auto const& b: entries)
              this->_address_book.emplace(r->id(), b);
            ELLE_DEBUG("added %s entries from %f", entries.size(), r);
        });
        this->_peers.emplace(peer);
        ELLE_DEBUG("%s: notifying connections", this);
        // Broadcast this peer existence
        if (this->local())
        {
          if (auto r = dynamic_cast<model::doughnut::Remote const*>(peer.get()))
          {
            if (this->doughnut()->version() < elle::Version(0, 8, 0))
            {
              NodeLocation nl(peer->id(), r->endpoints());
              this->local()->broadcast<void>("kouncil_discover",
                                             NodeLocations{nl});
            }
            else
            {
              PeerInfos pis;
              pis.insert(*this->_infos.find(peer->id()));
              this->local()->broadcast<void>("kouncil_discover", pis);
            }
          }
        }
        // Send the peer all known hosts and retrieve its known hosts
        // FIXME: handle local !
        if (auto r = dynamic_cast<model::doughnut::Remote*>(peer.get()))
        {
          ELLE_TRACE_SCOPE("fetch know peers of %s", r);
          if (this->doughnut()->version() < elle::Version(0, 8, 0))
          {
            auto advertise = r->make_rpc<NodeLocations (NodeLocations const&)>(
              "kouncil_advertise");
            auto peers = advertise(this->peers_locations());
            ELLE_TRACE("fetched %s peers", peers.size());
            ELLE_DUMP("peers: %s", peers);
            // FIXME: might be useless to broadcast these peers
            this->_discover(peers);
          }
          else
          {
            auto reg = r->make_rpc<PeerInfos(PeerInfos const&)>("kouncil_advertise");
            auto npi = reg(this->_infos);
            this->_discover(npi);
          }
        }
        else
          ELLE_ERR(
            "%s: not sending advertise to non-remote peer %s", this, peer);
      }

      template<typename E>
      std::vector<int>
      pick_n(E& gen, int size, int count)
      {
        std::vector<int> res;
        while (res.size() < static_cast<unsigned int>(count))
        {
          std::uniform_int_distribution<> random(0, size - 1 - res.size());
          int v = random(gen);
          for (auto r: res)
            if (v >= r)
              ++v;
          res.push_back(v);
          std::sort(res.begin(), res.end());
        }
        return res;
      }

      /*-------.
      | Lookup |
      `-------*/

      reactor::Generator<Overlay::WeakMember>
      Kouncil::_allocate(model::Address address, int n) const
      {
        using model::Address;
        return reactor::generator<Overlay::WeakMember>(
          [this, address, n]
          (reactor::Generator<Overlay::WeakMember>::yielder const& yield)
          {
            ELLE_DEBUG("%s: selecting %s nodes from %s peers",
                       this, n, this->_peers.size());
            if (static_cast<unsigned int>(n) >= this->_peers.size())
              for (auto p: this->_peers)
                yield(p);
            else
            {
              std::vector<int> indexes = pick_n(
                this->_gen,
                static_cast<int>(this->_peers.size()),
                n);
              for (auto r: indexes)
                yield(this->peers().get<1>()[r]);
            }
          });
      }

      reactor::Generator<Overlay::WeakMember>
      Kouncil::_lookup(model::Address address, int n, bool) const
      {
        using model::Address;
        return reactor::generator<Overlay::WeakMember>(
          [this, address, n]
          (reactor::Generator<Overlay::WeakMember>::yielder const& yield)
          {
            auto range = this->_address_book.get<1>().equal_range(address);
            int count = 0;
            for (auto it = range.first; it != range.second; ++it)
            {
              auto p = this->peers().find(it->node());
              ELLE_ASSERT(p != this->peers().end());
              yield(*p);
              if (++count >= n)
                break;
            }
            if (count == 0)
            {
              ELLE_TRACE_SCOPE("%s: block %f not found, checking all %s peers",
                               this, address, this->peers().size());
              for (auto peer: this->peers())
              {
                // FIXME: handle local !
                if (auto r = std::dynamic_pointer_cast<
                    model::doughnut::Remote>(peer))
                {
                  auto lookup =
                    r->make_rpc<std::unordered_set<Address> (Address)>(
                      "kouncil_lookup");
                  try
                  {
                    for (auto node: lookup(address))
                    {
                      try
                      {
                        ELLE_DEBUG("peer %f says node %f holds block %f",
                                   r->id(), node, address);
                        yield(this->lookup_node(node));
                        if (++count >= n)
                          break;
                      }
                      catch (NodeNotFound const&)
                      {
                        ELLE_WARN("node %f is said to hold block %f "
                                  "but is unknown to us", node, address);
                      }
                    }
                  }
                  catch (reactor::network::Exception const& e)
                  {
                    ELLE_DEBUG("skipping peer with network issue: %s (%s)", peer, e);
                    continue;
                  }
                  if (count > 0)
                    return;
                }
              }
            }
          });
      }

      Overlay::WeakMember
      Kouncil::_lookup_node(model::Address address) const
      {
        auto it = this->_peers.find(address);
        if (it != this->_peers.end())
        {
          ELLE_DEBUG("%s: node %s found in peers", this, address);
          return *it;
        }
        auto it2 = this->_disconnected_peers.find(address);
        if (it2 != this->_disconnected_peers.end())
        {
          ELLE_DEBUG("%s: node %s found in disconnected peers", this, address);
          return *it2;
        }
        ELLE_DEBUG("%s: node %s not found", this, address);
        return Overlay::WeakMember();
      }

      void
      Kouncil::_perform(std::string const& name, std::function<void()> job)
      {
        this->_tasks.emplace_back(new reactor::Thread(name, job));
        for (unsigned i=0; i<this->_tasks.size(); ++i)
        {
          if (this->_tasks[i]->done())
          {
            std::swap(this->_tasks[i], this->_tasks[this->_tasks.size()-1]);
            this->_tasks.pop_back();
            --i;
          }
        }
      }

      /*-----------.
      | Monitoring |
      `-----------*/

      std::string
      Kouncil::type_name()
      {
        return "kouncil";
      }

      elle::json::Array
      Kouncil::peer_list()
      {
        elle::json::Array res;
        for (auto const& p: this->peers())
          if (auto r = dynamic_cast<model::doughnut::Remote const*>(p.get()))
          {
            elle::json::Array endpoints;
            for (auto const& e: r->endpoints())
              endpoints.push_back(elle::sprintf("%s", e));
            res.push_back(elle::json::Object{
              { "id", elle::sprintf("%x", r->id()) },
              { "endpoints",  endpoints },
              { "connected",  true},
            });
          }
        for (auto const& p: this->_disconnected_peers)
          if (auto r = dynamic_cast<model::doughnut::Remote const*>(p.get()))
          {
            elle::json::Array endpoints;
            for (auto const& e: r->endpoints())
              endpoints.push_back(elle::sprintf("%s", e));
            res.push_back(elle::json::Object{
              { "id", elle::sprintf("%x", r->id()) },
              { "endpoints",  endpoints },
              { "connected",  false},
            });
          }
        return res;
      }

      elle::json::Object
      Kouncil::stats()
      {
        elle::json::Object res;
        res["type"] = this->type_name();
        res["id"] = elle::sprintf("%s", this->doughnut()->id());
        return res;
      }

      void
      Kouncil::_peer_disconnected(model::doughnut::Peer* peer)
      {
        auto it = this->_peers.find(peer->id());
        ELLE_ASSERT_NEQ(it, this->_peers.end());
        ELLE_ASSERT_EQ(it->get(), peer);
        auto& pi = this->_infos.at(peer->id());
        pi.last_seen = std::chrono::high_resolution_clock::now();
        pi.last_contact_attempt = pi.last_seen;
        this->_disconnected_peers.insert(*it);
        ELLE_TRACE("removing %s from peers", peer);
        auto its = this->_address_book.equal_range(peer->id());
        this->_address_book.erase(its.first, its.second);
        this->_peers.erase(it);
      }

      void
      Kouncil::_discover(PeerInfos const& pis)
      {
        for (auto const& pi: pis)
        {
          if (pi.first == model::Address::null)
            this->_perform("connect",
              [this, pi = pi] {
              this->_discover(pi);});
          else
          {
            bool should_discover = false;
            auto it = this->_infos.find(pi.first);
            if (it == this->_infos.end())
            {
              this->_infos.insert(pi);
              should_discover = true;
            }
            else
            {
              if (it->second.merge(pi.second))
              { // New data on a connected peer, we need to notify observers
                // FIXME: maybe notify on reconnection instead?
                this->_notify_observers(*it);
              }
            }
            if (should_discover)
              this->_perform("connect",
                [this, pi = *this->_infos.find(pi.first)] {
                this->_discover(pi);});
          }
        }
      }

      void
      Kouncil::_notify_observers(PeerInfos::value_type const& pi)
      {
        if (!this->local())
          return;
        this->_perform("notify observers",
          [this, pi=pi]
          {
            try
            {
              ELLE_DEBUG("%s: notifying observers of %s", this, pi);
              if (this->doughnut()->version() < elle::Version(0, 8, 0))
              {
                NodeLocation nl(pi.first, pi.second.endpoints_stamped);
                nl.endpoints().merge(pi.second.endpoints_unstamped);
                this->local()->broadcast<void>("kouncil_discover",
                                               NodeLocations{nl});
              }
              else
              {
                PeerInfos pis;
                pis.insert(pi);
                this->local()->broadcast<void>("kouncil_discover", pis);
              }
            }
            catch (elle::Error const& e)
            {
              ELLE_WARN("%s: unable to notify observer: %s", this, e);
            }
          });
      }

      void
      Kouncil::_advertise(model::doughnut::Remote& r)
      {
        ELLE_TRACE_SCOPE("fetch know peers of %s", r);
        try
        {
          if (this->doughnut()->version() < elle::Version(0, 8, 0))
          {
            auto advertise = r.make_rpc<NodeLocations (NodeLocations const&)>(
              "kouncil_advertise");
            auto peers = advertise(this->peers_locations());
            ELLE_TRACE("fetched %s peers", peers.size());
            ELLE_DUMP("peers: %s", peers);
            // FIXME: might be useless to broadcast these peers
            this->_discover_rpc(peers);
          }
          else
          {
            auto reg = r.make_rpc<PeerInfos(PeerInfos const&)>("kouncil_advertise");
            auto npi = reg(this->_infos);
            ELLE_TRACE("fetched %s peers", npi.size());
            ELLE_DUMP("peers: %s", npi);
            this->_discover(npi);
          }
        }
        catch (reactor::network::Exception const& e)
        {
          ELLE_TRACE("%s: network exception advertising %s: %s", this, r, e);
          // nothing to do, disconnected() will be emited and handled
        }
      }

      boost::optional<Endpoints>
      Kouncil::_endpoints_refetch(model::Address id)
      {
        auto it = this->_infos.find(id);
        if (it != this->_infos.end())
        {
          Endpoints res;
          res.merge(it->second.endpoints_stamped);
          res.merge(it->second.endpoints_unstamped);
          ELLE_DEBUG("updating endpoints for %s with %s entries", id, res.size());
          return res;
        }
        return boost::none;
      }

      bool
      Kouncil::PeerInfo::merge(Kouncil::PeerInfo const& from)
      {
        bool changed = false;
        if (this->stamp < from.stamp)
        {
          this->endpoints_stamped = from.endpoints_stamped;
          this->stamp = from.stamp;
          changed = true;
        }
        if (this->endpoints_unstamped.merge(from.endpoints_unstamped))
          changed = true;
        return changed;
      }

      void
      Kouncil::_watcher()
      {
        while (true)
        {
          auto now = std::chrono::high_resolution_clock::now();
          auto it = this->_disconnected_peers.begin();
          auto it2 = it;
          while (it != this->_disconnected_peers.end())
          {
            it2 = it;
            ++it2;
            auto id = (*it)->id();
            auto& info = this->_infos.at(id);
            if ((now - info.last_seen) / 2 < now - info.last_contact_attempt
              || now - info.last_contact_attempt > std::chrono::seconds(60))
            {
              ELLE_TRACE("%s: attempting to contact %s", this, *it);
              info.last_contact_attempt = now;
              // try contacting this node again
              this->_perform("ping", [peer=*it] {
                  try
                  {
                    peer->fetch(model::Address::random(), boost::none);
                  }
                  catch (elle::Error const& e)
                  {
                  }
              });
            }
            it = it2;
          }
          reactor::sleep(10_sec);
        }
      }
    }
  }
}
