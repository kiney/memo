namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      struct OKBContent
      {
      // Construction
      public:
        OKBContent(cryptography::KeyPair const& keys)
          : _key()
          , _owner(keys.K())
          , _version(-1)
          , _signature()
        {
          auto block_keys = cryptography::KeyPair::generate
            (cryptography::Cryptosystem::rsa, 2048);
          this->_owner._signature = block_keys.k().sign(keys.K());
          this->_key.~PublicKey();
          new (&this->_key) cryptography::PublicKey(block_keys.K());
        }

        OKBContent()
          : _key()
          , _owner()
          , _version()
          , _signature()
        {}

      // Content
      public:
        struct Owner
        {
          Owner(cryptography::PublicKey key)
            : _key(std::move(key))
            , _signature()
          {}

          Owner()
            : _key()
            , _signature()
          {}

          ELLE_ATTRIBUTE_R(cryptography::PublicKey, key);
          ELLE_ATTRIBUTE_R(cryptography::Signature, signature);
          friend class OKBContent;

          void
          serialize(elle::serialization::Serializer& input)
          {
            input.serialize("key", this->_key);
            input.serialize("signature", this->_signature);
          }
        };
        ELLE_ATTRIBUTE_R(cryptography::PublicKey, key);
        ELLE_ATTRIBUTE_R(Owner, owner);
        ELLE_ATTRIBUTE_R(int, version);
        ELLE_ATTRIBUTE_R(cryptography::Signature, signature);
        friend class OKB;
      };

      class OKB
        : public OKBContent
        , public blocks::MutableBlock
      {
      // Types
      public:
        typedef OKB Self;
        typedef blocks::MutableBlock Super;


      // Construction
      public:
        OKB(cryptography::KeyPair const& keys)
          : OKBContent(keys)
          , Super(OKB::_hash_address(this->_key))
          , _keys(keys)
        {}

      // Validation
      protected:
        elle::Buffer
        _sign() const
        {
          elle::Buffer res;
          {
            // FIXME: use binary to sign
            elle::IOStream s(res.ostreambuf());
            elle::serialization::json::SerializerOut output(s);
            output.serialize("block_key", this->_key);
            output.serialize("data", this->data());
            output.serialize("version", this->_version);
          }
          return res;
        }

        virtual
        void
        _seal() override
        {
          ELLE_DEBUG_SCOPE("%s: seal", *this);
          ++this->_version; // FIXME: idempotence in case the write fails ?
          auto sign = this->_sign();
          this->_signature = this->_keys.k().sign(cryptography::Plain(sign));
        }

        virtual
        bool
        _validate(blocks::Block const& previous) const override
        {
          if (!this->_validate())
            return false;
          auto previous_okb = dynamic_cast<OKB const*>(&previous);
          return previous_okb && this->version() > previous_okb->version();
        }

        virtual
        bool
        _validate() const override
        {
          ELLE_DEBUG_SCOPE("%s: validate", *this);
          auto expected_address = OKB::_hash_address(this->_key);
          if (this->address() != expected_address)
          {
            ELLE_DUMP("%s: address %x invalid, expecting %x",
                      *this, this->address(), expected_address);
            return false;
          }
          ELLE_DUMP("%s: address is valid", *this);
          if (!this->_key.verify(this->_owner.signature(), this->_owner.key()))
            return false;
          ELLE_DUMP("%s: owner key is valid", *this);
          auto sign = this->_sign();
          if (!this->_owner.key().verify(this->_signature,
                                         cryptography::Plain(sign)))
            return false;
          ELLE_DUMP("%s: payload is valid", *this);
          return true;
        }

      private:
        static
        Address
        _hash_address(cryptography::PublicKey const& key)
        {
          auto hash = cryptography::oneway::hash
            (key, cryptography::oneway::Algorithm::sha256);
          return Address(hash.buffer().contents());
        }
        ELLE_ATTRIBUTE_R(cryptography::KeyPair, keys);

      // Serialization
      public:
        OKB(elle::serialization::Serializer& input)
          : OKBContent()
          , Super(input)
        {
          this->_serialize(input);
        }

        virtual
        void
        serialize(elle::serialization::Serializer& s) override
        {
          this->Super::serialize(s);
          this->_serialize(s);
        }

      private:
        void
        _serialize(elle::serialization::Serializer& input)
        {
          input.serialize("key", this->_key);
          input.serialize("owner", this->_owner);
          input.serialize("version", this->_version);
          input.serialize("signature", this->_signature);
        }
      };
      static const elle::serialization::Hierarchy<blocks::Block>::
      Register<OKB> _register_okb_serialization("OKB");
    }
  }
}
