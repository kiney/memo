#include <infinit/filesystem/Node.hh>
#include <infinit/filesystem/Directory.hh>
#include <infinit/filesystem/umbrella.hh>

#include <infinit/model/blocks/ACLBlock.hh>
#include <infinit/model/doughnut/NB.hh>
#include <infinit/model/doughnut/ACB.hh>
#include <infinit/model/doughnut/Doughnut.hh>

#include <memory>

#include <sys/stat.h> // S_IMFT...

#ifdef INFINIT_WINDOWS
#undef stat
#endif

ELLE_LOG_COMPONENT("infinit.filesystem.Node");

namespace infinit
{
  namespace filesystem
  {
    class ACLConflictResolver: public model::ConflictResolver
    {
    public:
      ACLConflictResolver(model::Model* model,
                          bool r,
                          bool w,
                          std::string const& key)
      : _model(model)
      , _read(r)
      , _write(w)
      , _userkey(key)
      {}
      ACLConflictResolver(elle::serialization::SerializerIn& s)
      {
        serialize(s);
      }
      void serialize(elle::serialization::Serializer& s) override
      {
        s.serialize("read", _read);
        s.serialize("write", _write);
        s.serialize("userkey", _userkey);
        if (s.in())
        {
          infinit::model::Model* model = nullptr;
          const_cast<elle::serialization::Context&>(s.context()).get(model);
          ELLE_ASSERT(model);
          _model = model;
        }
      }
      std::unique_ptr<Block>
      operator() (Block& block,
                  Block& current,
                  model::StoreMode mode)
      {
        ELLE_TRACE("ACLConflictResolver: replaying set_permissions on new block.");
        std::unique_ptr<model::User> user = _model->make_user(
          elle::Buffer(_userkey.data(), _userkey.size()));
        auto& acl = dynamic_cast<model::blocks::ACLBlock&>(current);
        // Force a change
        acl.set_permissions(*user, !_read, !_write);
        acl.set_permissions(*user, _read, _write);
        return current.clone();
      }
      model::Model* _model;
      bool _read;
      bool _write;
      std::string _userkey;
    };
    static const elle::serialization::Hierarchy<model::ConflictResolver>::
    Register<ACLConflictResolver> _register_dcr("acl");

    void
    Node::rename(boost::filesystem::path const& where)
    {
      boost::filesystem::path current = full_path();
      std::string newname = where.filename().string();
      boost::filesystem::path newpath = where.parent_path();
      if (!_parent)
        throw rfs::Error(EINVAL, "Cannot delete root node");
      auto dir = std::dynamic_pointer_cast<Directory>(
        _owner.filesystem()->path(newpath.string()));
      dir->_fetch();
      if (dir->_files.find(newname) != dir->_files.end())
      {
        ELLE_TRACE_SCOPE("%s: remove existing destination", *this);
        // File and empty dir gets removed.
        auto target = _owner.filesystem()->path(where.string());
        struct stat st;
        target->stat(&st);
        if (S_ISDIR(st.st_mode))
        {
          try
          {
            target->rmdir();
          }
          catch(rfs::Error const& e)
          {
            throw rfs::Error(EISDIR, "Target is a directory");
          }
        }
        else
          target->unlink();
        ELLE_DEBUG("removed move target %s", where);
      }
      auto data = _parent->_files.at(_name);
      _parent->_files.erase(_name);
      _parent->_commit({OperationType::remove, _name});
      dir->_files.insert(std::make_pair(newname, data));
      dir->_commit({OperationType::insert, newname, data.first, data.second});
      _name = newname;
      _parent = dir;
      // Move the node in cache
      ELLE_DEBUG("Extracting %s", current);
      auto p = _owner.filesystem()->extract(current.string());
      if (p)
      {
        std::dynamic_pointer_cast<Node>(p)->_name = newname;
        // This might delete the dummy Unknown on destination which is fine
        ELLE_DEBUG("Setting %s", where);
        _owner.filesystem()->set(where.string(), p);
      }
    }

    void
    Node::_remove_from_cache(boost::filesystem::path full_path)
    {
      if (full_path == boost::filesystem::path())
        full_path = this->full_path();
      std::shared_ptr<rfs::Path> self = _owner.filesystem()->extract(full_path.string());
      ELLE_TRACE("remove_from_cache: %s released (%s), this=%s, path=%s", _name, self, this, full_path);
      new reactor::Thread("delayed_cleanup", [self] { ELLE_DEBUG("async_clean");}, true);
    }

    boost::filesystem::path
    Node::full_path()
    {
      if (_parent == nullptr)
        return "/";
      return _parent->full_path() / _name;
    }

    void
    Node::chmod(mode_t mode)
    {
      _fetch();
      _header.mode = mode;
      _header.ctime = time(nullptr);
      _commit();
    }

    void
    Node::chown(int uid, int gid)
    {
      _fetch();
      _header.uid = uid;
      _header.gid = gid;
      _header.ctime = time(nullptr);
      _commit();
    }

    void
    Node::removexattr(std::string const& k)
    {
      ELLE_LOG_COMPONENT("infinit.filesystem.Node.xattr");
      ELLE_TRACE_SCOPE("%s: remove attribute \"%s\"", *this, k);
      _fetch();
      if (_header.xattrs.erase(k))
      {
         _header.ctime = time(nullptr);
        _commit();
      }
      else
        ELLE_TRACE_SCOPE("no such attribute");
    }

    static auto const overlay_info = "user.infinit.overlay.";

    void
    Node::setxattr(std::string const& k, std::string const& v, int flags)
    {
      ELLE_LOG_COMPONENT("infinit.filesystem.Node.xattr");
      ELLE_TRACE_SCOPE("%s: set attribute \"%s\"", *this, k);
      ELLE_DUMP("value: %s", elle::ConstWeakBuffer(v));
      /* Drop quarantine flags, preventing the files from being opened.
      * https://github.com/osxfuse/osxfuse/issues/162
      */
      if (k == "com.apple.quarantine")
        return;
      if (k.substr(0, strlen(overlay_info)) == overlay_info)
      {
        std::string okey = k.substr(strlen(overlay_info));
        umbrella([&] {
          dynamic_cast<model::doughnut::Doughnut*>(_owner.block_store().get())
            ->overlay()->query(okey, v);
        }, EINVAL);
        return;
      }
      _fetch();
      _header.xattrs[k] = elle::Buffer(v.data(), v.size());
      _header.ctime = time(nullptr);
      _commit();
    }

    std::string
    Node::getxattr(std::string const& k)
    {
      ELLE_LOG_COMPONENT("infinit.filesystem.Node.xattr");
      ELLE_TRACE_SCOPE("%s: get attribute \"%s\"", *this, k);
      if (k.substr(0, strlen(overlay_info)) == overlay_info)
      {
        std::string okey = k.substr(strlen(overlay_info));
        elle::json::Json v = umbrella([&] {
          return dynamic_cast<model::doughnut::Doughnut*>(_owner.block_store().get())
            ->overlay()->query(okey, {});
        }, EINVAL);
        if (v.empty())
          return "{}";
        else
          return elle::json::pretty_print(v);
      }
      _fetch();
      auto it = _header.xattrs.find(k);
      if (it == _header.xattrs.end())
      {
        ELLE_DEBUG("no such attribute");
        throw rfs::Error(ENOATTR, "No attribute", elle::Backtrace());
      }
      std::string value = it->second.string();
      ELLE_DUMP("value: %s", elle::ConstWeakBuffer(value));
      return value;
    }

    void
    Node::stat(struct stat* st)
    {
      memset(st, 0, sizeof(struct stat));
      #ifndef INFINIT_WINDOWS
      st->st_blksize = 16384;
      st->st_blocks = _header.size / 512;
      #endif
      st->st_mode  = 0600;
      st->st_size  = this->_header.size;
      st->st_atime = this->_header.atime;
      st->st_mtime = this->_header.mtime;
      st->st_ctime = this->_header.ctime;
      st->st_nlink = this->_header.links;
      st->st_uid   = getuid();
      st->st_gid   = getgid();
      st->st_dev = 1;
      auto block = _header_block();
      std::pair<bool, bool> perms = _owner.get_permissions(*block);
      if (!perms.first)
        st->st_mode &= ~0400;
      if (!perms.second)
        st->st_mode &= ~0200;
      st->st_ino = (unsigned short)(uint64_t)(void*)this;
    }

    void
    Node::utimens(const struct timespec tv[2])
    {
      ELLE_TRACE_SCOPE("%s: utimens: %s", *this, tv);
      _fetch();
      _header.atime = tv[0].tv_sec;
      _header.mtime = tv[1].tv_sec;
      _header.ctime = time(nullptr);
      _commit();
    }

    std::unique_ptr<infinit::model::User>
    Node::_get_user(std::string const& value)
    {
      if (value.empty())
        THROW_INVAL;
      ELLE_TRACE("setxattr raw key");
      elle::Buffer userkey = elle::Buffer(value.data(), value.size());
      auto user = _owner.block_store()->make_user(userkey);
      return std::move(user);
    }

    static std::pair<bool, bool> parse_flags(std::string const& flags)
    {
      bool r = false;
      bool w = false;
      if (flags == "clear")
        ;
      else if (flags == "setr")
        r = true;
      else if (flags == "setw")
        w = true;
      else if (flags == "setrw")
      {
        r = true; w = true;
      }
      else
        THROW_NODATA;
      return std::make_pair(r, w);
    }

    void
    Node::set_permissions(std::string const& flags,
                          std::string const& userkey,
                          Address self_address)
    {
      std::pair<bool, bool> perms = parse_flags(flags);
      std::unique_ptr<infinit::model::User> user =
        umbrella([&] {return _get_user(userkey);}, EINVAL);
      if (!user)
      {
        ELLE_WARN("user %s does not exist", userkey);
        THROW_INVAL;
      }
      auto acl = std::dynamic_pointer_cast<model::blocks::ACLBlock>(
        this->_owner.fetch_or_die(self_address));
      if (!acl)
        throw rfs::Error(EIO, "Block is not an ACL block");
      // permission check
      auto acb = dynamic_cast<model::doughnut::ACB*>(acl.get());
      if (!acb)
        throw rfs::Error(EIO,
          elle::sprintf("Block is not an ACB block: %s", typeid(*acl).name()));
      auto dn =
        std::dynamic_pointer_cast<model::doughnut::Doughnut>(_owner.block_store());
      auto keys = dn->keys();
      if (keys.K() != *acb->owner_key())
        THROW_ACCES;
      ELLE_TRACE("Setting permission at %s for %s", acl->address(), user->name());
      umbrella([&] {acl->set_permissions(*user, perms.first, perms.second);},
        EACCES);
      _owner.store_or_die(
        std::move(acl),
        model::STORE_UPDATE,
        elle::make_unique<ACLConflictResolver>(
          _owner.block_store().get(), perms.first, perms.second, userkey
        ));
    }
  }
}
