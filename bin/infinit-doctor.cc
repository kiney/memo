#include <regex>
#include <unordered_map>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>

#include <elle/bytes.hh>
#include <elle/filesystem/TemporaryDirectory.hh>
#include <elle/filesystem/path.hh>
#include <elle/log.hh>
#include <elle/log/TextLogger.hh>
#include <elle/os/environ.hh>
#include <elle/string/algorithm.hh>
#include <elle/string/algorithm.hh>
#include <elle/system/Process.hh>

#include <reactor/connectivity/connectivity.hh>
#include <reactor/filesystem.hh>
#include <reactor/network/upnp.hh>
#include <reactor/scheduler.hh>
#include <reactor/TimeoutGuard.hh>
#include <reactor/http/exceptions.hh>

#include <infinit/storage/Dropbox.hh>
#include <infinit/storage/Filesystem.hh>
#include <infinit/storage/GCS.hh>
#include <infinit/storage/GoogleDrive.hh>
#include <infinit/storage/Strip.hh>
#include <cryptography/random.hh>
#ifndef INFINIT_WINDOWS
# include <infinit/storage/sftp.hh>
#endif
#include <infinit/storage/S3.hh>

ELLE_LOG_COMPONENT("infinit-doctor");

#include <infinit-doctor.hh>
#include <main.hh>
#include <networking.hh>

namespace bfs = boost::filesystem;

namespace
{
  bool no_color = false;

  infinit::Infinit ifnt;

  std::string banished_log_level("reactor.network.UTPSocket:NONE");

  std::string
  result(bool value)
  {
    return value ? "OK" : "Bad";
  }

  void
  section(std::ostream& out,
          std::string name)
  {
    boost::algorithm::to_upper(name);
    if (!no_color)
      out << "[1m";
    out << name << ":";
    if (!no_color)
      out << "[0m";
    out << std::endl;
  }

  template <typename C, typename ... Args>
  typename C::reference
  store(C& container,
        std::string const& key,
        Args&&... args)
  {
    container.emplace_back(key, std::forward<Args>(args)...);
    return container.back();
  }

  template <typename C>
  bool
  sane(C const& c, bool all = true)
  {
    auto filter = [&] (typename C::value_type const& x)
      {
        return x.sane();
      };
    if (all)
      return std::all_of(c.begin(), c.end(), filter);
    else
      return std::any_of(c.begin(), c.end(), filter);
  }

  template <typename C>
  bool
  warning(C const& c)
  {
    return std::any_of(c.begin(), c.end(),
                       [&] (typename C::value_type const& x)
                       {
                         return x.warning();
                       });
  }

  std::ostream&
  status(std::ostream& out,
         bool sane,
         bool warn = false)
  {
    if (!sane)
    {
      if (!no_color)
        out << "[33;00;31m";
      out << "[ERROR]";
    }
    else if (warn)
    {
      if (!no_color)
        out << "[33;00;33m";
      out << "[WARNING]";
    }
    else
    {
      if (!no_color)
        out << "[33;00;32m";
      out << "[OK]";
    }
    return out << "[0m";
  }

  std::ostream&
  print_reason(std::ostream& out, std::string const& reason, int indent = 2)
  {
    out << std::string(indent, ' ');
    if (!no_color)
      out << "[1m";
    out << "Reason:";
    if (!no_color)
      out << "[0m";
    out << " " << reason;
    return out;
  }

  template <typename T>
  void
  print_entry(std::ostream& out, T const& entry, bool sane, bool warning)
  {
    status(out << "  ", sane, warning) << " " << entry.name();
  }

  template <typename C>
  void
  print(std::ostream& out,
        std::string name,
        C& container,
        bool verbose)
  {
    bool sane = true;
    bool warning = false;
    for (auto const& x: container)
    {
      sane &= x.sane();
      warning |= x.warning();
      if (!sane && warning)
        break;
    }
    bool broken = !sane || warning;
    if (!name.empty())
      name[0] = std::toupper(name[0]);
    status(out, sane, warning) << " " << name;
    if (verbose || broken)
    {
      if (!container.empty())
        out << ":\n";
      for (auto const& item: container)
        if (verbose || !item.sane() || item.warning())
        {
          print_entry(out, item, item.sane(), item.warning());
          item.print(out << " ", verbose);
          out << std::endl;
        }
    }
    else
      out << std::endl;
  }
}

namespace reporting
{
  Result::Result(std::string const& name)
    : _name(name)
    , _sane(false)
    , reason()
    , _warning(false)
  {}

  Result::Result(std::string const& name,
                 bool sane,
                 Reason const& reason,
                 bool warning)
    : _name(name)
    , _sane(sane)
    , reason(reason)
    , _warning(warning)
  {}

  Result::Result(elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  Result::~Result()
  {}

  void
  Result::_print(std::ostream& out, bool verbose) const
  {}

  bool
  Result::show(bool verbose) const
  {
    return (!this->sane() || verbose || this->warning()) && this->_show(verbose);
  }

  bool
  Result::_show(bool verbose) const
  {
    return true;
  }

  std::ostream&
  Result::print(std::ostream& out, bool verbose, bool rc) const
  {
    status(out, this->sane(), this->warning()) << " " << this->name();
    if (this->show(verbose))
      out << ":";
    this->_print(out, verbose);
    if (this->show(verbose) && this->reason)
      print_reason(out << std::endl, *this->reason, 2);
    // status(out << std::endl << "  - ", this->sane(), this->warning())
    //   << ": " << *this->reason;
    return out << std::endl;
  }

  void
  Result::serialize(elle::serialization::Serializer& s)
  {
    s.serialize("name", this->_name);
    s.serialize("sane", this->_sane);
    s.serialize("reason", this->reason);
    s.serialize("warning", this->_warning);
  }

  std::ostream&
  ConfigurationIntegrityResults::Result::print(
    std::ostream& out, bool verbose) const
  {
    if (!this->sane())
      out << "is faulty because";
    this->_print(out, verbose);
    if (!this->sane() && this->reason)
      out << " " << *this->reason;
    return out;
  }

  ConfigurationIntegrityResults::StorageResoucesResult::StorageResoucesResult(
    std::string const& name,
    bool sane,
    std::string const& type,
    reporting::Result::Reason const& reason)
    : Result(name, sane, reason)
    , type(type)
  {}

  ConfigurationIntegrityResults::StorageResoucesResult::StorageResoucesResult(
    elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  ConfigurationIntegrityResults::StorageResoucesResult::_print(
    std::ostream& out, bool) const
  {}

  void
  ConfigurationIntegrityResults::StorageResoucesResult::serialize(
    elle::serialization::Serializer& s)
  {
    Result::serialize(s);
    s.serialize("type", this->type);
  }

  ConfigurationIntegrityResults::NetworkResult::NetworkResult(
    std::string const& name,
    bool sane,
    FaultyStorageResources storage_resources,
    Result::Reason extra_reason,
    bool linked)
    : Result(name, sane, extra_reason, !linked)
    , linked(linked)
    , storage_resources(storage_resources)
  {}

  ConfigurationIntegrityResults::NetworkResult::NetworkResult(
    elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  ConfigurationIntegrityResults::NetworkResult::serialize(
    elle::serialization::Serializer& s)
  {
    Result::serialize(s);
    s.serialize("linked", this->linked);
    s.serialize("storage_resources", this->storage_resources);
  }

  void
  ConfigurationIntegrityResults::NetworkResult::_print(
    std::ostream& out, bool verbose) const
  {
    if (!this->linked)
    {
      out << "is not linked";
    }
    if (this->storage_resources)
    {
      auto s = this->storage_resources->size();
      if (s > 0)
        out << elle::join(storage_resources->begin(),
                          storage_resources->end(), "], [");
      if (s == 1)
        out << " storage resource is faulty";
      else if (s > 1)
        out << " storage resources are faulty";
    }
  }

  ConfigurationIntegrityResults::VolumeResult::VolumeResult(
    std::string const& name,
    bool sane,
    FaultyNetwork faulty_network,
    Result::Reason extra_reason)
    : Result(name, sane, extra_reason)
    , faulty_network(faulty_network)
  {}

  ConfigurationIntegrityResults::VolumeResult::VolumeResult(
    elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  ConfigurationIntegrityResults::VolumeResult::serialize(
    elle::serialization::Serializer& s)
  {
    Result::serialize(s);
    s.serialize("network", this->faulty_network);
  }

  void
  ConfigurationIntegrityResults::VolumeResult::_print(
    std::ostream& out, bool) const
  {
    if (this->faulty_network)
      out << "network " << *this->faulty_network << " is faulty";
  }

  ConfigurationIntegrityResults::DriveResult::DriveResult(
    std::string const& name,
    bool sane,
    FaultyVolume faulty_volume,
    Result::Reason extra_reason)
    : Result(name, sane, extra_reason)
    , faulty_volume(faulty_volume)
  {}

  ConfigurationIntegrityResults::DriveResult::DriveResult(
    elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  ConfigurationIntegrityResults::DriveResult::serialize(
    elle::serialization::Serializer& s)
  {
    Result::serialize(s);
    s.serialize("volume", this->faulty_volume);
  }

  void
  ConfigurationIntegrityResults::DriveResult::_print(
    std::ostream& out, bool) const
  {
    if (this->faulty_volume)
    {
      out << "volume " << *this->faulty_volume << " is faulty";
    }
  }

  ConfigurationIntegrityResults::LeftoversResult::LeftoversResult(
    std::string const& name,
    Result::Reason r)
    : Result(name, true, r)
  {}

  ConfigurationIntegrityResults::LeftoversResult::LeftoversResult(
    elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  ConfigurationIntegrityResults::LeftoversResult::serialize(
    elle::serialization::Serializer& s)
  {
    Result::serialize(s);
  }

  ConfigurationIntegrityResults::ConfigurationIntegrityResults(bool only)
    : _only(only)
  {}

  ConfigurationIntegrityResults::ConfigurationIntegrityResults(
    elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  bool
  ConfigurationIntegrityResults::sane() const
  {
    return ::sane(this->storage_resources)
      && ::sane(this->networks)
      && ::sane(this->volumes)
      && ::sane(this->drives);
    // Leftovers is always sane.
  }

  bool
  ConfigurationIntegrityResults::warning() const
  {
    return ::warning(this->storage_resources)
      || ::warning(this->networks)
      || ::warning(this->volumes)
      || ::warning(this->drives)
      || ::warning(this->leftovers);
  }

  void
  ConfigurationIntegrityResults::serialize(elle::serialization::Serializer& s)
  {
    s.serialize("storage resources", this->storage_resources);
    s.serialize("networks", this->networks);
    s.serialize("volumes", this->volumes);
    s.serialize("drives", this->drives);
    s.serialize("leftovers", this->leftovers);
    if (s.out())
    {
      bool sane = this->sane();
      s.serialize("sane", sane);
    }
  }

  void
  ConfigurationIntegrityResults::print(std::ostream& out, bool verbose) const
  {
    if (!this->only())
      section(out, "Configuration integrity");
    ::print(out, "Storage resources", storage_resources, verbose);
    ::print(out, "Networks", networks, verbose);
    ::print(out, "Volumes", volumes, verbose);
    ::print(out, "Drives", drives, verbose);
    ::print(out, "Leftovers", leftovers, verbose);
    out << std::endl;
  }

  SystemSanityResults::UserResult::UserResult(
    std::tuple<bool, Result::Reason> const& validity,
    std::string const &name)
    : Result("Username", true, std::get<1>(validity), !std::get<0>(validity))
    , _user_name(name)
  {}

  SystemSanityResults::UserResult::UserResult(std::string const& name)
    : UserResult(valid(name), name)
  {}

  std::tuple<bool, Result::Reason>
  SystemSanityResults::UserResult::valid(std::string const& name) const
  {
    static const auto allowed = std::regex(infinit::User::name_regex());
    if (std::regex_match(name, allowed))
      return std::make_tuple(true, Result::Reason{});
    else
      return std::make_tuple(
        false,
        Result::Reason{
          elle::sprintf(
            "default system user name \"%s\" is not compatible with Infinit "
            "naming policies, you'll need to use --as <other_name>", name)});
  }

  void
  SystemSanityResults::UserResult::_print(std::ostream& out, bool verbose) const
  {
    if (this->show(verbose) && this->sane())
      out << " " << this->_user_name;
  }

  void
  SystemSanityResults::UserResult::serialize(elle::serialization::Serializer& s)
  {
    Result::serialize(s);
    s.serialize("user_name", this->_user_name);
  }

  SystemSanityResults::SpaceLeft::SpaceLeft(size_t minimum,
                                            double minimum_ratio,
                                            size_t available,
                                            size_t capacity,
                                            Result::Reason const& reason)
    : Result("Space left",
             capacity != 0,
             reason,
             ((available / (double) capacity) < minimum_ratio)
             && available < minimum)
    , minimum(minimum)
    , minimum_ratio(minimum_ratio)
    , available(available)
    , capacity(capacity)
  {}

  SystemSanityResults::SpaceLeft::SpaceLeft(
    elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  SystemSanityResults::SpaceLeft::_print(std::ostream& out, bool verbose) const
  {
    if (this->show(verbose))
    {
      if (!this->sane() || this->warning())
        out << std::endl << "  - " << "low";
      elle::fprintf(
        out, " %s available (~%s%%)",
        elle::human_data_size(this->available, false),
        (int) (100 * this->available / (double) this->capacity));
    }
  }

  void
  SystemSanityResults::SpaceLeft::serialize(elle::serialization::Serializer& s)
  {
    Result::serialize(s);
    s.serialize("minimum", this->minimum);
    s.serialize("minimum_ratio", this->minimum_ratio);
    s.serialize("available", this->available);
    s.serialize("capacity", this->capacity);
  }

  SystemSanityResults::EnvironResult::EnvironResult(
    Environ const& environ)
    : Result("Environ",
             true,
             (environ.empty()
              ? reporting::Result::Reason{}
              : reporting::Result::Reason{
               "your environ contains variables that will modify "
               "Infinit default behavior. For more details visit "
               "https://infinit.sh/documentation/environment-variables"}),
             !environ.empty())
    , environ(environ)
  {}

  SystemSanityResults::EnvironResult::EnvironResult(
    elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  bool
  SystemSanityResults::EnvironResult::_show(bool verbose) const
  {
    return !this->environ.empty();
  }

  void
  SystemSanityResults::EnvironResult::_print(
    std::ostream& out, bool verbose) const
  {
    if (this->show(verbose))
    {
      for (auto const& entry: this->environ)
        out << std::endl << "  " << entry.first << ": " << entry.second;
    }
  }

  void
  SystemSanityResults::EnvironResult::serialize(
    elle::serialization::Serializer& s)
  {
    Result::serialize(s);
    s.serialize("entries", this->environ);
  }

  SystemSanityResults::PermissionResult::PermissionResult(
    std::string const& name,
    bool exists, bool read, bool write)
    : Result(name, exists && read && write)
    , exists(exists)
    , read(read)
    , write(write)
  {}

  SystemSanityResults::PermissionResult::PermissionResult(
    elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  SystemSanityResults::PermissionResult::print(
    std::ostream& out, bool verbose) const
  {
    if (this->show(verbose))
    {
      out
        << "exists: " << result(this->exists)
        << ", readable: " << result(this->read)
        << ", writable: " << result(this->write);
    }
  }

  void
  SystemSanityResults::PermissionResult::serialize(
    elle::serialization::Serializer& s)
  {
    Result::serialize(s);
    s.serialize("exists", this->exists);
    s.serialize("read", this->read);
    s.serialize("write", this->write);
  }

  SystemSanityResults::FuseResult::FuseResult(bool sane)
    : Result("FUSE",
             true,
             (sane
              ? reporting::Result::Reason{}
              : reporting::Result::Reason{
               "Unable to find or interact with the driver. You won't be able "
               "to mount a filesystem interface through FUSE "
               "(visit https://infinit.sh/get-started for more details)"}),
             !sane)
  {}

  SystemSanityResults::FuseResult::FuseResult(
    elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  SystemSanityResults::FuseResult::_print(std::ostream& out, bool verbose) const
  {
    if (verbose)
    {
      if (this->sane() && !this->warning())
        out << " you will be able to mount a filesystem interface through FUSE";
    }
  }

  void
  SystemSanityResults::FuseResult::serialize(elle::serialization::Serializer& s)
  {
    Result::serialize(s);
  }


  SystemSanityResults::SystemSanityResults(bool only)
    : _only(only)
  {}

  SystemSanityResults::SystemSanityResults(elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  SystemSanityResults::print(std::ostream& out, bool verbose) const
  {
    if (!this->only())
      section(out, "System sanity");
    this->user.print(out, verbose);
    this->space_left.print(out, verbose);
    this->environ.print(out, verbose, false);
    ::print(out, "Permissions", this->permissions, verbose);
    this->fuse.print(out, verbose);
    out << std::endl;
  }

  bool
  SystemSanityResults::sane() const
  {
    return this->user.sane()
      && this->space_left.sane()
      && this->environ.sane()
      && this->fuse.sane()
      && ::sane(this->permissions);
  }

  bool
  SystemSanityResults::warning() const
  {
    return this->user.warning()
      || this->space_left.warning()
      || this->environ.warning()
      || this->fuse.warning()
      || ::warning(this->permissions);
  }

  void
  SystemSanityResults::serialize(elle::serialization::Serializer& s)
  {
    s.serialize("user name", this->user);
    s.serialize("space left", this->space_left);
    s.serialize("environment", this->environ);
    s.serialize("permissions", this->permissions);
    s.serialize("fuse", this->fuse);
    if (s.out())
    {
      bool sane = this->sane();
      s.serialize("sane", sane);
    }
  }

  ConnectivityResults::BeyondResult::BeyondResult(
    bool sane, reporting::Result::Reason const& r)
    : reporting::Result(
      elle::sprintf("Connection to %s", infinit::beyond()), sane, r)
  {}

  ConnectivityResults::BeyondResult::BeyondResult()
    : reporting::Result(elle::sprintf("Connection to %s", infinit::beyond()))
  {}

  void
  ConnectivityResults::BeyondResult::serialize(
    elle::serialization::Serializer& s)
  {
    Result::serialize(s);
  }

  ConnectivityResults::InterfaceResults::InterfaceResults(IPs const& ips)
    : Result("Local interfaces", !ips.empty())
    , entries(ips)
  {}

  ConnectivityResults::InterfaceResults::InterfaceResults(
    elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  ConnectivityResults::InterfaceResults::_print(
    std::ostream& out, bool verbose) const
  {
    if (this->show(verbose))
      for (auto const& entry: this->entries)
        out << std::endl << "  " << entry;
  }

  void
  ConnectivityResults::InterfaceResults::serialize(
    elle::serialization::Serializer& s)
  {
    Result::serialize(s);
    s.serialize("entries", this->entries);
  }

  ConnectivityResults::ProtocolResult::ProtocolResult(std::string const& name,
                                                      std::string const& address,
                                                      uint16_t local_port,
                                                      uint16_t remote_port,
                                                      bool internal)
    : Result(name, true)
    , address(address)
    , local_port(local_port)
    , remote_port(remote_port)
    , internal(internal)
  {}

  ConnectivityResults::ProtocolResult::ProtocolResult(std::string const& name,
                                                      std::string const& error)
    : Result(name, false, error)
  {}

  void
  ConnectivityResults::ProtocolResult::serialize(
    elle::serialization::Serializer& s)
  {
    Result::serialize(s);
    s.serialize("address", this->address);
    s.serialize("local_port", this->local_port);
    s.serialize("remote_port", this->remote_port);
    s.serialize("internal", this->internal);
  }

  void
  ConnectivityResults::ProtocolResult::print(
    std::ostream& out, bool verbose) const
  {
    if (this->show(verbose))
    {
      if (this->sane())
      {
        if (this->address)
          out << std::endl << "    Address: " << *this->address;
        if (this->local_port)
          out << std::endl << "    Local port: " << *this->local_port;
        if (this->remote_port)
          out << std::endl << "    Remote port: " << *this->remote_port;
        if (this->internal)
          out << std::endl << "    Internal: " << (*this->internal ? "Yes" : "No");
      }
      if (this->reason)
        print_reason(out << std::endl, *this->reason, 4);
    }
  }

  ConnectivityResults::NATResult::NATResult(bool cone)
    : Result("NAT", true)
    , cone(cone)
  {}

  ConnectivityResults::NATResult::NATResult(std::string const& error)
    : Result("NAT", false, error)
  {}

  ConnectivityResults::NATResult::NATResult(elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  ConnectivityResults::NATResult::_print(
    std::ostream& out, bool verbose) const
  {
    if (this->show(verbose) && this->sane())
      out << " OK (" << (this->cone ? "CONE" : "NOT CONE") << ")";
  }

  void
  ConnectivityResults::NATResult::serialize(elle::serialization::Serializer& s)
  {
    Result::serialize(s);
    s.serialize("cone", cone);
  }

  ConnectivityResults::UPnPResult::RedirectionResult::Address::Address(
    std::string host,
    uint16_t port)
    : host(host)
    , port(port)
  {}

  ConnectivityResults::UPnPResult::RedirectionResult::Address::Address(
    elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  ConnectivityResults::UPnPResult::RedirectionResult::Address::print(
    std::ostream& out) const
  {
    out << this->host << ":" << this->port;
  }

  void
  ConnectivityResults::UPnPResult::RedirectionResult::Address::serialize(
    elle::serialization::Serializer& s)
  {
    s.serialize("host", this->host);
    s.serialize("port", this->port);
  }

  ConnectivityResults::UPnPResult::RedirectionResult::RedirectionResult(
    std::string const& name,
    bool sane,
    Result::Reason const& reason)
    : Result(name, true, reason, !sane)
  {}

  ConnectivityResults::UPnPResult::RedirectionResult::RedirectionResult(
    elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  ConnectivityResults::UPnPResult::RedirectionResult::serialize(
    elle::serialization::Serializer& s)
  {
    Result::serialize(s);
    s.serialize("internal", this->internal);
    s.serialize("external", this->external);
  }

  void
  ConnectivityResults::UPnPResult::RedirectionResult::print(
    std::ostream& out, bool verbose) const
  {
    if (this->show(verbose))
    {
      status(out << std::endl << "  ", this->sane(), this->warning())
        << " " << this->name() << ": ";
      if (internal && external)
        out << "local endpoint (" << this->internal
            << ") successfully mapped (to " << this->external << ")";
      if (this->warning() && this->reason)
        out << std::endl << "    Reason: " << *this->reason;
    }
  }

  ConnectivityResults::UPnPResult::UPnPResult(bool available)
    : Result("UPnP", available)
    , available(available)
  {}

  ConnectivityResults::UPnPResult::UPnPResult(elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  ConnectivityResults::UPnPResult::_print(std::ostream& out, bool verbose) const
  {
    if (this->show(verbose))
    {
      if (this->external)
        out << " external IP address: " << this->external;
      for (auto const& redirection: redirections)
        redirection.print(out, verbose);
    }
  }

  bool
  ConnectivityResults::UPnPResult::sane() const
  {
    return Result::sane() && this->available && ::sane(this->redirections);
  }

  void
  ConnectivityResults::UPnPResult::sane(bool s)
  {
    Result::sane(s);
  }

  bool
  ConnectivityResults::UPnPResult::warning() const
  {
    return Result::warning() || ::warning(this->redirections);
  }

  void
  ConnectivityResults::UPnPResult::warning(bool w)
  {
    Result::warning(w);
  }

  void
  ConnectivityResults::UPnPResult::serialize(elle::serialization::Serializer& s)
  {
    Result::serialize(s);
    s.serialize("available", this->available);
    s.serialize("external", this->external);
    s.serialize("redirections", this->redirections);
  }

  ConnectivityResults::ConnectivityResults(bool only)
    : _only(only)
  {}

  ConnectivityResults::ConnectivityResults(elle::serialization::SerializerIn& s)
  {
    this->serialize(s);
  }

  void
  ConnectivityResults::print(std::ostream& out, bool verbose) const
  {
    if (!this->only())
      section(out, "Connectivity");
    this->beyond.print(out, verbose);
    this->interfaces.print(out, verbose, false);
    this->nat.print(out, verbose);
    this->upnp.print(out, verbose, false);
    ::print(out, "Protocols", this->protocols, verbose);
    out << std::endl;
  }

  bool
  ConnectivityResults::sane() const
  {
    return this->beyond.sane()
      && this->interfaces.sane()
      && this->nat.sane()
      && this->upnp.sane()
      && ::sane(this->protocols, false);
  }

  bool
  ConnectivityResults::warning() const
  {
    return this->beyond.warning()
      || this->interfaces.warning()
      || this->nat.warning()
      || this->upnp.warning()
      || ::warning(this->protocols);
  }

  void
  ConnectivityResults::serialize(elle::serialization::Serializer& s)
  {
    s.serialize("beyond", this->beyond);
    s.serialize("interfaces", this->interfaces);
    s.serialize("protocols", this->protocols);
    s.serialize("nat", this->nat);
    s.serialize("upnp", this->upnp);
    if (s.out())
    {
      bool sane = this->sane();
      s.serialize("sane", sane);
    }
  }

  All::All()
    : configuration_integrity(false)
    , system_sanity(false)
    , connectivity(false)
  {}

  All::All(elle::serialization::SerializerIn& s)
    : configuration_integrity(
      s.deserialize<ConfigurationIntegrityResults>("configuration_integrity"))
    , system_sanity(s.deserialize<SystemSanityResults>("system_sanity"))
    , connectivity(s.deserialize<ConnectivityResults>("connectivity"))
  {}

  void
  All::print(std::ostream& out, bool verbose) const
  {
    configuration_integrity.print(out, verbose);
    system_sanity.print(out, verbose);
    connectivity.print(out, verbose);
  }

  bool
  All::sane() const
  {
    return this->configuration_integrity.sane()
      && this->system_sanity.sane()
      && this->connectivity.sane();
  }

  bool
  All::warning() const
  {
    return this->configuration_integrity.warning()
      || this->system_sanity.warning()
      || this->connectivity.warning();
  }

  void
  All::serialize(elle::serialization::Serializer& s)
  {
    s.serialize("configuration_integrity", this->configuration_integrity);
    s.serialize("system_sanity", this->system_sanity);
    s.serialize("connectivity", this->connectivity);
    if (s.out())
    {
      bool sane = this->sane();
      s.serialize("sane", sane);
    }
  }
}

namespace
{
  // Return the infinit related environment.
  reporting::Environ
  infinit_related_environ()
  {
    using boost::algorithm::starts_with;
    return elle::os::environ([](auto const& k, auto const& v) {
        return ((starts_with(k, "INFINIT_") || starts_with(k, "ELLE_"))
                && v != banished_log_level);
      });
  }

  uint16_t
  get_upnp_tcp_port(boost::program_options::variables_map const& args)
  {
    if (auto port = optional<uint16_t>(args, "upnp_tcp_port"))
      return *port;
    else
      return infinit::cryptography::random::generate<uint16_t>(10000, 65535);
  }

  uint16_t
  get_upnp_udt_port(boost::program_options::variables_map const& args)
  {
    if (auto port = optional<uint16_t>(args, "upnp_udt_port"))
      return *port;
    else
      return infinit::cryptography::random::generate<uint16_t>(10000, 65535);
  }

  std::string
  get_connectivity_server_address(
    boost::program_options::variables_map const& args)
  {
    if (auto address = optional<std::string>(args, "server"))
      return *address;
    else
      return "192.241.139.66";
  }

  void
  _connectivity(boost::program_options::variables_map const& args,
                reporting::ConnectivityResults& results)
  {
    ELLE_TRACE("contact beyond")
    {
      try
      {
        reactor::http::Request r(infinit::beyond(), reactor::http::Method::GET, {10_sec});
        reactor::wait(r);
        auto status = (r.status() == reactor::http::StatusCode::OK);
        if (status)
          results.beyond = {status};
        else
          results.beyond = {status, elle::sprintf("%s", r.status())};
      }
      catch (reactor::http::RequestError const&)
      {
        results.beyond =
          {false, elle::sprintf("Couldn't connect to %s", infinit::beyond())};
      }
      catch (elle::Error const&)
      {
        results.beyond = {false, elle::exception_string()};
      }
    }
    auto public_ips = std::vector<std::string>{};
    ELLE_TRACE("list interfaces")
    {
      auto interfaces = elle::network::Interface::get_map(
        elle::network::Interface::Filter::no_loopback);
      for (auto i: interfaces)
        if (!i.second.ipv4_address.empty())
          public_ips.emplace_back(i.second.ipv4_address);
      results.interfaces = {public_ips};
    }
    // XXX: This should be nat.infinit.sh or something.
    std::string host = get_connectivity_server_address(args);
    uint16_t port = 5456;
    auto run = [&] (std::string const& name,
                    std::function<reactor::connectivity::Result (
                      std::string const& host,
                      uint16_t port)> const& function,
                    int deltaport = 0)
      {
        static const reactor::Duration timeout = 3_sec;
        ELLE_TRACE("connect using %s to %s:%s", name, host, port + deltaport);
        std::string result = elle::sprintf("  %s: ", name);
        try
        {
          reactor::TimeoutGuard guard(timeout);
          auto address = function(host, port + deltaport);
          bool external = std::find(
            public_ips.begin(), public_ips.end(), address.host
            ) == public_ips.end();
          store(results.protocols, name, host, address.local_port,
                address.remote_port, !external);
        }
        catch (reactor::Terminate const&)
        {
          throw;
        }
        catch (reactor::network::ResolutionError const& error)
        {
          store(results.protocols, name,
                elle::sprintf("Couldn't connect to %s", error.host()));
        }
        catch (reactor::Timeout const&)
        {
          store(results.protocols, name,
                elle::sprintf("Couldn't connect after 3 seconds"));
        }
        catch (...)
        {
          store(results.protocols, name, elle::exception_string());
        }
      };
    run("TCP", reactor::connectivity::tcp);
    run("UDP", reactor::connectivity::udp);
    run("UTP",
        std::bind(reactor::connectivity::utp,
                  std::placeholders::_1,
                  std::placeholders::_2,
                  0),
        1);
    run("UTP (XOR)",
        std::bind(reactor::connectivity::utp,
                  std::placeholders::_1,
                  std::placeholders::_2,
                  0xFF),
        2);
    run("RDV UTP",
        std::bind(reactor::connectivity::rdv_utp,
                  std::placeholders::_1,
                  std::placeholders::_2,
                  0),
        1);
    run("RDV UTP (XOR)",
        std::bind(reactor::connectivity::rdv_utp,
                  std::placeholders::_1,
                  std::placeholders::_2,
                  0xFF),
        2);
    ELLE_TRACE("NAT")
    {
      try
      {
        auto nat = reactor::connectivity::nat(host, port);
        // Super uglY.
        auto cone = nat.find("NOT_CONE") == std::string::npos &&
          nat.find("CONE") != std::string::npos;
        results.nat = {cone};
      }
      catch (reactor::Terminate const&)
      {
        throw;
      }
      catch (...)
      {
        results.nat = {elle::exception_string()};
      }
    }
    ELLE_TRACE("UPnP")
    {
      auto upnp = reactor::network::UPNP::make();
      try
      {
        results.upnp.sane(true);
        results.upnp.available = false;
        upnp->initialize();
        results.upnp.available = upnp->available();
        results.upnp.warning(true);
        results.upnp.external = upnp->external_ip();
        results.upnp.warning(false);
        using Address =
          reporting::ConnectivityResults::UPnPResult::RedirectionResult::Address;
        auto redirect = [&] (reactor::network::Protocol protocol,
                             uint16_t port)
          {
            auto type = elle::sprintf("%s", protocol);
            auto& res = store(results.upnp.redirections, type);
            try
            {
              auto convert = [] (std::string const& port)
                {
                  int i = std::stoi(port);
                  uint16_t narrow = i;
                  if (narrow != i)
                    elle::err("invalid port: %s", i);
                  return narrow;
                };
              auto pm = upnp->setup_redirect(protocol, port);
              res.sane(true);
              res.warning(false);
              res.internal = Address{pm.internal_host, convert(pm.internal_port)};
              res.external = Address{pm.external_host, convert(pm.external_port)};
            }
            catch (reactor::Terminate const&)
            {
              throw;
            }
            catch (...)
            {
              res = {type, false, elle::exception_string()};
            }
          };
        redirect(reactor::network::Protocol::tcp, get_upnp_tcp_port(args));
        redirect(reactor::network::Protocol::udt, get_upnp_udt_port(args));
      }
      catch (reactor::Terminate const&)
      {
        throw;
      }
      catch (...)
      {
        // UPnP is always considered sane.
        results.upnp.warning(true);
        results.upnp.reason = elle::exception_string();
      }
    }
  }

  // Return the current user permissions on a given path.
  std::pair<bool, bool>
  permissions(bfs::path const& path)
  {
    if (!bfs::exists(path))
      elle::err("%s doesn't exist", path);
    auto s = bfs::status(path);
    bool read = (s.permissions() & bfs::perms::owner_read)
      && (s.permissions() & bfs::perms::others_read);
    bool write = (s.permissions() & bfs::perms::owner_write)
      && (s.permissions() & bfs::perms::others_write);
    if (!read)
    {
      boost::system::error_code code;
      bfs::recursive_directory_iterator it(path, code);
      if (!code)
        read = true;
    }
    if (!write)
    {
      auto p = path / ".tmp";
      boost::system::error_code code;
      elle::SafeFinally remove([&] { bfs::remove(p, code); });
      bfs::ofstream f;
      {
        f.open(p, std::ios_base::out);
        f << "test";
        f.flush();
      }
      write = f.good();
    }
    return std::make_pair(read, write);
  }

  // Return the permissions as a string:
  // r, w, rw, None, ...
  std::string
  permissions_string(bfs::path const& path)
  {
    try
    {
      auto perms = permissions(path);
      std::string res = "";
      if (perms.first)
        res += "r";
      if (perms.second)
        res += "w";
      if (res.empty())
        res = "None";
      return res;
    }
    catch (...)
    {
      return elle::exception_string();
    }
  }

  std::pair<bool, std::string>
  has_permission(bfs::path const& path,
                 bool mandatory = true)
  {
    auto res = permissions_string(path);
    auto good = (res == "rw");
    auto sane = (good || !mandatory);
    return std::make_pair(sane, res);
  }

  bool
  fuse(bool /*verbose*/)
  {
    try
    {
      struct NoOp : reactor::filesystem::Operations
      {
        std::shared_ptr<reactor::filesystem::Path>
        path(std::string const& path) override
        {
          return nullptr;
        }
      };

      reactor::filesystem::FileSystem f(std::make_unique<NoOp>(), false);
      elle::filesystem::TemporaryDirectory d;
      f.mount(d.path(), {});
      f.unmount();
      f.kill();
      return true;
    }
    catch (...)
    {
      return false;
    }
  }

  void
  _system_sanity(boost::program_options::variables_map const& args,
                 reporting::SystemSanityResults& result)
  {
    result.fuse = {fuse(false)};
    ELLE_TRACE("user name")
      try
      {
        auto self_name = self_user_name(args);
        result.user = {self_name};
      }
      catch (...)
      {
        result.user = {};
      }
    ELLE_TRACE("calculate space left")
    {
      size_t min = 50 * 1024 * 1024;
      double min_ratio = 0.02;
      auto f = bfs::space(infinit::xdg_data_home());
      result.space_left = {min, min_ratio, f.available, f.capacity};
    }
    ELLE_TRACE("look for Infinit related environment")
    {
      auto env = infinit_related_environ();
      result.environ = {env};
    }
    ELLE_TRACE("check permissions")
    {
      auto test_permissions = [&] (bfs::path const& path)
        {
          if (bfs::exists(path))
          {
            auto perms = permissions(path);
            store(result.permissions, path.string(), true, perms.first,
                  perms.second);
          }
          else
            store(result.permissions, path.string(), false, false, false);
        };
      test_permissions(elle::system::home_directory());
      test_permissions(infinit::xdg_cache_home());
      test_permissions(infinit::xdg_config_home());
      test_permissions(infinit::xdg_data_home());
      test_permissions(infinit::xdg_state_home());
    }
  }

  template <typename T>
  std::map<std::string, std::pair<T, bool>>
  parse(std::vector<T> container)
  {
    std::map<std::string, std::pair<T, bool>> output;
    for (auto& item: container)
    {
      auto name = item.name;
      output.emplace(std::piecewise_construct,
                     std::forward_as_tuple(name),
                     std::forward_as_tuple(std::move(item), false));
    }
    return output;
  }

  template <typename T>
  std::map<std::string, std::pair<std::unique_ptr<T>, bool>>
  parse(std::vector<std::unique_ptr<T>> container)
  {
    std::map<std::string, std::pair<std::unique_ptr<T>, bool>> output;
    for (auto& item: container)
    {
      auto name = item->name;
      output.emplace(std::piecewise_construct,
                     std::forward_as_tuple(name),
                     std::forward_as_tuple(std::move(item), false));
    }
    return output;
  }

  template <typename ExpectedType>
  void
  load(bfs::path const& path, std::string const& type)
  {
    bfs::ifstream i;
    ifnt._open_read(i, path, type, path.filename().string());
    infinit::load<ExpectedType>(i);
  }

  void
  _configuration_integrity(boost::program_options::variables_map const& args,
                           reporting::ConfigurationIntegrityResults& results)
  {
    auto users = parse(ifnt.users_get());
    auto aws_credentials = ifnt.credentials_aws();
    auto gcs_credentials = ifnt.credentials_gcs();
    auto storage_resources = parse(ifnt.storages_get());
    auto drives = parse(ifnt.drives_get());
    auto volumes = parse(ifnt.volumes_get());
    auto user = boost::optional<infinit::User>{};
    try
    {
      user = self_user(ifnt, args);
    }
    catch (...)
    {
    }
    auto networks = parse(ifnt.networks_get(user));
    ELLE_TRACE("verify storage resources")
      for (auto& elem: storage_resources)
      {
        auto& storage = elem.second.first;
        auto& status = elem.second.second;
        if (auto s3config = dynamic_cast<infinit::storage::S3StorageConfig const*>(
              storage.get()))
        {
          auto it =
            std::find_if(
              aws_credentials.begin(),
              aws_credentials.end(),
              [&s3config] (auto const& credentials)
              {
#define COMPARE(field) (credentials->field == s3config->credentials.field())
                return COMPARE(access_key_id) && COMPARE(secret_access_key);
#undef COMPARE
              });
          status = (it != aws_credentials.end());
        if (status)
          store(results.storage_resources, storage->name, status, "S3");
        else
          store(results.storage_resources, storage->name, status, "S3", std::string("Missing credentials"));
        }
        if (auto fsconfig = dynamic_cast<infinit::storage::FilesystemStorageConfig const*>(storage.get()))
        {
          auto perms = has_permission(fsconfig->path);
          status = perms.first;
          store(results.storage_resources, storage->name, status, "filesystem", perms.second);
        }
        if (auto gcsconfig = dynamic_cast<infinit::storage::GCSConfig const*>(storage.get()))
        {
          auto it =
            std::find_if(
              gcs_credentials.begin(),
              gcs_credentials.end(),
              [&gcsconfig] (std::unique_ptr<infinit::OAuthCredentials, std::default_delete<infinit::Credentials>> const& credentials)
              {
                return credentials->refresh_token == gcsconfig->refresh_token;
              });
          status = (it != gcs_credentials.end());
          if (status)
            store(results.storage_resources, storage->name, status, "GCS");
          else
            store(results.storage_resources, storage->name, status, "GCS", std::string{"Missing credentials"});
        }
#ifndef INFINIT_WINDOWS
        if (/* auto ssh = */
          dynamic_cast<infinit::storage::SFTPStorageConfig const*>(storage.get()))
        {
          // XXX:
        }
#endif
      }
    ELLE_TRACE("verify networks")
      for (auto& elem: networks)
      {
        auto const& network = elem.second.first;
        auto& status = elem.second.second;
        auto storage_names = std::vector<std::string>{};
        bool linked = network.model != nullptr;
        if (linked && network.model->storage)
        {
          if (auto strip = dynamic_cast<infinit::storage::StripStorageConfig*>(
                network.model->storage.get()))
            for (auto const& s: strip->storage)
              storage_names.emplace_back(s->name);
          else
            storage_names.emplace_back(network.model->storage->name);
        }
        auto faulty = std::vector<std::string>{};
        status = storage_names.empty() || std::all_of(
          storage_names.begin(),
          storage_names.end(),
          [&] (std::string const& name) -> bool
          {
            auto it = storage_resources.find(name);
            auto res = (it != storage_resources.end() && it->second.second);
            if (!res)
              faulty.emplace_back(name);
            return res;
          });
        if (status)
          store(results.networks, network.name, status, boost::none,
                boost::none, linked);
        else
          store(results.networks, network.name, status, faulty,
                boost::none, linked);
      }
    ELLE_TRACE("verify volumes")
      for (auto& elems: volumes)
      {
        auto const& volume = elems.second.first;
        auto& status = elems.second.second;
        auto network = networks.find(volume.network);
        auto network_presents = network != networks.end();
        status = network_presents && network->second.second;
        if (status)
          store(results.volumes, volume.name, status);
        else
          store(results.volumes, volume.name, status, volume.network);
      }
    ELLE_TRACE("verify drives")
      for (auto& elems: drives)
      {
        auto const& drive = elems.second.first;
        auto& status = elems.second.second;
        auto volume = volumes.find(drive.volume);
        auto volume_presents = volume != volumes.end();
        auto volume_ok = volume_presents && volume->second.second;
        auto network = networks.find(drive.network);
        auto network_presents = network != networks.end();
        auto network_ok = network_presents && network->second.second;
        status = network_ok && volume_ok;
        if (status)
          store(results.drives, drive.name, status);
        else
          store(results.drives, drive.name, status, drive.volume);
      }

    namespace bf = bfs;
    auto& leftovers = results.leftovers;
    auto path_contains_file = [](bfs::path dir, bfs::path file) -> bool
      {
        file.remove_filename();
        auto dir_len = std::distance(dir.begin(), dir.end());
        auto file_len = std::distance(file.begin(), file.end());
        if (dir_len > file_len)
          return false;
        return std::equal(dir.begin(), dir.end(), file.begin());
      };
    for (auto it = bfs::recursive_directory_iterator(infinit::xdg_data_home());
         it != bfs::recursive_directory_iterator();
         ++it)
      if (is_regular_file(it->status()) && !infinit::is_hidden_file(it->path()))
      {
        try
        {
          // Share.
          if (path_contains_file(ifnt._network_descriptors_path(), it->path()))
            load<infinit::NetworkDescriptor>(it->path(), "network_descriptor");
          else if (path_contains_file(ifnt._networks_path(), it->path()))
            load<infinit::Network>(it->path(), "network");
          else if (path_contains_file(ifnt._volumes_path(), it->path()))
            load<infinit::Volume>(it->path(), "volume");
          else if (path_contains_file(ifnt._drives_path(), it->path()))
            load<infinit::Drive>(it->path(), "drive");
          else if (path_contains_file(ifnt._passports_path(), it->path()))
            load<infinit::Passport>(it->path(), "passport");
          else if (path_contains_file(ifnt._users_path(), it->path()))
            load<infinit::User>(it->path(), "users");
          else if (path_contains_file(ifnt._storages_path(), it->path()))
            load<std::unique_ptr<infinit::storage::StorageConfig>>(it->path(), "storage");
          else if (path_contains_file(ifnt._credentials_path(), it->path()))
            load<std::unique_ptr<infinit::Credentials>>(it->path(), "credentials");
          else if (path_contains_file(infinit::xdg_data_home() / "blocks", it->path()))
            {}
          else if (path_contains_file(infinit::xdg_data_home() / "ui", it->path()))
            {}
          else
            store(leftovers, it->path().string());
        }
        catch (...)
        {
          store(leftovers, it->path().string(), elle::exception_string());
        }
      }
    for (auto it = bfs::recursive_directory_iterator(infinit::xdg_cache_home());
         it != bfs::recursive_directory_iterator();
         ++it)
      if (is_regular_file(it->status()) && !infinit::is_hidden_file(it->path()))
      {
        try
        {
          if (!path_contains_file(ifnt._user_avatar_path(), it->path())
              && !path_contains_file(ifnt._drive_icon_path(), it->path()))
            store(leftovers, it->path().string());
        }
        catch (...)
        {
          store(leftovers, it->path().string(), elle::exception_string());
        }
      }
    for (auto it = bfs::recursive_directory_iterator(infinit::xdg_state_home());
         it != bfs::recursive_directory_iterator();
         ++it)
      if (is_regular_file(it->status()) && !infinit::is_hidden_file(it->path()))
      {
        try
        {
          if (path_contains_file(infinit::xdg_state_home() / "cache", it->path()));
          else if (it->path() == infinit::xdg_state_home() / "critical.log");
          else if (it->path().filename() == "root_block")
          {
            // The root block path is:
            // <qualified_network_name>/<qualified_volume_name>/root_block
            auto network_volume = it->path().parent_path().lexically_relative(
              infinit::xdg_state_home());
            auto name = network_volume.begin();
            std::advance(name, 1); // <network_name>
            auto network = *network_volume.begin() / *name;
            std::advance(name, 1);
            auto volume_owner = name;
            std::advance(name, 1); // <volume_name>
            auto volume = *volume_owner / *name;
            if (volumes.find(volume.string()) == volumes.end())
              store(leftovers, it->path().string(),
                    reporting::Result::Reason{"volume is gone"});
            if (networks.find(network.string()) == networks.end())
              store(leftovers, it->path().string(),
                    reporting::Result::Reason{"network is gone"});
          }
          else
            store(leftovers, it->path().string());
        }
        catch (...)
        {
          store(leftovers, it->path().string(), elle::exception_string());
        }
      }
  }

  void
  report_error(std::ostream& out, bool sane, bool warning = false)
  {
    if (!sane)
      elle::err("Please refer to each individual error message. "
                "If you cannot figure out how to fix your issues, "
                "please visit https://infinit.sh/faq.");
    else if (!script_mode)
    {
      if (warning)
        out <<
          "Doctor has detected minor issues but nothing that should prevent "
          "Infinit from working.";
      else
        out << "All good, everything should work.";
      out << std::endl;
    }
  }

  template <typename Report>
  void
  output(std::ostream& out,
         Report const& results,
         bool verbose)
  {
    if (script_mode)
      infinit::save(out, results);
    else
      results.print(out, verbose);
  }
}

COMMAND(configuration_integrity)
{
  reporting::ConfigurationIntegrityResults results;
  _configuration_integrity(args, results);
  output(std::cout, results, flag(args, "verbose"));
  report_error(std::cout, results.sane(), results.warning());
}

COMMAND(connectivity)
{
  reporting::ConnectivityResults results;
  _connectivity(args, results);
  output(std::cout, results, flag(args, "verbose"));
  report_error(std::cout, results.sane(), results.warning());
}

COMMAND(system_sanity)
{
  reporting::SystemSanityResults results;
  _system_sanity(args, results);
  output(std::cout, results, flag(args, "verbose"));
  report_error(std::cout, results.sane(), results.warning());
}

COMMAND(networking)
{
  bool server = args.count("host") == 0;
  auto v = ::version;
  if (infinit::compatibility_version)
    v = *infinit::compatibility_version;
  if (server)
  {
    elle::fprintf(std::cout, "Server mode (version: %s):", v) << std::endl;
    auto servers = infinit::networking::Servers(args, v);
    reactor::sleep();
  }
  else
  {
    elle::fprintf(std::cout, "Client mode (version: %s):", v) << std::endl;
    infinit::networking::perfom(mandatory<std::string>(args, "host"), args, v);
  }
}

COMMAND(run_all)
{
  reporting::All a;
  _system_sanity(args, a.system_sanity);
  _configuration_integrity(args, a.configuration_integrity);
  _connectivity(args, a.connectivity);
  output(std::cout, a, flag(args, "verbose"));
  report_error(std::cout, a.sane(), a.warning());
}

int
main(int argc, char** argv)
{
  using boost::program_options::value;
  using boost::program_options::bool_switch;
  Mode::OptionDescription upnp_tcp_port =
    { "upnp_tcp_port", value<uint16_t>(),
      "port to try to get an tcp upnp connection on" };
  Mode::OptionDescription upnp_udt_port =
    { "upnp_udt_port", value<uint16_t>(),
      "port to try to get an udt upnp connection on" };
  Mode::OptionDescription connectivity_server =
    { "server", value<std::string>(),
      "connectivity server address (default = 192.241.139.66)"};
  Mode::OptionDescription verbose =
    { "verbose,v", bool_switch(), "output everything" };
  Mode::OptionDescription do_not_use_color =
    { "no-color", bool_switch(&no_color), "don't use colored output" };
  Mode::OptionDescription protocol = {
    "protocol", value<std::string>(),
    "RPC protocol to use: tcp,utp,all (default: all)"
  };
  Mode::OptionDescription packet_size =
    { "packet_size,s", value<elle::Buffer::Size>(),
      "size of the packet to send (client only)" };
  Mode::OptionDescription packets_count =
    { "packets_count,n", value<int64_t>(),
      "number of packets to exchange (client only)" };
  Mode::OptionDescription host =
    { "host,H", value<std::string>(),
      "the host to connect to (if not specified, you are considered server)" };
  Mode::OptionDescription port =
    { "port,p", value<uint16_t>(),
      "port to perform tests on (if unspecified,"
      "\n  --tcp_port = port,"
      "\n  --utp_port = port + 1,"
      "\n  --xored_utp_port = port + 2)" };
  Mode::OptionDescription tcp_port =
    { "tcp_port,t", value<uint16_t>(), "port to perform tcp tests on"};
  Mode::OptionDescription utp_port =
    { "utp_port,u", value<uint16_t>(),
      "port to perform utp tests on. (if unspecified,"
      "\n  --xored_utp_port = utp_port + 1)" };
  Mode::OptionDescription xored_utp_port =
    { "xored_utp_port,x", value<uint16_t>(), "port to perform xored utp tests on"};
  Mode::OptionDescription xored =
    { "xored,X", value<std::string>(),
      "performs test applying a 0xFF xor on the utp traffic, value=yes,no,both"};
  Mode::OptionDescription mode =
    { "mode,m", value<std::string>(),
      "mode to use: upload,download,all (default: all) (client only)" };

  Modes modes {
    {
      "all",
      "Perform all possible checks",
      &run_all,
      "",
      {
        do_not_use_color,
        verbose,
      }
    },
    {
      "system",
      "Perform sanity checks on your system",
      &system_sanity,
      "",
      {
        do_not_use_color,
        verbose
      }
    },
    {
      "configuration",
      "Perform integrity checks on the Infinit configuration files",
      &configuration_integrity,
      "",
      {
        do_not_use_color,
        verbose
      }
    },
    {
      "connectivity",
      "Perform connectivity checks",
      &connectivity,
      "",
      {
        do_not_use_color,
        upnp_tcp_port,
        upnp_udt_port,
        connectivity_server,
        verbose,
      }
    },
    {
      "networking",
      "Perform networking speed tests between nodes",
      &networking,
      "",
      {
        mode,
        protocol,
        packet_size,
        packets_count,
        host,
        port,
        tcp_port,
        utp_port,
        xored_utp_port,
        do_not_use_color,
        verbose
      }
    }
  };
  // Disable reactor.network.UTPSocket annoying log.
  auto env = banished_log_level;
  if (elle::os::inenv("ELLE_LOG_LEVEL"))
    env += ",";
  elle::os::setenv(
    "ELLE_LOG_LEVEL",
    elle::sprintf("%s%s", env, elle::os::getenv("ELLE_LOG_LEVEL", "")), true);
  return infinit::main("Infinit diagnostic utility", modes, argc, argv,
                       std::string("path"), boost::none);
}
