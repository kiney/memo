#include <infinit/model/doughnut/Doughnut.hh>

#include <elle/Buffer.hh>
#include <elle/Error.hh>
#include <elle/IOStream.hh>
#include <elle/cast.hh>
#include <elle/log.hh>
#include <elle/serialization/json.hh> // FIXME

#include <reactor/Scope.hh>
#include <reactor/exception.hh>

#include <infinit/storage/MissingKey.hh>

#include <infinit/model/MissingBlock.hh>
#include <infinit/model/blocks/ImmutableBlock.hh>
#include <infinit/model/blocks/MutableBlock.hh>
#include <infinit/model/doughnut/ACB.hh>
#include <infinit/model/doughnut/OKB.hh>
#include <infinit/model/doughnut/Remote.hh>
#include <infinit/model/doughnut/UB.hh>
#include <infinit/model/doughnut/User.hh>

ELLE_LOG_COMPONENT("infinit.model.doughnut.Doughnut");

#include <infinit/model/doughnut/CHB.cc>

namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      class PlainMutableBlock: public blocks::MutableBlock
      {
      public:
        typedef blocks::MutableBlock Super;
        PlainMutableBlock()
        : Super(Address::random()) {}
        PlainMutableBlock(elle::serialization::Serializer& input)
        : Super(input) {}
      };
      static const elle::serialization::Hierarchy<blocks::Block>::
      Register<PlainMutableBlock> _register_pmb_serialization("PMB");
      class PlainACLBlock: public blocks::ACLBlock
      {
      public:
        typedef blocks::ACLBlock Super;
        PlainACLBlock()
        : Super(Address::random()) {}
        PlainACLBlock(elle::serialization::Serializer& input)
        : Super(input) {}
      };
      static const elle::serialization::Hierarchy<blocks::Block>::
      Register<PlainACLBlock> _register_paclb_serialization("PACLB");
      class PlainImmutableBlock: public blocks::ImmutableBlock
      {
      public:
        PlainImmutableBlock()
        : Super(Address::random()) {}
        PlainImmutableBlock(elle::Buffer b)
        : Super(Address::random(), b)
        {}
        PlainImmutableBlock(elle::serialization::Serializer& input)
        : Super(input)
        {}
        typedef blocks::ImmutableBlock Super;
      };
      static const elle::serialization::Hierarchy<blocks::Block>::
      Register<PlainImmutableBlock> _register_pib_serialization("PIB");

      Doughnut::Doughnut(std::unique_ptr<overlay::Overlay> overlay,
                         std::unique_ptr<Consensus> consensus,
                         bool plain)
        : _overlay(std::move(overlay))
        , _consensus(std::move(consensus))
        , _keys()
        , _plain(plain)
      {
        if (!this->_consensus)
          this->_consensus = elle::make_unique<Consensus>(*this);
      }

      Doughnut::Doughnut(cryptography::rsa::KeyPair keys,
                         std::unique_ptr<overlay::Overlay> overlay,
                         std::unique_ptr<Consensus> consensus,
                         bool plain)
        : Doughnut(std::move(overlay), std::move(consensus), plain)
      {
        this->_keys.emplace(std::move(keys));
      }

      Doughnut::Doughnut(std::string name,
                         cryptography::rsa::KeyPair keys,
                         std::unique_ptr<overlay::Overlay> overlay,
                         std::unique_ptr<Consensus> consensus,
                         bool plain)
        : Doughnut(std::move(keys),
                   std::move(overlay),
                   std::move(consensus),
                   plain)
      {
        try
        {
          auto block = this->fetch(UB::hash_address(this->keys().K()));
          ELLE_DEBUG("%s: user reverse block already present", *this);
          auto ub = elle::cast<UB>::runtime(block);
          if (ub->name() != name)
            throw elle::Error(
              elle::sprintf("This key is already associated with name %s",
                            ub->name()));
        }
        catch(MissingBlock const&)
        {
          ELLE_TRACE_SCOPE("%s: store reverse user block", *this);
          UB user(std::move(name), this->keys().K(), true);
          this->store(user);
        }
        try
        {
          auto block = this->fetch(UB::hash_address(name));
          ELLE_DEBUG("%s: user block already present", *this);
          auto ub = elle::cast<UB>::runtime(block);
          if (ub->key() != this->keys().K())
            throw elle::Error(
              elle::sprintf("user block exists at %s(%x) with different key",
                            name, UB::hash_address(name)));
        }
        catch (MissingBlock const&)
        {
          ELLE_TRACE_SCOPE("%s: store user block", *this);
          UB user(std::move(name), this->keys().K());
          this->store(user);
        }
      }

      infinit::cryptography::rsa::KeyPair&
      Doughnut::keys()
      {
        if (!this->_keys)
          throw elle::Error(elle::sprintf("%s is key-less", *this));
        return this->_keys.get();
      }

      std::unique_ptr<blocks::MutableBlock>
      Doughnut::_make_mutable_block() const
      {
        ELLE_TRACE_SCOPE("%s: create OKB", *this);
        if (_plain)
           return elle::make_unique<PlainMutableBlock>();
        auto res = elle::make_unique<OKB>(const_cast<Doughnut*>(this));
        return std::move(res);
      }

      std::unique_ptr<blocks::ImmutableBlock>
      Doughnut::_make_immutable_block(elle::Buffer content) const
      {
        ELLE_TRACE_SCOPE("%s: create CHB", *this);
        if (_plain)
          return elle::make_unique<PlainImmutableBlock>(std::move(content));
        else
          return elle::make_unique<CHB>(std::move(content));
      }

      std::unique_ptr<blocks::ACLBlock>
      Doughnut::_make_acl_block() const
      {
        ELLE_TRACE_SCOPE("%s: create ACB", *this);
        if (_plain)
          return elle::make_unique<PlainACLBlock>();
        else
          return elle::make_unique<ACB>(const_cast<Doughnut*>(this));
      }

      std::unique_ptr<model::User>
      Doughnut::_make_user(elle::Buffer const& data) const
      {
        if (data.size() == 0)
          throw elle::Error("invalid empty user");
        if (data[0] == '{')
        {
          ELLE_TRACE_SCOPE("%s: fetch user from public key", *this);
          elle::IOStream input(data.istreambuf());
          elle::serialization::json::SerializerIn s(input);
          cryptography::rsa::PublicKey pub(s);
          try
          {
            auto block = this->fetch(UB::hash_address(pub));
            auto ub = elle::cast<UB>::runtime(block);
            return elle::make_unique<doughnut::User>
              (ub->key(), ub->name());
          }
          catch (MissingBlock const&)
          {
            ELLE_TRACE("Reverse UB not found, returning no name");
            return elle::make_unique<doughnut::User>(pub, "");
          }
        }
        else
        {
          ELLE_TRACE_SCOPE("%s: fetch user from name", *this);
          auto block = this->fetch(UB::hash_address(data.string()));
          auto ub = elle::cast<UB>::runtime(block);
          return elle::make_unique<doughnut::User>
            (ub->key(), data.string());
        }
      }

      void
      Doughnut::_store(blocks::Block& block, StoreMode mode)
      {
        this->_consensus->store(*this->_overlay, block, mode);
      }

      std::unique_ptr<blocks::Block>
      Doughnut::_fetch(Address address) const
      {
        std::unique_ptr<blocks::Block> res;
        try
        {
          return this->_consensus->fetch(*this->_overlay, address);
        }
        catch (infinit::storage::MissingKey const&)
        {
          return nullptr;
        }
      }

      void
      Doughnut::_remove(Address address)
      {
        this->_consensus->remove(*this->_overlay, address);
      }

      struct DoughnutModelConfig:
        public ModelConfig
      {
      public:
        std::unique_ptr<infinit::overlay::OverlayConfig> overlay;
        std::unique_ptr<infinit::cryptography::rsa::KeyPair> key;
        boost::optional<bool> plain;
        boost::optional<std::string> name;

        DoughnutModelConfig(elle::serialization::SerializerIn& input)
          : ModelConfig()
        {
          this->serialize(input);
        }

        void
        serialize(elle::serialization::Serializer& s)
        {
          s.serialize("overlay", this->overlay);
          s.serialize("keys", this->key);
          s.serialize("plain", this->plain);
          s.serialize("name", this->name);
        }

        virtual
        std::unique_ptr<infinit::model::Model>
        make()
        {
          if (!key)
            return elle::make_unique<infinit::model::doughnut::Doughnut>(
              overlay->make(),
              nullptr,
              plain && *plain);
          else if (!name)
            return elle::make_unique<infinit::model::doughnut::Doughnut>(
              std::move(*key),
              overlay->make(),
              nullptr,
              plain && *plain);
          else
            return elle::make_unique<infinit::model::doughnut::Doughnut>(
              std::move(this->name.get()),
              std::move(*key),
              overlay->make(),
              nullptr,
              plain && *plain);
        }
      };

      static const elle::serialization::Hierarchy<ModelConfig>::
      Register<DoughnutModelConfig> _register_DoughnutModelConfig("doughnut");
    }
  }
}
