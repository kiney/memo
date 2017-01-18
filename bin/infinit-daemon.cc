// http://opensource.apple.com/source/mDNSResponder/mDNSResponder-576.30.4/mDNSPosix/PosixDaemon.c
#if __APPLE__
# define daemon yes_we_know_that_daemon_is_deprecated_in_os_x_10_5_thankyou
#endif

#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>

#ifdef INFINIT_LINUX
# include <mntent.h>
#endif

#ifdef INFINIT_MACOSX
# include <sys/param.h>
# include <sys/mount.h>
#endif

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <elle/log.hh>
#include <elle/make-vector.hh>
#include <elle/system/PIDFile.hh>
#include <elle/system/Process.hh>
#include <elle/system/unistd.hh>
#include <elle/serialization/json.hh>
#include <elle/system/self-path.hh>

#include <reactor/network/http-server.hh>
#include <reactor/network/unix-domain-server.hh>
#include <reactor/network/unix-domain-socket.hh>

#include <infinit/utility.hh>

#include <infinit/storage/Filesystem.hh>

ELLE_LOG_COMPONENT("infinit-daemon");

#include <main.hh>
#include <password.hh>

#if __APPLE__
# undef daemon
extern "C" int daemon(int, int);
#endif

namespace bfs = boost::filesystem;

namespace
{
  infinit::Infinit ifnt;

  struct SystemUser
  {
    SystemUser(unsigned int uid,
               unsigned int gid,
               std::string name,
               std::string home)
      : uid(uid)
      , gid(gid)
      , name(name)
      , home(home)
    {}

    SystemUser(unsigned int uid, boost::optional<std::string> home_ = {})
      : uid(uid)
      , gid(0)
    {
      passwd* pwd = getpwuid(uid);
      if (!pwd)
        elle::err("No user found with uid %s", uid);
      name = pwd->pw_name;
      home = home_.value_or(pwd->pw_dir);
      gid = pwd->pw_gid;
    }

    SystemUser(std::string const& name_, boost::optional<std::string> home_ = {})
    {
      passwd* pwd = getpwnam(name_.c_str());
      if (!pwd)
        elle::err("No user found with name %s", name_);
      name = pwd->pw_name;
      home = home_.value_or(pwd->pw_dir);
      uid = pwd->pw_uid;
      gid = pwd->pw_gid;
    }
    unsigned int uid;
    unsigned int gid;
    std::string name;
    std::string home;

    struct Lock
      : public reactor::Lock
    {
      Lock(SystemUser const& su, reactor::Lockable& l)
        : reactor::Lock(l)
      {
        prev_home = elle::os::getenv("INFINIT_HOME", "");
        prev_data_home = elle::os::getenv("INFINIT_DATA_HOME", "");
        elle::os::setenv("INFINIT_HOME", su.home, 1);
        if (!elle::os::getenv("INFINIT_HOME_OVERRIDE", "").empty())
          elle::os::setenv("INFINIT_HOME",
            elle::os::getenv("INFINIT_HOME_OVERRIDE", ""), 1);
        elle::os::unsetenv("INFINIT_DATA_HOME");
        prev_euid = geteuid();
        prev_egid = getegid();
        elle::setegid(su.gid);
        elle::seteuid(su.uid);
      }

      Lock(Lock const& b) = delete;

      Lock(Lock && b) = default;

      ~Lock()
      {
        if (prev_home.empty())
          elle::os::unsetenv("INFINIT_HOME");
        else
          elle::os::setenv("INFINIT_HOME", prev_home, 1);
        if (!prev_data_home.empty())
          elle::os::setenv("INFINIT_DATA_HOME", prev_data_home, 1);
        elle::seteuid(prev_euid);
        elle::setegid(prev_egid);
      }

      std::string prev_home, prev_data_home;
      int prev_euid, prev_egid;
    };

    Lock
    enter(reactor::Lockable& l) const
    {
      return {*this, l};
    }
  };

  std::pair<std::string, infinit::User>
  split(std::string const& name)
  {
    auto p = name.find_first_of('/');
    if (p == name.npos)
      elle::err("Malformed qualified name");
    return {name.substr(p+1), ifnt.user_get(name.substr(0, p))};
  }
}

// Not in anonymous namespace because it overloads another `optional`.
static
boost::optional<std::string>
optional(elle::json::Object const& options, std::string const& name)
{
  auto it = options.find(name);
  if (it == options.end())
    return {};
  else
    return boost::any_cast<std::string>(it->second);
}


void link_network(std::string const& name,
                  elle::json::Object const& options = elle::json::Object{})
{
  auto cname = split(name);
  auto desc = ifnt.network_descriptor_get(cname.first, cname.second, false);
  auto users = ifnt.users_get();
  auto passport = boost::optional<infinit::Passport>{};
  auto user = boost::optional<infinit::User>{};
  ELLE_TRACE("checking if any user is owner");
  for (auto const& u: users)
    if (u.public_key == desc.owner && u.private_key)
    {
      passport.emplace(u.public_key, desc.name,
        infinit::cryptography::rsa::KeyPair(u.public_key,
                                            u.private_key.get()));
      user.emplace(u);
      break;
    }
  if (!passport)
  {
    ELLE_TRACE("Trying to acquire passport");
    for (auto const& u: users)
    {
      try
      {
        passport.emplace(ifnt.passport_get(name, u.name));
        user.emplace(u);
        break;
      }
      catch (infinit::MissingLocalResource const&)
      {
        try
        {
          passport.emplace(infinit::beyond_fetch<infinit::Passport>(elle::sprintf(
            "networks/%s/passports/%s", name, u.name),
              "passport for",
              name,
              u));
          user.emplace(u);
          break;
        }
        catch (elle::Error const&)
        {}
      }
    }
  }
  if (!passport)
    elle::err("Failed to acquire passport.");
  ELLE_TRACE("Passport found for user %s", user->name);

  auto storage_config = [&] () -> std::unique_ptr<infinit::storage::StorageConfig> {
    auto storagedesc = optional(options, "storage");
    if (storagedesc && storagedesc->empty())
    {
      auto storagename = boost::replace_all_copy(name + "_storage", "/", "_");
      ELLE_LOG("Creating local storage %s", storagename);
      auto path = infinit::xdg_data_home() / "blocks" / storagename;
      return
        std::make_unique<infinit::storage::FilesystemStorageConfig>(
          storagename, path.string(), boost::none, boost::none);
    }
    else if (storagedesc)
    {
      try
      {
        return ifnt.storage_get(*storagedesc);
      }
      catch (infinit::MissingLocalResource const&)
      {
        elle::err("storage specification for new storage not implemented");
      }
    }
    else
      return nullptr;
  }();

  auto network = infinit::Network(
    desc.name,
    std::make_unique<infinit::model::doughnut::Configuration>(
      infinit::model::Address::random(0), // FIXME
      std::move(desc.consensus),
      std::move(desc.overlay),
      std::move(storage_config),
      user->keypair(),
      std::make_shared<infinit::cryptography::rsa::PublicKey>(desc.owner),
      std::move(*passport),
      user->name,
      boost::optional<int>(),
      desc.version,
      desc.admin_keys,
      std::vector<infinit::model::Endpoints>()),
    boost::none);
  ifnt.network_save(*user, network, true);
  ifnt.network_save(std::move(network), true);
}

void
acquire_network(std::string const& name)
{
  auto desc = infinit::beyond_fetch<infinit::NetworkDescriptor>("network", name);
  ifnt.network_save(desc);
  try
  {
    auto nname = split(name);
    auto net = ifnt.network_get(nname.first, nname.second, true);
  }
  catch (elle::Error const&)
  {
    link_network(name);
  }
}

void
acquire_volume(std::string const& name)
{
  auto desc = infinit::beyond_fetch<infinit::Volume>("volume", name);
  ifnt.volume_save(desc, true);
  try
  {
    auto nname = split(desc.network);
    auto net = ifnt.network_get(nname.first, nname.second, true);
  }
  catch (infinit::MissingLocalResource const&)
  {
    acquire_network(desc.network);
  }
  catch (elle::Error const&)
  {
    link_network(desc.network);
  }
}

struct Mount
{
  std::unique_ptr<elle::system::Process> process;
  infinit::MountOptions options;
};

struct MountInfo
  : elle::Printable
{
  MountInfo(std::string n,
            bool l,
            boost::optional<std::string> m)
    : name(std::move(n))
    , live(l)
    , mountpoint(std::move(m))
  {}
  MountInfo() = default;

  void
  serialize(elle::serialization::Serializer& s)
  {
    s.serialize("name", name);
    s.serialize("live", live);
    s.serialize("mountpoint", mountpoint);
  }

  void
  print(std::ostream& out) const override
  {
    out << name << ": "
        << (live && mountpoint ? "mounted"
            : mountpoint ? "crashed" : "not running");
    if (mountpoint)
      out << ": " << *mountpoint;
  }

  std::string name;
  bool live;
  boost::optional<std::string> mountpoint;
};

class MountManager
{
public:
  MountManager(bfs::path mount_root = bfs::temp_directory_path(),
               std::string mount_substitute = "")
    : _mount_root(mount_root)
    , _mount_substitute(mount_substitute)
    , _fetch(false)
    , _push(false)
  {}
  ~MountManager();
  void
  start(std::string const& name, infinit::MountOptions opts = {},
        bool force_mount = false,
        bool wait_for_mount = false,
        std::vector<std::string> const& extra_args = {});
  void
  stop(std::string const& name);
  void
  status(std::string const& name, elle::serialization::SerializerOut& reply);
  bool
  exists(std::string const& name);
  std::string
  mountpoint(std::string const& name, bool ignore_subst=false);
  std::vector<infinit::descriptor::BaseDescriptor::Name>
  list();
  std::vector<MountInfo>
  status();
  void
  create_volume(std::string const& name,
                elle::json::Object const& args);
  void
  delete_volume(std::string const& name);
  infinit::Network
  create_network(elle::json::Object const& options,
                 infinit::User const& owner);
  void
  update_network(infinit::Network& network,
                 elle::json::Object const& options);
  void
  acquire_volumes();
  ELLE_ATTRIBUTE_RW(bfs::path, mount_root);
  ELLE_ATTRIBUTE_RW(std::string, mount_substitute);
  ELLE_ATTRIBUTE_RW(boost::optional<std::string>, log_level);
  ELLE_ATTRIBUTE_RW(boost::optional<std::string>, log_path);
  ELLE_ATTRIBUTE_RW(std::string, default_user);
  ELLE_ATTRIBUTE_RW(boost::optional<std::string>, default_network);
  ELLE_ATTRIBUTE_RW(std::vector<std::string>, advertise_host);
  ELLE_ATTRIBUTE_RW(bool, fetch);
  ELLE_ATTRIBUTE_RW(bool, push);
private:
  std::unordered_map<std::string, Mount> _mounts;
};

MountManager::~MountManager()
{
  while (!this->_mounts.empty())
    this->stop(this->_mounts.begin()->first);
}

void
MountManager::acquire_volumes()
{
  if (this->_fetch)
    for (auto const& u: ifnt.users_get())
      if (u.private_key)
        try
        {
          auto res = infinit::beyond_fetch<
            std::unordered_map<std::string, std::vector<infinit::Volume>>>(
              elle::sprintf("users/%s/volumes", u.name),
              "volumes for user",
              u.name,
              u);
          for (auto const& volume: res["volumes"])
          {
            try
            {
              acquire_volume(volume.name);
            }
            catch (infinit::ResourceAlreadyFetched const& error)
            {
            }
            catch (elle::Error const& e)
            {
              ELLE_WARN("failed to acquire %s: %s", volume.name, e);
            }
          }
        }
        catch (elle::Error const& e)
        {
          ELLE_WARN("failed to acquire volumes from beyond: %s", e);
        }
  if (this->_default_network)
  {
    ELLE_TRACE_SCOPE("%s: list volumes from network %s",
                     this, this->_default_network.get());
    auto owner = ifnt.user_get(this->default_user());
    auto net = ifnt.network_get(this->_default_network.get(), owner, true);
    auto process = [&]
    {
      static const auto root = elle::system::self_path().parent_path();
      auto args = std::vector<std::string>{
        (root / "infinit-volume").string(),
        "--fetch",
        "--network",
        net.name,
        "--service",
        "--as",
        this->default_user(),
      };
      infinit::MountOptions mo;
      if (this->_fetch)
        mo.fetch = true;
      if (this->_push)
        mo.push = true;
      auto env = elle::os::Environ{};
      mo.to_commandline(args, env);
      return std::make_unique<elle::system::Process>(args, true);
    }();
    process->wait();
  }
}

std::vector<infinit::descriptor::BaseDescriptor::Name>
MountManager::list()
{
  this->acquire_volumes();
  return elle::make_vector(ifnt.volumes_get(),
                           [](auto&& v) { return v.name; });
}

std::string
MountManager::mountpoint(std::string const& name, bool raw)
{
  auto it = _mounts.find(name);
  if (it == _mounts.end())
    elle::err("not mounted: %s", name);
  if (!it->second.options.mountpoint)
    elle::err("running without mountpoint: %s", name);
  std::string pre = it->second.options.mountpoint.get();
  if (!raw && !this->_mount_substitute.empty())
  {
    auto sep = this->_mount_substitute.find(":");
    if (sep == std::string::npos)
      pre = this->_mount_substitute + pre;
    else
    {
      std::string search = this->_mount_substitute.substr(0, sep);
      std::string repl = this->_mount_substitute.substr(sep+1);
      auto pos = pre.find(search);
      if (pos != pre.npos)
      {
        pre = pre.substr(0, pos) + repl + pre.substr(pos + search.size());
      }
    }
  }
  ELLE_TRACE("replying with mountpoint %s", pre);
  return pre;
}

bool
MountManager::exists(std::string const& name)
{
  try
  {
    auto volume = ifnt.volume_get(name);
    return true;
  }
  catch (elle::Error const& e)
  {
    return false;
  }
}

bool
is_mounted(std::string const& path)
{
#ifdef INFINIT_LINUX
  FILE* mtab = setmntent("/etc/mtab", "r");
  struct mntent mnt;
  char strings[4096];
  bool found = false;
  while (getmntent_r(mtab, &mnt, strings, sizeof(strings)))
  {
    if (std::string(mnt.mnt_dir) == path)
    {
      found = true;
      break;
    }
  }
  endmntent(mtab);
  return found;
#elif defined(INFINIT_WINDOWS)
  // We mount as drive letters under windows
  return bfs::exists(path);
#elif defined(INFINIT_MACOSX)
  struct statfs sfs;
  int res = statfs(path.c_str(), &sfs);
  if (res)
    return false;
  return bfs::path(path) == bfs::path(sfs.f_mntonname);
#else
  throw elle::Error("is_mounted is not implemented");
#endif
}

void
MountManager::start(std::string const& name,
                    infinit::MountOptions opts,
                    bool force_mount,
                    bool wait_for_mount,
                    std::vector<std::string> const& extra_args)
{
  infinit::Volume volume = [&]
  {
    try
    {
      return ifnt.volume_get(name);
    }
    catch (infinit::MissingLocalResource const&)
    {
      acquire_volume(name);
      return ifnt.volume_get(name);
    }
  }();
  if (this->_mounts.count(volume.name))
    elle::err("already mounted: %s", volume.name);
  try
  {
    infinit::User u = [&] {
      if (volume.mount_options.as)
        return ifnt.user_get(*volume.mount_options.as);
      else
        return ifnt.user_get(volume.network.substr(0, volume.network.find_first_of('/')));
    }();
    auto net = ifnt.network_get(volume.network, u, true);
  }
  catch (infinit::MissingLocalResource const&)
  {
    acquire_network(volume.network);
  }
  catch (elle::Error const&)
  {
    link_network(volume.network);
  }
  volume.mount_options.merge(opts);
  if (!volume.mount_options.fuse_options)
    volume.mount_options.fuse_options = std::vector<std::string>{"allow_root"};
  else
    volume.mount_options.fuse_options->push_back("allow_root");
  Mount m{nullptr, volume.mount_options};
  std::string mount_prefix(name + "-");
  boost::replace_all(mount_prefix, "/", "_");
  if (force_mount && !m.options.mountpoint)
  {
    auto username = elle::system::username();
    boost::system::error_code erc;
    bfs::permissions(this->_mount_root,
                     bfs::remove_perms
                     | bfs::others_read
                     | bfs::others_exe
                     | bfs::others_write
                     );
    m.options.mountpoint =
    (this->_mount_root /
      (mount_prefix + bfs::unique_path().string())).string();
  }
  if (this->_fetch)
    m.options.fetch = true;
  if (this->_push)
    m.options.push = true;
  static const auto root = elle::system::self_path().parent_path();
  auto args = std::vector<std::string>{
    (root / "infinit-volume").string(),
    "--run",
    volume.name,
  };
  if (!m.options.as && !this->default_user().empty())
  {
    args.emplace_back("--as");
    args.emplace_back(this->default_user());
  }
  auto env = elle::os::Environ{};
  m.options.to_commandline(args, env);
  for (auto const& host: _advertise_host)
  {
    args.emplace_back("--advertise-host");
    args.emplace_back(host);
  }
  args.insert(args.end(), extra_args.begin(), extra_args.end());
  if (this->_log_level)
    env.emplace("ELLE_LOG_LEVEL", _log_level.get());
  if (this->_log_path)
    env.emplace("ELLE_LOG_FILE",
                _log_path.get()
                + "/infinit-volume-"
                + boost::replace_all_copy(name, "/", "_")
                + '-'
                + boost::posix_time::to_iso_extended_string(
                  boost::posix_time::microsec_clock::universal_time())
                + ".log");
  ELLE_TRACE("Spawning with %s %s", args, env);
  // FIXME upgrade Process to accept env
  for (auto const& e: env)
    elle::os::setenv(e.first, e.second, true);
  m.process = std::make_unique<elle::system::Process>(args, true);
  int pid = m.process->pid();
  std::thread t([pid] {
      int status = 0;
      ::waitpid(pid, &status, 0);
  });
  t.detach();
  auto mountpoint = m.options.mountpoint;
  this->_mounts.emplace(name, std::move(m));
  if (wait_for_mount && mountpoint)
  {
    for (int i = 0; i < 100; ++i)
    {
      if (kill(pid, 0))
      {
        ELLE_WARN("infinit-volume for \"%s\" not running", name);
        break;
      }
      if (is_mounted(mountpoint.get()))
        return;
      reactor::sleep(100_ms);
    }
    this->stop(name);
    elle::err("unable to mount %s", name);
  }
}

void
MountManager::stop(std::string const& name)
{
  auto it = _mounts.find(name);
  if (it == _mounts.end())
    elle::err("not mounted: %s");
  auto pid = it->second.process->pid();
  ::kill(pid, SIGTERM);
  bool force_kill = true;
  for (int i = 0; i < 15; ++i)
  {
    reactor::sleep(1_sec);
    if (::kill(pid, 0))
    {
      force_kill = false;
      break;
    }
  }
  if (force_kill)
    ::kill(pid, SIGKILL);
  this->_mounts.erase(it);
}

void
MountManager::status(std::string const& name,
                     elle::serialization::SerializerOut& reply)
{
  auto it = this->_mounts.find(name);
  if (it == this->_mounts.end())
    elle::err("not mounted: %s", name);
  bool live = ! kill(it->second.process->pid(), 0);
  reply.serialize("live", live);
  reply.serialize("name", name);
  if (it->second.options.mountpoint)
    reply.serialize("mountpoint", mountpoint(name));
}

std::vector<MountInfo>
MountManager::status()
{
  return elle::make_vector(_mounts, [](auto&& m) {
      return MountInfo{
        m.first,
        !kill(m.second.process->pid(), 0),
        m.second.options.mountpoint,
      };
    });
}

void
MountManager::update_network(infinit::Network& network,
                             elle::json::Object const& options)
{
  bool updated = false;
  if (auto storagedesc = optional(options, "storage"))
  {
    updated = true;
    std::unique_ptr<infinit::storage::StorageConfig> storage_config;
    if (storagedesc->empty())
    {
      std::string storagename = network.name + "_storage";
      boost::replace_all(storagename, "/", "_");
      ELLE_LOG("Creating local storage %s", storagename);
      auto path = infinit::xdg_data_home() / "blocks" / storagename;
      storage_config = std::make_unique<infinit::storage::FilesystemStorageConfig>(
        storagename, path.string(), boost::none, boost::none);
    }
    else
    {
      try
      {
        storage_config = ifnt.storage_get(*storagedesc);
      }
      catch (infinit::MissingLocalResource const&)
      {
        throw elle::Error("Storage specification for new storage not implemented");
      }
    }
    network.model->storage = std::move(storage_config);
  }
  else if (optional(options, "no-storage"))
  {
    network.model->storage.reset();
    updated = true;
  }
  if (auto portstring = optional(options, "port"))
  {
    auto dht = dynamic_cast<infinit::model::doughnut::Configuration*>(network.model.get());
    dht->port = std::stoi(*portstring);
    if (*dht->port == 0)
      dht->port.reset();
    updated = true;
  }
  if (auto rfstring = optional(options, "replication-factor"))
  {
    auto dht = dynamic_cast<infinit::model::doughnut::Configuration*>(network.model.get());
    int rf = std::stoi(*rfstring);
    auto consensus = dynamic_cast
      <infinit::model::doughnut::consensus::Paxos::Configuration*>
        (dht->consensus.get());
    consensus->replication_factor(rf);
    updated = true;
  }
  if (updated)
    ifnt.network_save(std::move(network), true);
}

infinit::Network
MountManager::create_network(elle::json::Object const& options,
                             infinit::User const& owner)
{
  auto netname = optional(options, "network");
  if (!netname)
    netname = this->_default_network;
  ELLE_LOG("Creating network %s", netname);
  int rf = 1;
  if (auto rfstring = optional(options, "replication-factor"))
    rf = std::stoi(*rfstring);
  // create the network
  auto kelips =
    std::make_unique<infinit::overlay::kelips::Configuration>();
  kelips->k = 1;
  kelips->rpc_protocol = infinit::model::doughnut::Protocol::all;
  std::unique_ptr<infinit::model::doughnut::consensus::Configuration> consensus_config;
  consensus_config = std::make_unique<
    infinit::model::doughnut::consensus::Paxos::Configuration>(
        rf,
        std::chrono::seconds(10 * 60));
  auto port = boost::optional<int>{};
  if (auto portstring = optional(options, "port"))
    port = std::stoi(*portstring);
  auto dht =
    std::make_unique<infinit::model::doughnut::Configuration>(
      infinit::model::Address::random(0), // FIXME
      std::move(consensus_config),
      std::move(kelips),
      std::unique_ptr<infinit::storage::StorageConfig>(),
      owner.keypair(),
      std::make_shared<infinit::cryptography::rsa::PublicKey>(owner.public_key),
      infinit::model::doughnut::Passport(
        owner.public_key,
        ifnt.qualified_name(*netname, owner),
        infinit::cryptography::rsa::KeyPair(owner.public_key,
                                            owner.private_key.get())),
      owner.name,
      port,
      version,
      infinit::model::doughnut::AdminKeys(),
      std::vector<infinit::model::Endpoints>());
  auto fullname = ifnt.qualified_name(*netname, owner);
  auto network = infinit::Network(fullname, std::move(dht), boost::none);
  ifnt.network_save(std::move(network));
  report_created("network", *netname);
  link_network(fullname, options);
  auto desc = infinit::NetworkDescriptor(ifnt.network_get(*netname, owner, true));
  if (this->_push)
    try
    {
      beyond_push("network", desc.name, desc, owner);
    }
    catch (elle::Error const& e)
    {
      ELLE_WARN("Failed to push network %s to beyond: %s", desc.name, e);
    }
  return ifnt.network_get(*netname, owner, true);
}

void
MountManager::create_volume(std::string const& name,
                            elle::json::Object const& options)
{
  this->acquire_volumes();
  auto username = optional(options, "user");
  if (!username)
    username = this->_default_user;
  auto user = ifnt.user_get(*username);
  auto netname = optional(options, "network");
  if (!netname)
  {
    if (!this->_default_network)
      elle::err("either network or a default network must be set");
    netname = this->_default_network;
  }
  auto nname = *netname;
  if (nname.find("/") == nname.npos)
    nname = *username + "/" + nname;
  infinit::Network network ([&]() -> infinit::Network {
      try
      {
        auto net = ifnt.network_get(*netname, user, true);
        update_network(net, options);
        return net;
      }
      catch (infinit::MissingLocalResource const&)
      {
        return create_network(options, user);
      }
      catch (elle::Error const&)
      {
        link_network(nname, options);
        return ifnt.network_get(*netname, user, true);
      }
  }());
  auto process = [&]
    {
      static const auto root = elle::system::self_path().parent_path();
      auto args = std::vector<std::string>{
        (root / "infinit-volume").string(),
        "--create",
        name,
        "--network",
        network.name,
        "--create-root",
        "--register-service",
      };
      auto mo = infinit::MountOptions{};
      if (this->_fetch)
        mo.fetch = true;
      if (this->_push)
        mo.push = true;
      mo.as = username;
      auto env = elle::os::Environ{};
      mo.to_commandline(args, env);
      return std::make_unique<elle::system::Process>(args, true);
    }();
  if (process->wait())
    elle::err("volume creation failed");
  auto qname = elle::sprintf("%s/%s", this->_default_user, name);
  auto volume = ifnt.volume_get(qname);
  report_created("volume", volume.name);
  if (this->_push)
    try
    {
      beyond_push("volume", volume.name, volume, user);
    }
    catch (elle::Error const& e)
    {
      ELLE_WARN("failed to push %s to beyond: %s", volume.name, e);
    }
}

void
MountManager::delete_volume(std::string const& name)
{
  auto owner = ifnt.user_get(this->default_user());
  auto qname = ifnt.qualified_name(name, owner);
  auto path = ifnt._volume_path(qname);
  auto volume = ifnt.volume_get(qname);
  beyond_delete("volume", qname, owner, true);
  if (bfs::remove(path))
    report_action("deleted", "volume", name, "locally");
  else
    elle::err("File for volume could not be deleted: %s", path);
}

class DockerVolumePlugin
{
public:
  DockerVolumePlugin(MountManager& manager,
                     SystemUser& user, reactor::Mutex& mutex);
  ~DockerVolumePlugin();
  void install(bool tcp, int tcp_port,
               bfs::path socket_folder,
               bfs::path descriptor_folder);
  void uninstall();
  std::string mount(std::string const& name);
private:
  ELLE_ATTRIBUTE_R(MountManager&, manager);
  std::unique_ptr<reactor::network::HttpServer> _server;
  std::unordered_map<std::string, int> _mount_count;
  SystemUser& _user;
  reactor::Mutex& _mutex;
  bfs::path _socket_path;
  bfs::path _spec_json_path;
  bfs::path _spec_url_path;
};

namespace
{
  elle::json::Object
  daemon_command(std::string const& s, bool hold = false);

  class PIDFile
    : public elle::PIDFile
  {
  public:
    PIDFile()
      : elle::PIDFile(this->path())
    {}

    static
    bfs::path
    root_path()
    {
      auto res = bfs::path("/run/user/0/infinit/filesystem/daemon.pid");
      if (getuid() == 0 && bfs::create_directories(res.parent_path()))
        bfs::permissions(res.parent_path(), bfs::add_perms | bfs::others_read);
      return res;
    }

    static
    bfs::path
    path()
    {
      if (getuid() == 0)
        return root_path();
      else
        return infinit::xdg_runtime_dir() / "daemon.pid";
    }

    static
    int
    read()
    {
      return elle::PIDFile::read(PIDFile::path());
    }
  };

  bool
  daemon_running()
  {
    try
    {
      daemon_command("{\"operation\": \"status\"}");
      return true;
    }
    catch (elle::Error const& e)
    {
      ELLE_TRACE("status command threw %s", e);
      return false;
    }
  }

  int
  daemon_pid(bool ensure_running = false)
  {
    int pid = 0;
    try
    {
      // FIXME: Try current user, then root. Must be a better way to do this.
      try
      {
        pid = PIDFile::read();
      }
      catch (elle::Error const&)
      {
        pid = elle::PIDFile::read(PIDFile::root_path());
      }
      if (ensure_running && !daemon_running())
        pid = 0;
    }
    catch (elle::Error const& e)
    {
      ELLE_TRACE("error getting PID from %s and %s: %s",
                 PIDFile::path(), PIDFile::root_path(), e);
    }
    return pid;
  }

  elle::serialization::json::SerializerIn
  cmd_response_serializer(elle::json::Object const& json,
                          std::string const& action);

  void
  daemon_stop()
  {
    int pid = daemon_pid(true);
    if (!pid)
      elle::err("daemon is not running");
    try
    {
      auto json = daemon_command("{\"operation\": \"stop\"}");
      cmd_response_serializer(json, "stop daemon");
    }
    catch (reactor::network::ConnectionClosed const&)
    {}
    auto signal_and_wait_exit = [pid] (boost::optional<int> signal = {}) {
      if (kill(pid, 0) && errno == EPERM)
        elle::err("permission denied");
      if (signal)
      {
        ELLE_TRACE("sending %s", strsignal(*signal));
        if (kill(pid, *signal))
          return false;
        std::cout << "daemon signaled, waiting..." << std::endl;
      }
      int count = 0;
      while (++count < 150)
      {
        if (kill(pid, 0) && errno == ESRCH)
          return true;
        usleep(100000);
      }
      return false;
    };
    if (!signal_and_wait_exit()
      && !signal_and_wait_exit(SIGTERM)
    && !signal_and_wait_exit(SIGKILL))
    elle::err("unable to stop daemon");
    std::cout << "daemon stopped" << std::endl;
  }

  void
  daemonize()
  {
    if (daemon(1, 0))
      elle::err("failed to daemonize: %s", strerror(errno));
  }

  elle::json::Object
  daemon_command(std::string const& s, bool hold)
  {
    reactor::Scheduler sched;
    elle::json::Object res;
    reactor::Thread main_thread(
      sched,
      "main",
      [&]
      {
        // try local then global
        std::unique_ptr<reactor::network::UnixDomainSocket> sock = [] {
          try
          {
            try
            {
              return std::make_unique<reactor::network::UnixDomainSocket>(
                daemon_sock_path());
            }
            catch (elle::Error const&)
            {
              return std::make_unique<reactor::network::UnixDomainSocket>(
                bfs::path("/tmp/infinit-root/daemon.sock"));
            }
          }
          catch (reactor::network::ConnectionRefused const&)
          {
            elle::err("ensure that an instance of infinit-daemon is running");
          }
        }();
        std::string cmd = s + "\n";
        ELLE_TRACE("writing query: %s", s);
        sock->write(elle::ConstWeakBuffer(cmd));
        ELLE_TRACE("reading result");
        auto buffer = sock->read_until("\n");
        auto stream = elle::IOStream(buffer.istreambuf());
        res = boost::any_cast<elle::json::Object>(elle::json::read(stream));
        if (hold)
          reactor::sleep();
      });
    sched.run();
    return res;
  }

  void
  restart_volume(MountManager& manager, std::string const& volume, bool always_start=false)
  {
    infinit::MountOptions mo;
    try
    {
      mo.mountpoint = manager.mountpoint(volume, true);
    }
    catch (elle::Error const&)
    {}
    try
    {
      manager.stop(volume);
    }
    catch (elle::Error const& e)
    {
      if (!always_start)
        throw;
    }
    reactor::sleep(5_sec);
    manager.start(volume, mo, false, true);
  }

  std::string
  process_command(elle::json::Object query, MountManager& manager,
                  std::function<void()>& on_end)
  {
    ELLE_TRACE("command: %s", elle::json::pretty_print(query));
    auto command = elle::serialization::json::SerializerIn(query, false);
    std::stringstream ss;
    {
      auto response = elle::serialization::json::SerializerOut(ss, false);
      auto op = command.deserialize<std::string>("operation");
      response.serialize("operation", op);
      try
      {
        if (op == "status")
        {
          response.serialize("status", "Ok");
        }
        else if (op == "stop")
        {
          if (getuid() == geteuid())
            throw elle::Exit(0);
          else
            elle::err("permission denied");
        }
        else if (op == "volume-list")
        {
          auto res = manager.list();
          response.serialize("volumes", res);
        }
        else if (op == "volume-status")
        {
          if (auto volume
              = command.deserialize<boost::optional<std::string>>("volume"))
            manager.status(*volume, response);
          else
            response.serialize("volumes", manager.status());
        }
        else if (op == "volume-start")
        {
          auto volume = command.deserialize<std::string>("volume");
          auto opts =
            command.deserialize<boost::optional<infinit::MountOptions>>("options");
          manager.start(volume, opts ? opts.get() : infinit::MountOptions(),
                        true, true);
          response.serialize("volume", volume);
        }
        else if (op == "volume-stop")
        {
          auto volume = command.deserialize<std::string>("volume");
          manager.stop(volume);
          response.serialize("volume", volume);
        }
        else if (op == "volume-restart")
        {
          auto volume = command.deserialize<std::string>("volume");
          restart_volume(manager, volume);
          response.serialize("volume", volume);
        }
        else if (op == "disable-storage")
        {
          auto volume = command.deserialize<std::string>("volume");
          ELLE_LOG("Disabling storage on %s", volume);
          auto opts = elle::json::Object {
            {"no-storage", std::string{}},
          };
          manager.create_volume(volume, opts);
          restart_volume(manager, volume);
        }
        else if (op == "enable-storage")
        {
          auto volume = command.deserialize<std::string>("volume");
          ELLE_LOG("Enabling storage on %s", volume);
          auto opts = elle::json::Object {
            {"storage", std::string{}},
          };
          manager.create_volume(volume, opts);
          restart_volume(manager, volume, true);
          if (command.deserialize<bool>("hold"))
            on_end = [volume,&manager]() {
              ELLE_LOG("Disabling storage on %s", volume);
              elle::json::Object opts;
              opts["no-storage"] = std::string();
              manager.create_volume(volume, opts);
              restart_volume(manager, volume);
            };
        }
        else
        {
          elle::err("unknown operation: %s", op);
        }
        response.serialize("result", "Ok");
      }
      catch (elle::Error const& e)
      {
        response.serialize("result", "Error");
        response.serialize("error", e.what());
      }
    }
    ss << '\n';
    return ss.str();
  }
}

COMMAND(stop)
{
  daemon_stop();
}

COMMAND(status)
{
  if (daemon_running())
    std::cout << "Running" << std::endl;
  else
    std::cout << "Stopped" << std::endl;
}

COMMAND(fetch)
{
  acquire_volume(mandatory(args, "name"));
}

namespace
{
  void
  auto_mounter(std::vector<std::string> mounts,
               DockerVolumePlugin& dvp)
  {
    ELLE_TRACE("entering automounter");
    while (!mounts.empty())
    {
      for (unsigned int i=0; i<mounts.size(); ++i)
        try
        {
          dvp.mount(mounts[i]);
          mounts[i] = mounts[mounts.size()-1];
          mounts.pop_back();
          --i;
        }
        catch (elle::Error const& e)
        {
          ELLE_TRACE("Mount of %s failed: %s", mounts[i], e);
        }
      if (!mounts.empty())
        reactor::sleep(20_sec);
    }
    ELLE_TRACE("Exiting automounter");
  }

  template <typename T>
  T
  with_default(boost::program_options::variables_map const& vm,
               std::string const& name,
               T default_value)
  {
    return optional<T>(vm, name).value_or(default_value);
  }

  void
  fill_manager_options(MountManager& manager,
                       boost::program_options::variables_map const& args)
  {
    auto loglevel = optional(args, "log-level");
    manager.log_level(loglevel);
    auto logpath = optional(args, "log-path");
    manager.log_path(logpath);
    manager.fetch(option_fetch(args));
    manager.push(option_push(args));
    manager.default_user(self_user_name(args));
    manager.default_network(optional(args, "default-network"));
    if (auto advertise = optional<std::vector<std::string>>(args, "advertise-host"))
      manager.advertise_host(*advertise);
  }


  void
  _run(boost::program_options::variables_map const& args, bool detach)
  {
    ELLE_TRACE("starting daemon");
    if (daemon_running())
      elle::err("daemon already running");
    auto system_user = [&] {
      auto name = optional(args, "docker-user");
      auto home = optional(args, "docker-home");
      if (name)
        return SystemUser(*name, home);
      else
        return SystemUser(getuid(), home);
    }();
    reactor::Mutex mutex;
    auto managers = std::unordered_map<int, std::unique_ptr<MountManager>>{};
    auto docker = std::unique_ptr<DockerVolumePlugin>{};
    auto mounter = std::unique_ptr<reactor::Thread>{};
    // Always call get_mount_root() before entering the SystemUser.
    auto get_mount_root = [&] (SystemUser const& user) {
      if (args.count("mount-root"))
      {
        auto res = bfs::path(args["mount-root"].as<std::string>());
        if (!bfs::exists(res))
          elle::err("mount root does not exist");
        if (!bfs::is_directory(res))
          elle::err("mount root is not a directory");
        // Add the uid so that we have a directory for each user.
        res /= std::to_string(user.uid);
        if (bfs::create_directories(res))
        {
          elle::chown(res.string(), user.uid, user.gid);
          bfs::permissions(res.string(), bfs::owner_all);
        }
        return bfs::canonical(res).string();
      }
      else
      {
        auto env_run_dir = elle::os::getenv("XDG_RUNTIME_DIR", "");
        auto user_run_dir = !env_run_dir.empty()
          ? env_run_dir : elle::sprintf("/run/user/%s", user.uid);
        if (bfs::create_directories(user_run_dir))
        {
          elle::chown(user_run_dir, user.uid, user.gid);
          bfs::permissions(user_run_dir, bfs::owner_all);
        }
        {
          if (mutex.locked())
            elle::err("call get_mount_root() before .enter() on the SystemUser");
          auto lock = user.enter(mutex);
          auto res = elle::sprintf("%s/infinit/filesystem/mnt", user_run_dir);
          bfs::create_directories(res);
          return res;
        }
      }
    };
    auto user_mount_root = get_mount_root(system_user);
    auto docker_mount_sub
      = with_default<std::string>(args, "docker-mount-substitute", "");
    {
      auto lock = system_user.enter(mutex);
      auto users = optional<std::vector<std::string>>(args, "login-user");
      if (users) for (auto const& u: *users)
      {
        auto sep = u.find_first_of(":");
        std::string name = u.substr(0, sep);
        std::string pass = u.substr(sep+1);
        LoginCredentials c {name, hash_password(pass, _hub_salt)};
        auto json = beyond_login(name, c);
        elle::serialization::json::SerializerIn input(json, false);
        auto user = input.deserialize<infinit::User>();
        ifnt.user_save(user, true);
        report_action("saved", "user", name, "locally");
      }
      ELLE_TRACE("starting initial manager");
      managers[getuid()].reset(new MountManager(user_mount_root,
                                                docker_mount_sub));
      MountManager& root_manager = *managers[getuid()];
      fill_manager_options(root_manager, args);
      docker = std::make_unique<DockerVolumePlugin>(
        root_manager, system_user, mutex);
      if (auto mount = optional<std::vector<std::string>>(args, "mount"))
        mounter = std::make_unique<reactor::Thread>("mounter",
          [&] {auto_mounter(*mount, *docker);});
    }
    if (flag(args, "docker"))
    {
#if !defined INFINIT_PRODUCTION_BUILD || defined INFINIT_LINUX
      try
      {
        docker->install(
          flag(args, "docker-socket-tcp"),
          std::stoi(with_default<std::string>(args, "docker-socket-port", "0")),
          with_default<std::string>(
            args, "docker-socket-path", "/run/docker/plugins"),
          with_default<std::string>(
            args, "docker-descriptor-path", "/usr/lib/docker/plugins"));
      }
      catch (std::exception const& e)
      {
        ELLE_ERR("Failed to install docker plugin: %s", e.what());
        ELLE_ERR("Docker plugin disabled");
      }
#endif
    }
    if (detach)
      daemonize();
    PIDFile pid;
    reactor::network::UnixDomainServer srv;
    auto sockaddr = daemon_sock_path();
    bfs::remove(sockaddr);
    srv.listen(sockaddr);
    chmod(sockaddr.string().c_str(), 0666);
    elle::SafeFinally terminator([&] {
      if (mounter)
        mounter->terminate_now();
      ELLE_LOG("stopped daemon");
    });
    elle::With<reactor::Scope>() << [&] (reactor::Scope& scope)
    {
      ELLE_LOG("started daemon");
      while (true)
      {
        auto socket = elle::utility::move_on_copy(srv.accept());
        auto native = socket->socket()->native();
        uid_t uid;
        gid_t gid;
#ifdef INFINIT_MACOSX
        if (getpeereid(native, &uid, &gid))
        {
          ELLE_ERR("getpeerid failed: %s", strerror(errno));
          continue;
        }
#elif defined INFINIT_LINUX
        struct ucred ucred;
        socklen_t len = sizeof ucred;
        if (getsockopt(native, SOL_SOCKET, SO_PEERCRED, &ucred, &len) == -1)
        {
           ELLE_ERR("getsocopt(peercred) failed: %s", strerror(errno));
           continue;
        }
        uid = ucred.uid;
        gid = ucred.gid;
#else
# error "unsupported platform"
#endif
        static reactor::Mutex mutex;
        (void)gid;
        SystemUser system_user(uid);
        MountManager* user_manager = nullptr;
        auto it = managers.find(uid);
        if (it == managers.end())
        {
          auto peer_mount_root = get_mount_root(system_user);
          auto lock = system_user.enter(mutex);
          user_manager = new MountManager(peer_mount_root, docker_mount_sub);
          fill_manager_options(*user_manager, args);
          managers[uid].reset(user_manager);
        }
        else
          user_manager = it->second.get();
        auto name = elle::sprintf("%s server", **socket);
        scope.run_background(
          name,
          [socket, user_manager, system_user]
          {
            auto on_end = std::function<void()>{};
            elle::SafeFinally sf([&] {
                try
                {
                  if (on_end)
                    on_end();
                }
                catch(elle::Error const& e)
                {
                  ELLE_WARN("Unexpected exception in on_end: %s", e);
                }
            });
            try
            {
              while (true)
              {
                auto json =
                  boost::any_cast<elle::json::Object>(elle::json::read(**socket));
                auto reply = [&] {
                  SystemUser::Lock lock(system_user.enter(mutex));
                  return process_command(json, *user_manager, on_end);
                }();
                ELLE_TRACE("Writing reply: '%s'", reply);
                socket->write(reply);
              }
            }
            catch (elle::Error const& e)
            {
              ELLE_TRACE("%s", e);
              try
              {
                socket->write(elle::sprintf("{\"error\": \"%s\"}\n", e.what()));
              }
              catch (elle::Error const&)
              {}
            }
          });
      }
    };
  }
}

COMMAND(start)
{
  _run(args, true);
}

COMMAND(run)
{
  _run(args, false);
}

DockerVolumePlugin::DockerVolumePlugin(MountManager& manager,
                                       SystemUser& user,
                                       reactor::Mutex& mutex)
  : _manager(manager)
  , _user(user)
  , _mutex(mutex)
{}

DockerVolumePlugin::~DockerVolumePlugin()
{
  uninstall();
}

void
DockerVolumePlugin::uninstall()
{
  boost::system::error_code erc;
  if (!this->_socket_path.empty())
    bfs::remove(this->_socket_path, erc);
  if (!this->_spec_json_path.empty())
    bfs::remove(this->_spec_json_path, erc);
  if (!this->_spec_url_path.empty())
    bfs::remove(this->_spec_url_path, erc);
}

std::string
DockerVolumePlugin::mount(std::string const& name)
{
  auto it = _mount_count.find(name);
  if (it != _mount_count.end())
  {
    ELLE_TRACE("Already mounted");
    ++it->second;
  }
  else
  {
    _manager.start(name, {}, true, true);
    _mount_count.insert(std::make_pair(name, 1));
  }
  return _manager.mountpoint(name);
}

void
DockerVolumePlugin::install(bool tcp,
                            int tcp_port,
                            bfs::path socket_folder,
                            bfs::path descriptor_folder)
{
  // plugin path is either in /etc/docker/plugins or /usr/lib/docker/plugins
  boost::system::error_code erc;
  create_directories(descriptor_folder, erc);
  if (erc)
    elle::err("Unable to create descriptor folder (%s): %s",
              descriptor_folder, erc.message());
  bfs::create_directories(socket_folder, erc);
  if (erc)
    elle::err("Unable to create socket folder (%s): %s",
              socket_folder, erc.message());
  this->_socket_path = socket_folder / "infinit.sock";
  bfs::remove(this->_socket_path, erc);
  this->_spec_json_path = descriptor_folder / "infinit.json";
  bfs::remove(this->_spec_json_path, erc);
  this->_spec_url_path = descriptor_folder / "infinit.spec";
  bfs::remove(this->_spec_url_path, erc);
  if (tcp)
  {
    this->_server = std::make_unique<reactor::network::HttpServer>(tcp_port);
    int port = this->_server->port();
    auto url = elle::sprintf("tcp://localhost:%s", port);
    bfs::ofstream ofs(this->_spec_url_path);
    if (!ofs.good())
      elle::err("Unable to write to URL .spec file: %s", this->_spec_url_path);
    ofs << url;
  }
  else
  {
    auto us = std::make_unique<reactor::network::UnixDomainServer>();
    us->listen(this->_socket_path);
    this->_server =
      std::make_unique<reactor::network::HttpServer>(std::move(us));
  }
  {
    auto json = elle::json::Object {
      {"Name", std::string{"infinit"}},
      {"Addr", std::string{"https://infinit.sh"}},
    };
    bfs::ofstream ofs(this->_spec_json_path);
    if (!ofs.good())
      elle::err("Unable to write JSON plugin description: %s",
                this->_spec_json_path);
    elle::json::write(ofs, json);
  }
#define ROUTE_SIG  (reactor::network::HttpServer::Headers const&,       \
                    reactor::network::HttpServer::Cookies const&,       \
                    reactor::network::HttpServer::Parameters const&,    \
                    elle::Buffer const& data) -> std::string
  _server->register_route("/Plugin.Activate",  reactor::http::Method::POST,
    [] ROUTE_SIG {
      ELLE_TRACE("Activating plugin");
      return "{\"Implements\": [\"VolumeDriver\"]}";
    });
  _server->register_route("/VolumeDriver.Create", reactor::http::Method::POST,
    [this] ROUTE_SIG {
      auto lock = this->_user.enter(this->_mutex);
      auto stream = elle::IOStream(data.istreambuf());
      auto json = boost::any_cast<elle::json::Object>(elle::json::read(stream));
      std::string err;
      try
      {
        auto opts = elle::json::Object{};
        try
        {
          opts = boost::any_cast<elle::json::Object>(json.at("Opts"));
        }
        catch(...)
        {}
        auto name = optional(json, "Name");
        if (!name)
          elle::err("missing 'Name' argument");
        this->_manager.create_volume(name.get(), opts);
      }
      catch (infinit::ResourceAlreadyFetched const&)
      {
        // This can happen, docker seems to be caching volume list:
        // a mount request can trigger a create request without any list.
      }
      catch (elle::Error const& e)
      {
        err = boost::replace_all_copy(elle::sprintf("%s", e), "\"", "'");
        ELLE_WARN("error creating volume: %s", e);
      }
      return "{\"Err\": \"" + err + "\"}";
    });
  _server->register_route("/VolumeDriver.Remove", reactor::http::Method::POST,
    [this] ROUTE_SIG {
      elle::err("use infinit-volume --delete to delete volumes");
      // auto lock = this->_user.enter(this->_mutex);
      // // Reverse the Create process.
      // auto stream = elle::IOStream(data.istreambuf());
      // auto json = boost::any_cast<elle::json::Object>(elle::json::read(stream));
      // std::string err;
      // try
      // {
      //   auto name = optional(json, "Name");
      //   if (!name)
      //     throw elle::Error("Missing 'Name' argument");
      //   this->_manager.delete_volume(name.get());
      // }
      // catch (elle::Error const& e)
      // {
      //   err = elle::sprintf("%s", e);
      //   ELLE_LOG("%s\n%s", e, e.backtrace());
      // }
      // boost::replace_all(err, "\"", "'");
      // return "{\"Err\": \"" + err + "\"}";
    });
  _server->register_route("/VolumeDriver.Get", reactor::http::Method::POST,
    [this] ROUTE_SIG {
      auto lock = this->_user.enter(this->_mutex);
      auto stream = elle::IOStream(data.istreambuf());
      auto json = boost::any_cast<elle::json::Object>(elle::json::read(stream));
      auto name = boost::any_cast<std::string>(json.at("Name"));
      if (this->_manager.exists(name))
        return "{\"Err\": \"\", \"Volume\": {\"Name\": \"" + name + "\" }}";
      else
        return "{\"Err\": \"No such mount\"}";
    });
  _server->register_route("/VolumeDriver.Mount", reactor::http::Method::POST,
    [this] ROUTE_SIG {
      auto lock = this->_user.enter(this->_mutex);
      auto stream = elle::IOStream(data.istreambuf());
      auto json = boost::any_cast<elle::json::Object>(elle::json::read(stream));
      auto name = boost::any_cast<std::string>(json.at("Name"));
      std::string mountpoint = mount(name);
      std::string res = "{\"Err\": \"\", \"Mountpoint\": \""
          + mountpoint +"\"}";
      ELLE_TRACE("reply: %s", res);
      return res;
    });
  _server->register_route("/VolumeDriver.Unmount", reactor::http::Method::POST,
    [this] ROUTE_SIG {
      auto lock = this->_user.enter(this->_mutex);
      auto stream = elle::IOStream(data.istreambuf());
      auto json = boost::any_cast<elle::json::Object>(elle::json::read(stream));
      auto name = boost::any_cast<std::string>(json.at("Name"));
      auto it = _mount_count.find(name);
      if (it == _mount_count.end())
        return "{\"Err\": \"No such mount\"}";
      --it->second;
      if (it->second == 0)
      {
        _mount_count.erase(it);
        _manager.stop(name);
      }
      return "{\"Err\": \"\"}";
    });
  _server->register_route("/VolumeDriver.Path", reactor::http::Method::POST,
    [this] ROUTE_SIG {
      auto lock = this->_user.enter(this->_mutex);
      auto stream = elle::IOStream(data.istreambuf());
      auto json = boost::any_cast<elle::json::Object>(elle::json::read(stream));
      auto name = boost::any_cast<std::string>(json.at("Name"));
      try
      {
        return "{\"Err\": \"\", \"Mountpoint\": \""
          + this->_manager.mountpoint(name) +"\"}";
      }
      catch (elle::Error const& e)
      {
        auto err = boost::replace_all_copy(elle::sprintf("%s", e), "\"", "'");
        return "{\"Err\": \"" + err + "\"}";
      }
    });
  _server->register_route("/VolumeDriver.List", reactor::http::Method::POST,
    [this] ROUTE_SIG {
      auto lock = this->_user.enter(this->_mutex);
      auto res = std::string{"{\"Err\": \"\", \"Volumes\": [ "};
      for (auto const& n: this->_manager.list())
        res += "{\"Name\": \"" + n  + "\"},";
      res = res.substr(0, res.size()-1);
      res += "]}";
      return res;
    });
   _server->register_route(
     "/VolumeDriver.Capabilities",
     reactor::http::Method::POST,
     [this] ROUTE_SIG {
       return "{}";
   });
}

namespace
{
  elle::serialization::json::SerializerIn
  cmd_response_serializer(elle::json::Object const& json,
                          std::string const& action)
  {
    auto s = elle::serialization::json::SerializerIn(json, false);
    if (json.count("error"))
      elle::err("Unable to %s: %s", action, s.deserialize<std::string>("error"));
    return s;
  }

  void
  volume_list()
  {
    auto json = daemon_command("{\"operation\": \"volume-list\"}");
    if (script_mode)
      std::cout << json;
    else
    {
      auto res = cmd_response_serializer(json, "list volumes");
      for (auto const& v: res.deserialize<std::vector<std::string>>("volumes"))
        std::cout << v << std::endl;
    }
  }

  void
  volume_status(boost::optional<std::string> name)
  {
    auto json = daemon_command(elle::sprintf(
      "{\"operation\": \"volume-status\"%s}",
      (name ? elle::sprintf(" ,\"volume\": \"%s\"", *name) : "")));
    if (script_mode)
      std::cout << json;
    else
    {
      auto res = cmd_response_serializer(json, "fetch volume status");
      if (name)
        std::cout << res.deserialize<MountInfo>() << std::endl;
      else
        for (auto const& m: res.deserialize<std::vector<MountInfo>>("volumes"))
          std::cout << m << std::endl;
    }
  }

  void
  volume_start(std::string const& name)
  {
    auto json = daemon_command(
      "{\"operation\": \"volume-start\", \"volume\": \"" + name +  "\"}");
    if (script_mode)
      std::cout << json;
    else
    {
      auto res = cmd_response_serializer(json, "start volume");
      std::cout << "Started: " << res.deserialize<std::string>("volume")
                << std::endl;
    }
  }

  void
  volume_stop(std::string const& name)
  {
    auto json = daemon_command(
      "{\"operation\": \"volume-stop\", \"volume\": \"" + name +  "\"}");
    if (script_mode)
      std::cout << json;
    else
    {
      auto res = cmd_response_serializer(json, "stop volume");
      std::cout << "Stopped: " << res.deserialize<std::string>("volume")
                << std::endl;
    }
  }

  void
  volume_restart(std::string const& name)
  {
    auto json = daemon_command(
      "{\"operation\": \"volume-restart\", \"volume\": \"" + name +  "\"}");
    if (script_mode)
      std::cout << json;
    else
    {
      auto res = cmd_response_serializer(json, "restart volume");
      std::cout << "Restarted: " << res.deserialize<std::string>("volume")
                << std::endl;
    }
  }
}

COMMAND(manage_volumes)
{
  auto flags = std::vector<std::string>
    { "list", "status", "start", "stop", "restart" };
  if (!exclusive_flag(args, flags))
  {
    auto opts = std::string{};
    for (auto const& f: flags)
      opts += elle::sprintf("\"--%s\", ", f);
    opts = opts.substr(0, opts.size() - 2);
    throw CommandLineError("Specify one of %s", opts);
  }
  if (flag(args, "list"))
    volume_list();
  if (flag(args, "status"))
    volume_status(optional(args, "name"));
  if (flag(args, "start"))
    volume_start(mandatory(args, "name"));
  if (flag(args, "stop"))
    volume_stop(mandatory(args, "name"));
  if (flag(args, "restart"))
    volume_restart(mandatory(args, "name"));
}

COMMAND(enable_storage)
{
  auto name = mandatory(args, "name");
  auto hold = flag(args, "hold");
  std::cout << daemon_command(
    "{\"operation\": \"enable-storage\", \"volume\": \"" + name +  "\""
    +",\"hold\": "  + (hold ? "true": "false")   + "}", hold);
}

COMMAND(disable_storage)
{
  auto name = mandatory(args, "name");
  std::cout << daemon_command(
    "{\"operation\": \"disable-storage\", \"volume\": \"" + name +  "\"}");
}

int
main(int argc, char** argv)
{
  using boost::program_options::value;
  using boost::program_options::bool_switch;
  std::vector<Mode::OptionDescription> options_run = {
    { "login-user", value<std::vector<std::string>>()->multitoken(),
      "Login with selected user(s), of form 'user:password'" },
    { "mount-root", value<std::string>(), elle::sprintf(
      "Default root path for all mounts\n(default: %s/infinit/filesystem/mnt)",
      elle::os::getenv("XDG_RUNTIME_DIR",
                       elle::sprintf("/run/user/%s", getuid()))) },
    { "default-network", value<std::string>(),
      "Default network for volume creation" },
    { "advertise-host", value<std::vector<std::string>>()->multitoken(),
      "Advertise given hostname as an extra endpoint when running volumes" },
    // { "mount,m", value<std::vector<std::string>>()->multitoken(),
    //   "mount given volumes on startup, keep trying on error" },
    { "fetch,f", bool_switch(), "Run volumes with --fetch" },
    { "push,p", bool_switch(), "Run volumes with --push" },
    { "publish", bool_switch(), "Alias for --publish" },
#if !defined(INFINIT_PRODUCTION_BUILD) || defined(INFINIT_LINUX)
    { "docker",
      value<bool>()->implicit_value(true, "true")->default_value(true, "true"),
      "Enable the Docker plugin\n(default: true)" },
    { "docker-user", value<std::string>(), elle::sprintf(
      "System user to use for docker plugin\n(default: %s)",
        SystemUser(getuid()).name) },
    { "docker-home", value<std::string>(),
      "Home directory to use for Docker user\n(default: /home/<docker-user>)" },
    { "docker-socket-tcp", bool_switch(),
      "Use a TCP socket for docker plugin" },
    { "docker-socket-port", value<std::string>(),
      "TCP port to use to communicate with Docker\n(default: random)" },
    { "docker-socket-path", value<std::string>(),
      "Path for plugin socket\n(default: /run/docker/plugins)" },
    { "docker-descriptor-path", value<std::string>(),
      "Path to add plugin descriptor\n(default: /usr/lib/docker/plugins)" },
    { "docker-mount-substitute", value<std::string>(),
      "[from:to|prefix] : Substitute 'from' to 'to' in advertised path" },
#endif
    { "log-level,l", value<std::string>(),
      "Log level to start volumes with (default: LOG)" },
    { "log-path,d", value<std::string>(), "Store volume logs in given path" },
  };
  Modes modes {
    {
      "status",
      "Query daemon status",
      &status,
      "",
      {},
    },
    {
      "run",
      "Run daemon in the foreground",
      &run,
      "",
      options_run,
    },
    {
      "start",
      "Start daemon in the background",
      &start,
      "",
      options_run,
    },
    {
      "stop",
      "Stop daemon",
      &stop,
      "",
      {},
    },
  };
  Modes hidden_modes {
    {
      "fetch",
      elle::sprintf("Fetch volume and its dependencies from %s", infinit::beyond(true)),
      &fetch,
      "--name VOLUME",
      {
        { "name,n", value<std::string>(), "volume name" },
      },
    },
    {
      "enable-storage",
      "Enable storage on associated network",
      &enable_storage,
      "--name VOLUME [--hold]",
      {
        { "name,n", value<std::string>(), "volume name"},
        { "hold", bool_switch(), "Keep storage online until this process terminates" },
      },
    },
    {
      "disable-storage",
      "Disable storage on associated network",
      &disable_storage,
      "--name VOLUME",
      {
        { "name,n", value<std::string>(), "volume name" },
      },
    },
    {
      "manage-volumes",
      "Manage daemon controlled volumes",
      &manage_volumes,
      "[--name VOLUME]",
      {
        { "list", bool_switch(), "list all volumes" },
        { "status", bool_switch(), "list all volume statuses" },
        { "start", bool_switch(), "start volume" },
        { "stop", bool_switch(), "stop volume" },
        { "restart", bool_switch(), "restart volume" },
        { "name", value<std::string>(), "volume name" },
      },
    },
  };
  return infinit::main(
    "Infinit daemon", modes, argc, argv, {}, {}, hidden_modes);
}
