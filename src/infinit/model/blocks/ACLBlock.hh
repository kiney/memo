#ifndef INFINIT_MODEL_BLOCKS_ACL_BLOCK_HH
# define INFINIT_MODEL_BLOCKS_ACL_BLOCK_HH

# include <infinit/model/User.hh>
# include <infinit/model/blocks/MutableBlock.hh>

namespace infinit
{
  namespace model
  {
    namespace blocks
    {
      class ACLBlock
        : public MutableBlock
      {
      /*------.
      | Types |
      `------*/
      public:
        typedef ACLBlock Self;
        typedef MutableBlock Super;

      /*-------------.
      | Construction |
      `-------------*/
      protected:
        ACLBlock(Address address);
        ACLBlock(Address address, elle::Buffer data);
        friend class infinit::model::Model;

      /*------------.
      | Permissions |
      `------------*/
      public:
        void
        set_permissions(User const& user,
                        bool read,
                        bool write);
        void
        copy_permissions(ACLBlock& to);

        struct Entry
        {
          Entry() {}
          Entry(std::unique_ptr<User> u, bool r, bool w)
          : user(std::move(u)), read(r), write(w) {}
          Entry(Entry&& b) = default;

          std::unique_ptr<User> user;
          bool read;
          bool write;
        };

        std::vector<Entry>
        list_permissions();

      protected:
        virtual
        void
        _set_permissions(User const& user,
                         bool read,
                         bool write);
        virtual
        void
        _copy_permissions(ACLBlock& to);

        virtual
        std::vector<Entry>
        _list_permissions();

      /*--------------.
      | Serialization |
      `--------------*/
      public:
        ACLBlock(elle::serialization::Serializer& input);
        virtual
        void
        serialize(elle::serialization::Serializer& s) override;
      private:
        void
        _serialize(elle::serialization::Serializer& input);
      };
    }
  }
}

#endif
